// VoiceRecorder.cpp - Voice recording implementation for Dialectic

#include "VoiceRecorder.h"
#include "HTTPManager.h"
#include "Config.h"
#include "GameThreadDispatcher.h"
#include "Misc.h"
#include "RuntimeGeneration.h"
#include "TaskManager.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmsystem.h>
#include <Functiondiscoverykeys_devpkey.h>
#include <thread>
#include <vector>
#include <atomic>
#include <sstream>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <fstream>
#include <memory>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "ole32.lib")

// Forward declare Log
void Log(const char* fmt, ...);

namespace VoiceRecorder {

// Recording state
static std::atomic<bool> g_isRecording(false);
static std::thread g_recordThread;
static std::atomic<bool> g_openMicMonitoringActive(false);
static std::atomic<bool> g_openMicMuted(false);
static std::atomic<double> g_openMicSensitivity(1000.0);
static std::atomic<double> g_openMicEndDelaySeconds(1.0);
static std::thread g_openMicThread;
static std::atomic<bool> g_recordWorkerActive(false);
static std::atomic<bool> g_openMicWorkerActive(false);
static std::atomic<std::uint64_t> g_serviceHeartbeatMs(0);
static std::atomic<std::uint64_t> g_captureWorkersStarted(0);
static std::atomic<std::uint64_t> g_captureWorkersCompleted(0);
static std::atomic<std::uint64_t> g_monitorWorkersStarted(0);
static std::atomic<std::uint64_t> g_monitorWorkersCompleted(0);

static std::uint64_t SteadyNowMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

static void RecordServiceHeartbeat() {
    g_serviceHeartbeatMs.store(SteadyNowMs(), std::memory_order_release);
}

class WorkerActivityGuard {
public:
    WorkerActivityGuard(std::atomic<bool>& active, std::atomic<std::uint64_t>& started,
                        std::atomic<std::uint64_t>& completed)
        : active_(active), completed_(completed) {
        active_.store(true, std::memory_order_release);
        started.fetch_add(1, std::memory_order_relaxed);
        RecordServiceHeartbeat();
    }

