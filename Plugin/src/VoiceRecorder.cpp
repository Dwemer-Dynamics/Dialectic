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
#include <mmsystem.h>
#include <thread>
#include <vector>
#include <atomic>
#include <sstream>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <cctype>

#pragma comment(lib, "winmm.lib")

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

static UINT_PTR ResolveConfiguredDeviceId() {
    if (Config::voiceRecordingDeviceId >= 0) {
        return static_cast<UINT_PTR>(Config::voiceRecordingDeviceId);
    }

    const std::string wantedName = ToLower(Config::voiceRecordingDeviceName);
    if (!wantedName.empty()) {
        const UINT count = waveInGetNumDevs();
        for (UINT i = 0; i < count; ++i) {
            WAVEINCAPSA caps = {};
            if (waveInGetDevCapsA(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
                const std::string candidate = ToLower(caps.szPname);
                if (candidate.find(wantedName) != std::string::npos) {
                    Log("VoiceRecorder: Matched configured capture device name '%s' to id %u (%s)",
                        Config::voiceRecordingDeviceName.c_str(), i, caps.szPname);
                    return static_cast<UINT_PTR>(i);
                }
            }
        }
        Log("VoiceRecorder: Configured capture device name '%s' was not found, using mapper",
            Config::voiceRecordingDeviceName.c_str());
    }

    return WAVE_MAPPER;
}

static MMRESULT OpenConfiguredWaveInput(HWAVEIN* hWaveIn, const WAVEFORMATEX* wfx) {
    const UINT_PTR deviceId = ResolveConfiguredDeviceId();
    MMRESULT result = waveInOpen(hWaveIn, deviceId, wfx, 0, 0, CALLBACK_NULL | WAVE_FORMAT_DIRECT);
    if (result != MMSYSERR_NOERROR) {
        result = waveInOpen(hWaveIn, deviceId, wfx, 0, 0, CALLBACK_NULL);
    }
    if (result != MMSYSERR_NOERROR && deviceId != WAVE_MAPPER) {
        Log("VoiceRecorder: Configured capture device %u failed to open (error: %d), trying mapper",
            static_cast<unsigned int>(deviceId), result);
        result = waveInOpen(hWaveIn, WAVE_MAPPER, wfx, 0, 0, CALLBACK_NULL | WAVE_FORMAT_DIRECT);
        if (result != MMSYSERR_NOERROR) {
            result = waveInOpen(hWaveIn, WAVE_MAPPER, wfx, 0, 0, CALLBACK_NULL);
        }
    }
    return result;
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

std::string GetCurrentRecordingDeviceName() {
    WAVEFORMATEX wfx = CreateRecordingWaveFormat();
    HWAVEIN hWaveIn = nullptr;

    MMRESULT result = OpenConfiguredWaveInput(&hWaveIn, &wfx);
    if (result != MMSYSERR_NOERROR) {
        Log("VoiceRecorder: Failed to open capture device for device-name lookup (error: %d)", result);
        return "Unavailable";
    }

    UINT deviceId = 0;
    result = waveInGetID(hWaveIn, &deviceId);
    if (result != MMSYSERR_NOERROR) {
        Log("VoiceRecorder: Failed to resolve wave input device id (error: %d)", result);
        waveInClose(hWaveIn);
        return "Unavailable";
    }

    WAVEINCAPSA caps = {};
    result = waveInGetDevCapsA(deviceId, &caps, sizeof(caps));
    waveInClose(hWaveIn);
    if (result != MMSYSERR_NOERROR) {
        Log("VoiceRecorder: Failed to query wave input device caps for id %u (error: %d)", deviceId, result);
        return "Unavailable";
    }

    return caps.szPname[0] ? std::string(caps.szPname) : "Unavailable";
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

    // Open the 'waveIn' recording device
    HWAVEIN hWaveIn = nullptr;
    MMRESULT result = OpenConfiguredWaveInput(&hWaveIn, &wfx);
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

    // Initialize the headers and add them to the queue
    for (int i = 0; i < 8; ++i) {
        headers[i].lpData = buffers[i];
        headers[i].dwBufferLength = BUFFER_SIZE;
        waveInPrepareHeader(hWaveIn, &headers[i], sizeof(headers[i]));
        waveInAddBuffer(hWaveIn, &headers[i], sizeof(headers[i]));
    }

    // Start recording
    waveInStart(hWaveIn);
    Log("VoiceRecorder: Recording started (buffer=%d bytes, approx %dms)", BUFFER_SIZE, bufferDurationMs);

    // Accumulated audio data
    std::vector<short> audioData;
    DWORD startTime = timeGetTime();
    signed long silenceTime = -500;  // Start with negative to allow initial silence

    // Check if key was initially pressed
    bool wasInitiallyPressed = (GetAsyncKeyState(boundKey) & 0x8000) != 0;
    Log("VoiceRecorder: Key initially pressed: %d", wasInitiallyPressed);
    
    // Wait a bit to avoid false key release detection
    Sleep(50);

    // Recording loop - continue while:
    // 1. Silence hasn't exceeded 4 seconds
    // 2. Total recording time < configured limit
    // 3. Recording hasn't been aborted
    while ((silenceTime < silenceStopMs) && ((timeGetTime() - startTime) < static_cast<DWORD>(maxRecordingMs)) && g_isRecording) {
        RecordServiceHeartbeat();
        // Check if key was released (only if it was initially pressed)
        // Check multiple times to avoid false positives
        if (wasInitiallyPressed) {
            bool isKeyDown = (GetAsyncKeyState(boundKey) & 0x8000) != 0;
            if (!isKeyDown) {
                // Key appears released, wait a bit and check again to confirm
                Sleep(10);
                bool stillReleased = !(GetAsyncKeyState(boundKey) & 0x8000);
                if (stillReleased) {
                    Log("VoiceRecorder: Key released, stopping recording");
                    break;
                }
            }
        }

        // Check if recording was externally stopped
        if (!g_isRecording) {
            Log("VoiceRecorder: Recording aborted externally");
            break;
        }

        // Process completed buffers
        for (auto& header : headers) {
            if (header.dwFlags & WHDR_DONE) {
                // Append the recorded audio data to the vector
                short* data = reinterpret_cast<short*>(header.lpData);
                size_t numSamples = header.dwBytesRecorded / sizeof(short);
                
                for (size_t i = 0; i < numSamples; ++i) {
                    audioData.push_back(data[i]);
                }

                // Check for silence in this buffer
                bool silent = true;
                for (size_t i = 0; i < numSamples; ++i) {
                    if (std::abs(data[i]) > silenceThreshold) {
                        silent = false;
                        break;
                    }
                }

                if (silent) {
                    silenceTime += bufferDurationMs;
                } else {
                    // Reset the silence timer if there is audio above the threshold
                    silenceTime = 0;
                }

                // Re-add the already prepared buffer to the queue.
                waveInAddBuffer(hWaveIn, &header, sizeof(header));
            }
        }

        // Sleep briefly to avoid excessive CPU usage
        Sleep(10);
    }

    // Stop recording and clean up
    waveInStop(hWaveIn);
    for (auto& header : headers) {
        waveInUnprepareHeader(hWaveIn, &header, sizeof(header));
    }
    waveInClose(hWaveIn);

    Log("VoiceRecorder: Recording stopped, captured %zu samples", audioData.size());

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
    MMRESULT result = OpenConfiguredWaveInput(&hWaveIn, &wfx);
    if (result != MMSYSERR_NOERROR) {
        Log("VoiceRecorder: Failed to open wave input for open mic monitoring (error: %d)", result);
        g_openMicMonitoringActive = false;
        return;
    }

    const int bufferSize = 16000 * 2 / 8;
    char buffers[4][bufferSize] = {};
    WAVEHDR headers[4] = {};

    for (int i = 0; i < 4; ++i) {
        headers[i].lpData = buffers[i];
        headers[i].dwBufferLength = bufferSize;
        waveInPrepareHeader(hWaveIn, &headers[i], sizeof(headers[i]));
        waveInAddBuffer(hWaveIn, &headers[i], sizeof(headers[i]));
    }

    waveInStart(hWaveIn);
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

            waveInAddBuffer(hWaveIn, &header, sizeof(header));
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    waveInStop(hWaveIn);
    for (auto& header : headers) {
        waveInUnprepareHeader(hWaveIn, &header, sizeof(header));
    }
    waveInClose(hWaveIn);

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