    ~WorkerActivityGuard() {
        RecordServiceHeartbeat();
        active_.store(false, std::memory_order_release);
        completed_.fetch_add(1, std::memory_order_relaxed);
    }

private:
    std::atomic<bool>& active_;
    std::atomic<std::uint64_t>& completed_;
};

static WAVEFORMATEX CreateRecordingWaveFormat() {
    WAVEFORMATEX wfx = {};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 1;
    wfx.nSamplesPerSec = 16000;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = wfx.wBitsPerSample * wfx.nChannels / 8;
    wfx.nAvgBytesPerSec = wfx.nBlockAlign * wfx.nSamplesPerSec;
    wfx.cbSize = 0;
    return wfx;
}

static std::string ToLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

static UINT_PTR ResolveCaptureDeviceId() {
    std::string wanted = Config::voiceRecordingPreferredDeviceName;
    const std::string displayPrefix = "Current Device: ";
    if (wanted.rfind(displayPrefix, 0) == 0) {
        wanted.erase(0, displayPrefix.size());
    }

    const std::string wantedLower = ToLower(wanted);
    if (wantedLower.empty() || wantedLower == "windows default") {
        return WAVE_MAPPER;
    }

    const UINT count = waveInGetNumDevs();
    for (UINT i = 0; i < count; ++i) {
        WAVEINCAPSA caps = {};
        if (waveInGetDevCapsA(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR || !caps.szPname[0]) {
            continue;
        }
        const std::string candidateLower = ToLower(caps.szPname);
        if (candidateLower.find(wantedLower) != std::string::npos ||
            wantedLower.find(candidateLower) != std::string::npos) {
            Log("VoiceRecorder: Automatically matched preferred capture device '%s' to id %u (%s)",
                wanted.c_str(), i, caps.szPname);
            return static_cast<UINT_PTR>(i);
        }
    }

    Log("VoiceRecorder: Preferred capture device '%s' was unavailable; using Windows default",
        wanted.c_str());
    return WAVE_MAPPER;
}

static MMRESULT OpenCaptureInput(HWAVEIN* hWaveIn, const WAVEFORMATEX* wfx) {
    const UINT_PTR deviceId = ResolveCaptureDeviceId();
    // Let Windows convert the default endpoint's native format to 16 kHz mono.
    // Some USB headset drivers accept a direct 16 kHz stream but return silence.
    MMRESULT result = waveInOpen(hWaveIn, deviceId, wfx, 0, 0, CALLBACK_NULL);
    if (result != MMSYSERR_NOERROR) {
        Log("VoiceRecorder: Converted capture open failed for device %u (error: %d), trying direct mode",
            static_cast<unsigned int>(deviceId), result);
        result = waveInOpen(hWaveIn, deviceId, wfx, 0, 0, CALLBACK_NULL | WAVE_FORMAT_DIRECT);
    }
    if (result != MMSYSERR_NOERROR && deviceId != WAVE_MAPPER) {
        Log("VoiceRecorder: Preferred capture device %u failed to open; falling back to Windows default",
            static_cast<unsigned int>(deviceId));
        result = waveInOpen(hWaveIn, WAVE_MAPPER, wfx, 0, 0, CALLBACK_NULL);
    }
    return result;
}

static void CloseWaveInputSafely(HWAVEIN& hWaveIn, WAVEHDR* headers, std::size_t headerCount,
                                 const char* context) {
    if (!hWaveIn) {
        return;
    }

    MMRESULT result = waveInStop(hWaveIn);
    if (result != MMSYSERR_NOERROR) {
        Log("VoiceRecorder: %s waveInStop failed (error: %d)", context, result);
    }

    // waveInStop can leave buffers queued. Reset synchronously returns every
    // queued buffer before their stack-backed storage is released.
    result = waveInReset(hWaveIn);
    if (result != MMSYSERR_NOERROR) {
        Log("VoiceRecorder: %s waveInReset failed (error: %d)", context, result);
    }

    for (std::size_t i = 0; i < headerCount; ++i) {
        WAVEHDR& header = headers[i];
        if (!(header.dwFlags & WHDR_PREPARED)) {
            continue;
        }

        MMRESULT unprepareResult = waveInUnprepareHeader(hWaveIn, &header, sizeof(header));
        for (int retry = 0; unprepareResult == WAVERR_STILLPLAYING && retry < 5; ++retry) {
            waveInReset(hWaveIn);
            Sleep(1);
            unprepareResult = waveInUnprepareHeader(hWaveIn, &header, sizeof(header));
        }
        if (unprepareResult != MMSYSERR_NOERROR) {
            Log("VoiceRecorder: %s buffer %zu unprepare failed (error: %d)",
                context, i, unprepareResult);
        }
    }

    result = waveInClose(hWaveIn);
    if (result != MMSYSERR_NOERROR) {
        Log("VoiceRecorder: %s waveInClose failed (error: %d)", context, result);
    }
    hWaveIn = nullptr;
}

static bool PrepareCaptureBuffer(HWAVEIN hWaveIn, WAVEHDR& header, char* buffer,
                                 DWORD bufferSize, std::size_t index, const char* context) {
    header.lpData = buffer;
    header.dwBufferLength = bufferSize;

    MMRESULT result = waveInPrepareHeader(hWaveIn, &header, sizeof(header));
    if (result != MMSYSERR_NOERROR) {
        Log("VoiceRecorder: %s buffer %zu prepare failed (error: %d)", context, index, result);
        return false;
    }

    result = waveInAddBuffer(hWaveIn, &header, sizeof(header));
    if (result != MMSYSERR_NOERROR) {
        Log("VoiceRecorder: %s buffer %zu queue failed (error: %d)", context, index, result);
        return false;
    }
    return true;
}

static std::string TrimText(const std::string& value) {
    const char* whitespace = " \t\r\n";
    const size_t start = value.find_first_not_of(whitespace);
    if (start == std::string::npos) {
        return "";
    }
    const size_t end = value.find_last_not_of(whitespace);
    return value.substr(start, end - start + 1);
}

static std::string WideToUtf8(const wchar_t* value) {
    if (!value || !value[0]) return "";
    const int required = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (required <= 1) return "";
    std::string converted(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, converted.data(), required, nullptr, nullptr);
    converted.resize(static_cast<std::size_t>(required - 1));
    return converted;
}

static std::string GetEndpointFriendlyName(IMMDevice* endpoint) {
    if (!endpoint) return "Unavailable";
    IPropertyStore* properties = nullptr;
    PROPVARIANT friendlyName;
    PropVariantInit(&friendlyName);
    std::string result = "Unavailable";
    if (SUCCEEDED(endpoint->OpenPropertyStore(STGM_READ, &properties)) && properties &&
        SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &friendlyName)) &&
        friendlyName.vt == VT_LPWSTR) {
        const std::string converted = WideToUtf8(friendlyName.pwszVal);
        if (!converted.empty()) result = converted;
    }
    PropVariantClear(&friendlyName);
    if (properties) properties->Release();
    return result;
}

static std::string GetEndpointId(IMMDevice* endpoint) {
    if (!endpoint) return "";
    LPWSTR endpointId = nullptr;
    if (FAILED(endpoint->GetId(&endpointId)) || !endpointId) return "";
    const std::string result = WideToUtf8(endpointId);
    CoTaskMemFree(endpointId);
    return result;
}

static std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int required = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (required <= 1) return {};
    std::wstring converted(static_cast<std::size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, converted.data(), required);
    converted.resize(static_cast<std::size_t>(required - 1));
    return converted;
}

struct WasapiEndpointCandidate {
    IMMDevice* endpoint{nullptr};
    std::string id;
    std::string name;
    int priority{3};
};

struct WasapiCaptureSession {
    IAudioClient* audioClient{nullptr};
    IAudioCaptureClient* captureClient{nullptr};
    std::string id;
    std::string name;
    std::vector<short> audio;
    bool started{false};
};

static void CloseWasapiSession(WasapiCaptureSession& session) {
    if (session.started && session.audioClient) session.audioClient->Stop();
    if (session.captureClient) session.captureClient->Release();
    if (session.audioClient) session.audioClient->Release();
    session.captureClient = nullptr;
    session.audioClient = nullptr;
    session.started = false;
}

static std::vector<WasapiEndpointCandidate> EnumerateWasapiCaptureEndpoints(
    IMMDeviceEnumerator* enumerator) {
    std::vector<WasapiEndpointCandidate> candidates;
    if (!enumerator) return candidates;

    std::string defaultId;
    IMMDevice* defaultEndpoint = nullptr;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eCapture, eMultimedia, &defaultEndpoint)) &&
        defaultEndpoint) {
        defaultId = GetEndpointId(defaultEndpoint);
        defaultEndpoint->Release();
    }

    std::string preferredName = Config::voiceRecordingPreferredDeviceName;
    const std::string displayPrefix = "Current Device: ";
    if (preferredName.rfind(displayPrefix, 0) == 0) {
        preferredName.erase(0, displayPrefix.size());
    }
    const std::string preferredLower = ToLower(preferredName);
    const std::string detectedId = Config::voiceRecordingDetectedEndpointId;

    IMMDeviceCollection* collection = nullptr;
    if (FAILED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection)) ||
        !collection) {
        return candidates;
    }

    UINT count = 0;
    collection->GetCount(&count);
    candidates.reserve(count);
    for (UINT i = 0; i < count; ++i) {
        IMMDevice* endpoint = nullptr;
        if (FAILED(collection->Item(i, &endpoint)) || !endpoint) continue;

        WasapiEndpointCandidate candidate;
        candidate.endpoint = endpoint;
        candidate.id = GetEndpointId(endpoint);
        candidate.name = GetEndpointFriendlyName(endpoint);
        const std::string candidateLower = ToLower(candidate.name);
        if (!detectedId.empty() && candidate.id == detectedId) {
            candidate.priority = 0;
        } else if (!preferredLower.empty() && preferredLower != "windows default" &&
                   (candidateLower.find(preferredLower) != std::string::npos ||
                    preferredLower.find(candidateLower) != std::string::npos)) {
            candidate.priority = 1;
        } else if (!defaultId.empty() && candidate.id == defaultId) {
            candidate.priority = 2;
        }
        candidates.push_back(std::move(candidate));
    }
    collection->Release();

    std::stable_sort(candidates.begin(), candidates.end(),
        [](const WasapiEndpointCandidate& left, const WasapiEndpointCandidate& right) {
            return left.priority < right.priority;
        });
    return candidates;
}

static bool CaptureWithWasapi(int boundKey, int silenceThreshold, int silenceStopMs,
                              int maxRecordingMs, std::vector<short>& audioData,
                              std::string& selectedEndpointId,
                              std::string& selectedEndpointName,
                              bool& forgetSavedEndpoint) {
    forgetSavedEndpoint = false;
    const HRESULT initResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool shouldUninitialize = SUCCEEDED(initResult);
    if (FAILED(initResult) && initResult != RPC_E_CHANGED_MODE) {
        Log("VoiceRecorder: WASAPI COM initialization failed (error: 0x%08lx)",
            static_cast<unsigned long>(initResult));
        return false;
    }

    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      IID_PPV_ARGS(&enumerator));
    if (FAILED(result) || !enumerator) {
        Log("VoiceRecorder: WASAPI endpoint enumerator failed (error: 0x%08lx); using WinMM fallback",
            static_cast<unsigned long>(result));
        if (shouldUninitialize) CoUninitialize();
        return false;
    }

    std::vector<WasapiEndpointCandidate> candidates = EnumerateWasapiCaptureEndpoints(enumerator);
    std::vector<std::unique_ptr<WasapiCaptureSession>> sessions;
    constexpr std::size_t kMaxDiscoveryEndpoints = 12;
    const bool savedEndpointAvailable =
        !Config::voiceRecordingDetectedEndpointId.empty() &&
        !candidates.empty() && candidates.front().priority == 0;
    std::size_t maxCaptureEndpoints = savedEndpointAvailable ? 1 : kMaxDiscoveryEndpoints;
    WAVEFORMATEX wfx = CreateRecordingWaveFormat();
    constexpr DWORD streamFlags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                  AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    constexpr REFERENCE_TIME bufferDuration = 10000000;

    for (std::size_t i = 0; i < candidates.size() && sessions.size() < maxCaptureEndpoints; ++i) {
        WasapiEndpointCandidate& candidate = candidates[i];
        auto session = std::make_unique<WasapiCaptureSession>();
        session->id = candidate.id;
        session->name = candidate.name;

        result = candidate.endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                              reinterpret_cast<void**>(&session->audioClient));
        if (SUCCEEDED(result)) {
            result = session->audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags,
                                                       bufferDuration, 0, &wfx, nullptr);
        }
        if (SUCCEEDED(result)) {
            result = session->audioClient->GetService(
                __uuidof(IAudioCaptureClient),
                reinterpret_cast<void**>(&session->captureClient));
        }
        if (SUCCEEDED(result)) result = session->audioClient->Start();
        if (FAILED(result)) {
            Log("VoiceRecorder: Skipping capture endpoint '%s' (error: 0x%08lx)",
                candidate.name.c_str(), static_cast<unsigned long>(result));
            CloseWasapiSession(*session);
            if (candidate.priority == 0) maxCaptureEndpoints = kMaxDiscoveryEndpoints;
            continue;
        }

        session->started = true;
        sessions.push_back(std::move(session));
    }

    for (auto& candidate : candidates) {
        if (candidate.endpoint) candidate.endpoint->Release();
        candidate.endpoint = nullptr;
    }
    enumerator->Release();
    enumerator = nullptr;

    if (sessions.empty()) {
        Log("VoiceRecorder: No active WASAPI capture endpoint could be opened; using WinMM fallback");
        if (shouldUninitialize) CoUninitialize();
        return false;
    }

    const bool usingSavedEndpoint =
        sessions.size() == 1 &&
        !Config::voiceRecordingDetectedEndpointId.empty() &&
        sessions.front()->id == Config::voiceRecordingDetectedEndpointId;
    if (usingSavedEndpoint) {
        Log("VoiceRecorder: WASAPI recording started on saved endpoint '%s'",
            sessions.front()->name.c_str());
    } else {
        Log("VoiceRecorder: WASAPI discovery recording started across %zu active endpoint(s)",
            sessions.size());
    }
    const DWORD startTime = timeGetTime();
    DWORD lastSignalTime = startTime;
    const bool wasInitiallyPressed = (GetAsyncKeyState(boundKey) & 0x8000) != 0;
    Sleep(50);

    while ((timeGetTime() - startTime) < static_cast<DWORD>(maxRecordingMs) && g_isRecording) {
        RecordServiceHeartbeat();
        if (wasInitiallyPressed && !(GetAsyncKeyState(boundKey) & 0x8000)) {
            Sleep(10);
            if (!(GetAsyncKeyState(boundKey) & 0x8000)) {
                Log("VoiceRecorder: Key released, stopping WASAPI recording");
                break;
            }
        }

        bool signalThisCycle = false;
        for (auto& sessionPtr : sessions) {
            WasapiCaptureSession& session = *sessionPtr;
            UINT32 packetFrames = 0;
            HRESULT packetResult = session.captureClient->GetNextPacketSize(&packetFrames);
            while (SUCCEEDED(packetResult) && packetFrames > 0) {
                BYTE* packetData = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                packetResult = session.captureClient->GetBuffer(
                    &packetData, &frames, &flags, nullptr, nullptr);
                if (FAILED(packetResult)) break;

                if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) || !packetData) {
                    session.audio.insert(session.audio.end(), frames, 0);
                } else {
                    const short* samples = reinterpret_cast<const short*>(packetData);
                    session.audio.insert(session.audio.end(), samples, samples + frames);
                    for (UINT32 sampleIndex = 0; sampleIndex < frames; ++sampleIndex) {
                        if (std::abs(static_cast<int>(samples[sampleIndex])) > silenceThreshold) {
                            signalThisCycle = true;
                            break;
                        }
                    }
                }
                session.captureClient->ReleaseBuffer(frames);
                packetResult = session.captureClient->GetNextPacketSize(&packetFrames);
            }
            if (FAILED(packetResult)) {
                Log("VoiceRecorder: Capture endpoint '%s' stopped returning packets (error: 0x%08lx)",
                    session.name.c_str(), static_cast<unsigned long>(packetResult));
            }
        }

        const DWORD now = timeGetTime();
        if (signalThisCycle) lastSignalTime = now;
        if ((now - startTime) >= 500 && (now - lastSignalTime) >= static_cast<DWORD>(silenceStopMs)) {
            Log("VoiceRecorder: Silence timeout reached during WASAPI recording");
            break;
        }
        Sleep(5);
    }

    for (auto& session : sessions) CloseWasapiSession(*session);

    const int signalFloor = std::max(64, std::min(250, silenceThreshold / 2));
    WasapiCaptureSession* winner = nullptr;
    double winnerScore = -1.0;
    for (auto& sessionPtr : sessions) {
        WasapiCaptureSession& session = *sessionPtr;
        int peak = 0;
        double sumSquares = 0.0;
        std::size_t signalSamples = 0;
        for (short sample : session.audio) {
            const int amplitude = std::abs(static_cast<int>(sample));
            peak = std::max(peak, amplitude);
            sumSquares += static_cast<double>(sample) * static_cast<double>(sample);
            if (amplitude >= signalFloor) ++signalSamples;
        }
        const double rms = session.audio.empty()
            ? 0.0
            : std::sqrt(sumSquares / static_cast<double>(session.audio.size()));
        const double signalPercent = session.audio.empty()
            ? 0.0
            : (100.0 * static_cast<double>(signalSamples) /
               static_cast<double>(session.audio.size()));
        const bool validSignal = session.audio.size() >= 8000 && peak >= signalFloor && rms >= 2.0;
        const double score = rms + (static_cast<double>(peak) * 0.02);
        Log("VoiceRecorder: Endpoint candidate '%s' samples=%zu peak=%d rms=%.1f signal=%.2f%% valid=%d",
            session.name.c_str(), session.audio.size(), peak, rms, signalPercent,
            validSignal ? 1 : 0);
        if (validSignal && score > winnerScore) {
            winner = &session;
            winnerScore = score;
        }
    }

    if (winner) {
        selectedEndpointId = winner->id;
        selectedEndpointName = winner->name;
        audioData = std::move(winner->audio);
        Log("VoiceRecorder: Automatically selected recording endpoint '%s'",
            selectedEndpointName.c_str());
    } else {
        Log("VoiceRecorder: All active capture endpoints returned digital silence");
        forgetSavedEndpoint = usingSavedEndpoint;
        audioData.clear();
    }

    if (shouldUninitialize) CoUninitialize();
    return true;
}

std::string GetCurrentRecordingDeviceName() {
    HRESULT initResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool shouldUninitialize = SUCCEEDED(initResult);
    if (FAILED(initResult) && initResult != RPC_E_CHANGED_MODE) {
        Log("VoiceRecorder: Failed to initialize COM for capture endpoint lookup (error: 0x%08lx)",
            static_cast<unsigned long>(initResult));
        return "Windows default";
    }

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* endpoint = nullptr;
    std::string deviceName = "Windows default";

    HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      IID_PPV_ARGS(&enumerator));
    if (SUCCEEDED(result) && !Config::voiceRecordingDetectedEndpointId.empty()) {
        const std::wstring endpointId = Utf8ToWide(Config::voiceRecordingDetectedEndpointId);
        if (!endpointId.empty()) result = enumerator->GetDevice(endpointId.c_str(), &endpoint);
    }
    if (!endpoint && enumerator) {
        result = enumerator->GetDefaultAudioEndpoint(eCapture, eMultimedia, &endpoint);
    }
    if (endpoint) {
        deviceName = GetEndpointFriendlyName(endpoint);
    } else {
        Log("VoiceRecorder: Failed to resolve default capture endpoint name (error: 0x%08lx)",
            static_cast<unsigned long>(result));
    }

    if (endpoint) endpoint->Release();
    if (enumerator) enumerator->Release();
    if (shouldUninitialize) CoUninitialize();
    return deviceName;
}

std::string GetCurrentRecordingDeviceDisplayName() {
    return "Current Device: " + GetCurrentRecordingDeviceName();
}

static void FinishWithCallback(const STTCallback& callback, const std::string& text,
                               std::uint64_t generation) {
    g_isRecording = false;
    if (!callback) return;
    auto complete = [callback, text]() { callback(text); };
    if (GameThreadDispatcher::IsGameThread()) complete();
    else GameThreadDispatcher::Enqueue("voice_input", "stt_completion", generation, std::move(complete));
}

static void SubmitSttUpload(std::string wavData, STTCallback callback, std::uint64_t generation) {
    g_isRecording = false;
    auto result = std::make_shared<std::string>();
    auto callbackHolder = std::make_shared<STTCallback>(std::move(callback));
    TaskManager::Options options;
    options.type = "stt_upload";
    options.key = "voice_input";
    options.generation = generation;
    options.scope.turnId = generation;
    options.lane = TaskManager::Lane::Interactive;
    options.priority = true;
    options.deadlineFromEnqueue = true;
    options.timeout = std::chrono::seconds(45);
    options.coalescing = TaskManager::CoalescingPolicy::RejectIfPendingOrActive;
    options.concurrencyLimit = 1;
    const TaskManager::TaskHandle handle = TaskManager::Submit(std::move(options),
        [wavData = std::move(wavData), result](const TaskManager::CancellationToken& token) {
            if (token.IsCancellationRequested()) return;
            *result = TrimText(HTTPManager::UploadAudioForSTT(wavData, &token));
        },
        [callbackHolder, result](bool succeeded, const char*) {
            if (*callbackHolder) (*callbackHolder)(succeeded ? *result : std::string{});
        });
    if (!handle) {
        FinishWithCallback(*callbackHolder, "", generation);
    }
}

// Recording thread function
static void RecordingThreadFunc(int boundKey, STTCallback callback, int requestedSilenceStopMs) {
    WorkerActivityGuard activity(g_recordWorkerActive, g_captureWorkersStarted, g_captureWorkersCompleted);
    const std::uint64_t generation = RuntimeGeneration::Current();
    Log("VoiceRecorder: Recording thread started with key code: %d", boundKey);
    
    // Set up wave format for recording (16-bit, 16kHz, mono - optimized for speech)
    WAVEFORMATEX wfx = CreateRecordingWaveFormat();

    const int silenceThreshold = std::max(1, Config::silenceThreshold);
    const int maxRecordingMs = std::max(1, Config::maxRecordingSeconds) * 1000;
    const int silenceStopMs = requestedSilenceStopMs > 0 ? requestedSilenceStopMs : 4000;
    Log("VoiceRecorder: Capture device: %s", GetCurrentRecordingDeviceName().c_str());
    Log("VoiceRecorder: Settings threshold=%d maxRecording=%dms silenceStop=%dms", silenceThreshold, maxRecordingMs, silenceStopMs);

    std::vector<short> audioData;
    std::string selectedEndpointId;
    std::string selectedEndpointName;
    bool forgetSavedEndpoint = false;
    const bool usedWasapi = CaptureWithWasapi(
        boundKey,
        silenceThreshold,
        silenceStopMs,
        maxRecordingMs,
        audioData,
        selectedEndpointId,
        selectedEndpointName,
        forgetSavedEndpoint);
    if (!usedWasapi) {
        // Legacy WinMM remains as a fallback for systems where WASAPI cannot initialize.
        HWAVEIN hWaveIn = nullptr;
        MMRESULT result = OpenCaptureInput(&hWaveIn, &wfx);
        if (result != MMSYSERR_NOERROR) {
            Log("VoiceRecorder: Failed to open wave input device (error: %d)", result);
            FinishWithCallback(callback, "", generation);
            return;
        }

        // Use eight 250ms-ish buffers at 16kHz/16-bit mono.
        const int BUFFER_SIZE = 16000 * 2 * 2 / 8;
        const int bufferDurationMs = (BUFFER_SIZE * 1000) / wfx.nAvgBytesPerSec;
        char buffers[8][BUFFER_SIZE] = {};
        WAVEHDR headers[8] = {};

        for (std::size_t i = 0; i < 8; ++i) {
            if (!PrepareCaptureBuffer(hWaveIn, headers[i], buffers[i], BUFFER_SIZE, i, "push-to-talk")) {
                CloseWaveInputSafely(hWaveIn, headers, 8, "push-to-talk setup failure");
                FinishWithCallback(callback, "", generation);
                return;
            }
        }

        result = waveInStart(hWaveIn);
        if (result != MMSYSERR_NOERROR) {
            Log("VoiceRecorder: push-to-talk waveInStart failed (error: %d)", result);
            CloseWaveInputSafely(hWaveIn, headers, 8, "push-to-talk start failure");
            FinishWithCallback(callback, "", generation);
            return;
        }
        Log("VoiceRecorder: WinMM fallback recording started (buffer=%d bytes, approx %dms)",
            BUFFER_SIZE, bufferDurationMs);

        const DWORD startTime = timeGetTime();
        signed long silenceTime = -500;
        const bool wasInitiallyPressed = (GetAsyncKeyState(boundKey) & 0x8000) != 0;
        Sleep(50);

        while (silenceTime < silenceStopMs &&
               (timeGetTime() - startTime) < static_cast<DWORD>(maxRecordingMs) && g_isRecording) {
            RecordServiceHeartbeat();
            if (wasInitiallyPressed && !(GetAsyncKeyState(boundKey) & 0x8000)) {
                Sleep(10);
                if (!(GetAsyncKeyState(boundKey) & 0x8000)) {
                    Log("VoiceRecorder: Key released, stopping WinMM fallback recording");
                    break;
                }
            }

            for (auto& header : headers) {
                if (header.dwFlags & WHDR_DONE) {
                    short* data = reinterpret_cast<short*>(header.lpData);
                    const size_t numSamples = header.dwBytesRecorded / sizeof(short);
                    audioData.insert(audioData.end(), data, data + numSamples);

                    bool silent = true;
                    for (size_t i = 0; i < numSamples; ++i) {
                        if (std::abs(static_cast<int>(data[i])) > silenceThreshold) {
                            silent = false;
                            break;
                        }
                    }
                    silenceTime = silent ? silenceTime + bufferDurationMs : 0;

                    result = waveInAddBuffer(hWaveIn, &header, sizeof(header));
                    if (result != MMSYSERR_NOERROR) {
                        Log("VoiceRecorder: push-to-talk buffer requeue failed (error: %d)", result);
                        g_isRecording = false;
                        break;
                    }
                }
            }
            Sleep(10);
        }

        CloseWaveInputSafely(hWaveIn, headers, 8, "push-to-talk");
    }

    if (usedWasapi && !selectedEndpointId.empty() && !selectedEndpointName.empty()) {
        GameThreadDispatcher::Enqueue(
            "voice_input",
            "persist_recording_endpoint",
            generation,
            [selectedEndpointId, selectedEndpointName]() {
                const bool changed =
                    Config::voiceRecordingDetectedEndpointId != selectedEndpointId ||
                    Config::voiceRecordingPreferredDeviceName != selectedEndpointName;
                Config::voiceRecordingDetectedEndpointId = selectedEndpointId;
                Config::voiceRecordingPreferredDeviceName = selectedEndpointName;
                if (!changed) return;

                const bool savedId = Config::WriteCustomINIValue(
                    "VoiceRecording", "DetectedEndpointId", selectedEndpointId.c_str());
                const bool savedName = Config::WriteCustomINIValue(
                    "VoiceRecording", "CurrentDevice", selectedEndpointName.c_str());
                Log("VoiceRecorder: Persisted automatic endpoint selection id=%d name=%d",
                    savedId ? 1 : 0, savedName ? 1 : 0);
            });
    } else if (forgetSavedEndpoint) {
        GameThreadDispatcher::Enqueue(
            "voice_input",
            "forget_recording_endpoint",
            generation,
            []() {
                Config::voiceRecordingDetectedEndpointId.clear();
                const bool cleared = Config::WriteCustomINIValue(
                    "VoiceRecording", "DetectedEndpointId", "");
                Log("VoiceRecorder: Saved endpoint returned silence and was cleared for rediscovery (%d)",
                    cleared ? 1 : 0);
            });
    }

    Log("VoiceRecorder: Recording stopped, captured %zu samples", audioData.size());

    if (audioData.empty()) {
        Log("VoiceRecorder: No usable microphone signal was captured; skipping STT upload");
        FinishWithCallback(callback, "", generation);
        return;
    }

    int peak = 0;
    double sumSquares = 0.0;
    size_t nonSilentSamples = 0;
    for (short sample : audioData) {
        const int amplitude = std::abs(static_cast<int>(sample));
        peak = std::max(peak, amplitude);
        sumSquares += static_cast<double>(sample) * static_cast<double>(sample);
        if (amplitude > silenceThreshold) {
            ++nonSilentSamples;
        }
    }
    const double rms = audioData.empty() ? 0.0 : std::sqrt(sumSquares / static_cast<double>(audioData.size()));
    const double nonSilentPercent = audioData.empty()
        ? 0.0
        : (100.0 * static_cast<double>(nonSilentSamples) / static_cast<double>(audioData.size()));
    Log("VoiceRecorder: Signal peak=%d rms=%.1f non_silent=%.2f%% threshold=%d",
        peak, rms, nonSilentPercent, silenceThreshold);

    // Build WAV file in memory
    std::stringstream wavStream;

    // Write the WAV file header
    // RIFF chunk descriptor
    const char* riffHeader = "RIFF";
    wavStream.write(riffHeader, 4);

    // File size (will update later)
    DWORD fileSize = 0;
    wavStream.write(reinterpret_cast<const char*>(&fileSize), 4);

    // WAVE format
    const char* waveHeader = "WAVE";
    wavStream.write(waveHeader, 4);

    // Format subchunk
    const char* formatHeader = "fmt ";
    wavStream.write(formatHeader, 4);

    DWORD fmtSize = 16;
    wavStream.write(reinterpret_cast<const char*>(&fmtSize), 4);
    wavStream.write(reinterpret_cast<const char*>(&wfx.wFormatTag), 2);
    wavStream.write(reinterpret_cast<const char*>(&wfx.nChannels), 2);
    wavStream.write(reinterpret_cast<const char*>(&wfx.nSamplesPerSec), 4);
    wavStream.write(reinterpret_cast<const char*>(&wfx.nAvgBytesPerSec), 4);
    wavStream.write(reinterpret_cast<const char*>(&wfx.nBlockAlign), 2);
    wavStream.write(reinterpret_cast<const char*>(&wfx.wBitsPerSample), 2);

    // Data subchunk
    const char* dataHeader = "data";
    wavStream.write(dataHeader, 4);

    // Data size
    DWORD dataSize = static_cast<DWORD>(audioData.size() * sizeof(short));
    wavStream.write(reinterpret_cast<const char*>(&dataSize), 4);

    // Write the audio data
    wavStream.write(reinterpret_cast<const char*>(audioData.data()), dataSize);

    // Update the file size in the WAV header
    fileSize = dataSize + 36;
    wavStream.seekp(4);
    wavStream.write(reinterpret_cast<const char*>(&fileSize), 4);

    // Calculate audio duration in seconds
    float audioDurationSecs = static_cast<float>(dataSize) / (16000.0f * sizeof(short));
    Log("VoiceRecorder: Audio duration: %.2f seconds, size: %u bytes", audioDurationSecs, fileSize);

    // Check if audio is too short
    if (audioDurationSecs < 0.5f) {
        Log("VoiceRecorder: Audio too short: %.2f seconds", audioDurationSecs);
        FinishWithCallback(callback, "", generation);
        return;
    }

    if (fileSize < 32000) {
        Log("VoiceRecorder: Audio file too small: %u bytes", fileSize);
        FinishWithCallback(callback, "", generation);
        return;
    }

    std::string wavData = wavStream.str();
    if (Config::voiceRecordingSaveLastWav) {
        const char* debugPath = "Data\\NVSE\\Plugins\\dialectic_last_stt.wav";
        std::ofstream debugFile(debugPath, std::ios::binary | std::ios::trunc);
        if (debugFile.is_open()) {
            debugFile.write(wavData.data(), static_cast<std::streamsize>(wavData.size()));
            Log("VoiceRecorder: Saved diagnostic STT WAV to %s", debugPath);
        } else {
            Log("VoiceRecorder: Could not save diagnostic STT WAV to %s", debugPath);
        }
    }
    Log("VoiceRecorder: WAV file built in memory; submitting managed STT task");
    SubmitSttUpload(std::move(wavData), std::move(callback), generation);
    Log("VoiceRecorder: Recording thread ended after STT submission");
}

static void OpenMicMonitoringThreadFunc(OpenMicCallback onVoiceDetected) {
    WorkerActivityGuard activity(g_openMicWorkerActive, g_monitorWorkersStarted, g_monitorWorkersCompleted);
    const std::uint64_t generation = RuntimeGeneration::Current();
    Log("VoiceRecorder: Open mic monitoring thread started");

    WAVEFORMATEX wfx = CreateRecordingWaveFormat();
    HWAVEIN hWaveIn = nullptr;
    MMRESULT result = OpenCaptureInput(&hWaveIn, &wfx);
    if (result != MMSYSERR_NOERROR) {
        Log("VoiceRecorder: Failed to open wave input for open mic monitoring (error: %d)", result);
        g_openMicMonitoringActive = false;
        return;
    }

    const int bufferSize = 16000 * 2 / 8;
    char buffers[4][bufferSize] = {};
    WAVEHDR headers[4] = {};

    for (std::size_t i = 0; i < 4; ++i) {
        if (!PrepareCaptureBuffer(hWaveIn, headers[i], buffers[i], bufferSize, i, "open mic")) {
            CloseWaveInputSafely(hWaveIn, headers, 4, "open mic setup failure");
            g_openMicMonitoringActive = false;
            return;
        }
    }

    result = waveInStart(hWaveIn);
    if (result != MMSYSERR_NOERROR) {
        Log("VoiceRecorder: open mic waveInStart failed (error: %d)", result);
        CloseWaveInputSafely(hWaveIn, headers, 4, "open mic start failure");
        g_openMicMonitoringActive = false;
        return;
    }
    Log("VoiceRecorder: Open mic monitoring started on device: %s", GetCurrentRecordingDeviceName().c_str());

    bool voiceDetected = false;
    double peakRmsSinceLastLog = 0.0;
    auto lastVoiceActivity = std::chrono::steady_clock::now();
    auto lastDebugLog = std::chrono::steady_clock::now();
    auto lastAudioLevelLog = std::chrono::steady_clock::now();

    while (g_openMicMonitoringActive) {
        RecordServiceHeartbeat();
        if (g_openMicMuted) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        const auto now = std::chrono::steady_clock::now();
        const double sensitivity = g_openMicSensitivity.load();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - lastDebugLog).count() >= 5) {
            Log("VoiceRecorder: Open mic active muted=%d sensitivity=%.1f", g_openMicMuted.load() ? 1 : 0, sensitivity);
            lastDebugLog = now;
        }

        for (auto& header : headers) {
            if (!(header.dwFlags & WHDR_DONE)) {
                continue;
            }

            short* data = reinterpret_cast<short*>(header.lpData);
            const size_t numSamples = header.dwBytesRecorded / sizeof(short);
            double rms = 0.0;
            if (numSamples > 0) {
                for (size_t i = 0; i < numSamples; ++i) {
                    const double sample = static_cast<double>(data[i]);
                    rms += sample * sample;
                }
                rms = std::sqrt(rms / static_cast<double>(numSamples));
            }

            peakRmsSinceLastLog = std::max(peakRmsSinceLastLog, rms);
            if (std::chrono::duration_cast<std::chrono::seconds>(now - lastAudioLevelLog).count() >= 5) {
                Log("VoiceRecorder: Open mic audio level current=%.1f peak=%.1f threshold=%.1f",
                    rms, peakRmsSinceLastLog, sensitivity);
                lastAudioLevelLog = now;
                peakRmsSinceLastLog = 0.0;
            }

            if (rms > sensitivity) {
                lastVoiceActivity = std::chrono::steady_clock::now();
                if (!voiceDetected && !g_isRecording) {
                    voiceDetected = true;
                    Log("VoiceRecorder: Open mic voice detected, releasing monitor for STT recording");
                    g_openMicMonitoringActive = false;
                    break;
                }
            } else if (voiceDetected) {
                const auto silenceDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - lastVoiceActivity).count();
                const int endDelayMs = static_cast<int>(std::max(0.1, g_openMicEndDelaySeconds.load()) * 1000.0);
                if (silenceDuration > endDelayMs) {
                    g_openMicMonitoringActive = false;
                    break;
                }
            }

            result = waveInAddBuffer(hWaveIn, &header, sizeof(header));
            if (result != MMSYSERR_NOERROR) {
                Log("VoiceRecorder: open mic buffer requeue failed (error: %d)", result);
                g_openMicMonitoringActive = false;
                break;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    CloseWaveInputSafely(hWaveIn, headers, 4, "open mic");

    if (voiceDetected && onVoiceDetected) {
        GameThreadDispatcher::Enqueue("voice_input", "open_mic_detected", generation,
            [onVoiceDetected = std::move(onVoiceDetected)]() { onVoiceDetected(); });
    }

    Log("VoiceRecorder: Open mic monitoring thread ended");
}

void StartRecording(int boundKey, STTCallback callback, int silenceStopMs) {
    if (g_isRecording) {
        Log("VoiceRecorder: Already recording, ignoring start request");
        return;
    }

    Log("VoiceRecorder: Starting recording with key: %d", boundKey);
    TaskManager::CancelByType("stt_upload");
    g_isRecording = true;

    // A completed std::thread remains joinable. Reap it before starting the
    // next recording so no recorder can survive into another request.
    if (g_recordThread.joinable()) {
        g_recordThread.join();
    }

    // Start recording in a new thread
    g_recordThread = std::thread(RecordingThreadFunc, boundKey, callback, silenceStopMs);
}

bool IsRecording() {
    return g_isRecording;
}

void StopRecording() {
    if (g_isRecording) {
        Log("VoiceRecorder: Stopping recording");
        g_isRecording = false;
    }
}

void Shutdown() {
    g_isRecording = false;
    g_openMicMonitoringActive = false;
    TaskManager::CancelByType("stt_upload");
    const std::thread::id current = std::this_thread::get_id();
    if (g_openMicThread.joinable() && g_openMicThread.get_id() != current) {
        g_openMicThread.join();
    }
    if (g_recordThread.joinable() && g_recordThread.get_id() != current) {
        g_recordThread.join();
    }
    RecordServiceHeartbeat();
    Log("VoiceRecorder: device workers shut down");
}

ServiceStatus GetServiceStatus() {
    ServiceStatus status;
    status.recording = g_isRecording.load(std::memory_order_acquire);
    status.openMicMonitoring = g_openMicMonitoringActive.load(std::memory_order_acquire);
    status.recordWorkerActive = g_recordWorkerActive.load(std::memory_order_acquire);
    status.openMicWorkerActive = g_openMicWorkerActive.load(std::memory_order_acquire);
    status.openMicMuted = g_openMicMuted.load(std::memory_order_acquire);
    const std::uint64_t heartbeat = g_serviceHeartbeatMs.load(std::memory_order_acquire);
    status.heartbeatAgeMs = heartbeat == 0 ? 0 : SteadyNowMs() - heartbeat;
    status.captureWorkersStarted = g_captureWorkersStarted.load(std::memory_order_relaxed);
    status.captureWorkersCompleted = g_captureWorkersCompleted.load(std::memory_order_relaxed);
    status.monitorWorkersStarted = g_monitorWorkersStarted.load(std::memory_order_relaxed);
    status.monitorWorkersCompleted = g_monitorWorkersCompleted.load(std::memory_order_relaxed);
    return status;
}

void StartOpenMicMonitoring(OpenMicCallback onVoiceDetected) {
    if (g_openMicMonitoringActive || g_isRecording) {
        return;
    }

    if (g_openMicThread.joinable()) {
        g_openMicThread.join();
    }

    g_openMicMonitoringActive = true;
    g_openMicThread = std::thread(OpenMicMonitoringThreadFunc, onVoiceDetected);
}

void StopOpenMicMonitoring() {
    g_openMicMonitoringActive = false;
    if (g_openMicThread.joinable()) {
        g_openMicThread.join();
    }
}

bool IsOpenMicMonitoring() {
    return g_openMicMonitoringActive;
}

void UpdateOpenMicSettings(float sensitivity, float endDelaySeconds, bool muted) {
    g_openMicSensitivity = std::max(1.0f, sensitivity);
    g_openMicEndDelaySeconds = std::max(0.1f, endDelaySeconds);
    g_openMicMuted = muted;
}

} // namespace VoiceRecorder
