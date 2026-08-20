#include "AudioManager.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include <xaudio2.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <vector>
#include <string>

#include "Logger.h"

#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "winmm.lib")

// Local type aliases
typedef uint16_t UInt16;
typedef uint32_t UInt32;

namespace AudioManager {

    static IXAudio2* g_pXAudio2 = nullptr;
    static IXAudio2MasteringVoice* g_pMasterVoice = nullptr;
    static IXAudio2SourceVoice* g_pSourceVoice = nullptr;
    static std::vector<unsigned char> g_audioBuffer;
    static UINT32 g_masterChannels = 0;
    static UINT32 g_sourceChannels = 0;

    static bool g_initialized = false;
    static bool g_isPlaying = false;
    static bool g_isPaused = false;
    static double g_playStartTime = 0.0;
    static double g_pauseStartTime = 0.0;
    static double g_totalPausedTime = 0.0;
    static float g_volume = 1.0f;
    static bool g_3DPlaybackEnabled = true;
    static bool g_cameraBasedAudio = false;
    static float g_3DPanStrength = 1.0f;
    static float g_lastAppliedPan = 99.0f;

    // Recording state
    static HWAVEIN g_hWaveIn = nullptr;
    static bool g_isRecording = false;
    static std::vector<char> g_recordBuffer;
    static WAVEHDR g_waveHeader = {};
    static char g_recordChunk[16384];  // 16KB recording buffer

    static float Clamp(float value, float minValue, float maxValue) {
        return std::max(minValue, std::min(value, maxValue));
    }

    static bool HasOutputMatrixTarget() {
        return g_pSourceVoice != nullptr &&
               g_pMasterVoice != nullptr &&
               g_sourceChannels > 0 &&
               g_masterChannels > 0;
    }

    static bool SetOutputMatrix(const std::vector<float>& matrix, const char* reason) {
        if (!HasOutputMatrixTarget() ||
            matrix.size() != static_cast<size_t>(g_sourceChannels) * static_cast<size_t>(g_masterChannels)) {
            return false;
        }

        HRESULT hr = g_pSourceVoice->SetOutputMatrix(
            g_pMasterVoice,
            g_sourceChannels,
            g_masterChannels,
            matrix.data(),
            XAUDIO2_COMMIT_NOW);
        if (FAILED(hr)) {
            Logger::LogWarning("AudioManager: SetOutputMatrix failed for %s (HRESULT=0x%08X src=%u dst=%u)",
                reason,
                static_cast<unsigned int>(hr),
                g_sourceChannels,
                g_masterChannels);
            return false;
        }

        return true;
    }

    static void ApplyCenteredOutputMatrix() {
        if (!HasOutputMatrixTarget()) {
            return;
        }

        if (std::fabs(g_lastAppliedPan) <= 0.001f) {
            return;
        }

        std::vector<float> matrix(static_cast<size_t>(g_sourceChannels) * static_cast<size_t>(g_masterChannels), 0.0f);
        if (g_sourceChannels == 1) {
            matrix[0] = 1.0f;
            if (g_masterChannels >= 2) {
                matrix[1] = 1.0f;
            }
        } else {
            const UINT32 mappedChannels = std::min(g_sourceChannels, g_masterChannels);
            for (UINT32 channel = 0; channel < mappedChannels; ++channel) {
                matrix[static_cast<size_t>(channel) * g_masterChannels + channel] = 1.0f;
            }
        }

        if (SetOutputMatrix(matrix, "centered playback")) {
            g_lastAppliedPan = 0.0f;
        }
    }

    static void ApplyPannedOutputMatrix(float pan) {
        if (!HasOutputMatrixTarget()) {
            return;
        }

        if (g_masterChannels < 2) {
            ApplyCenteredOutputMatrix();
            return;
        }

        pan = Clamp(pan, -1.0f, 1.0f);
        if (std::fabs(pan - g_lastAppliedPan) < 0.02f) {
            return;
        }

        const float left = pan <= 0.0f ? 1.0f : 1.0f - pan;
        const float right = pan >= 0.0f ? 1.0f : 1.0f + pan;

        std::vector<float> matrix(static_cast<size_t>(g_sourceChannels) * static_cast<size_t>(g_masterChannels), 0.0f);
        if (g_sourceChannels == 1) {
            matrix[0] = left;
            matrix[1] = right;
        } else {
            const float perSourceScale = 1.0f / static_cast<float>(g_sourceChannels);
            for (UINT32 source = 0; source < g_sourceChannels; ++source) {
                matrix[static_cast<size_t>(source) * g_masterChannels + 0] = left * perSourceScale;
                matrix[static_cast<size_t>(source) * g_masterChannels + 1] = right * perSourceScale;
            }
        }

        if (SetOutputMatrix(matrix, "3D pan")) {
            g_lastAppliedPan = pan;
        }
    }

    void Initialize() {
        if (g_initialized) {
            return;
        }

        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
            Logger::LogError("AudioManager: CoInitializeEx failed (HRESULT=0x%08X)", static_cast<unsigned int>(hr));
            return;
        }

        hr = XAudio2Create(&g_pXAudio2, 0, XAUDIO2_DEFAULT_PROCESSOR);
        if (FAILED(hr)) {
            Logger::LogError("AudioManager: XAudio2Create failed (HRESULT=0x%08X)", static_cast<unsigned int>(hr));
            return;
        }

        hr = g_pXAudio2->CreateMasteringVoice(&g_pMasterVoice);
        if (FAILED(hr)) {
            Logger::LogError("AudioManager: CreateMasteringVoice failed (HRESULT=0x%08X)", static_cast<unsigned int>(hr));
            g_pXAudio2->Release();
            g_pXAudio2 = nullptr;
            return;
        }

        XAUDIO2_VOICE_DETAILS masterDetails = {};
        g_pMasterVoice->GetVoiceDetails(&masterDetails);
        g_masterChannels = masterDetails.InputChannels > 0 ? masterDetails.InputChannels : 2;

        g_initialized = true;
        Logger::LogInfo("AudioManager: XAudio2 initialized masterChannels=%u", g_masterChannels);
    }

    void Shutdown() {
        if (g_pSourceVoice) {
            g_pSourceVoice->Stop(0);
            g_pSourceVoice->DestroyVoice();
            g_pSourceVoice = nullptr;
        }

        if (g_pMasterVoice) {
            g_pMasterVoice->DestroyVoice();
            g_pMasterVoice = nullptr;
        }

        if (g_pXAudio2) {
            g_pXAudio2->Release();
            g_pXAudio2 = nullptr;
        }

        g_audioBuffer.clear();
        g_sourceChannels = 0;
        g_masterChannels = 0;
        g_lastAppliedPan = 99.0f;
        g_initialized = false;
        g_isPlaying = false;
        g_isPaused = false;
        g_playStartTime = 0.0;
        g_pauseStartTime = 0.0;
        g_totalPausedTime = 0.0;
    }

    bool LoadWAV(const unsigned char* wavData, unsigned long dataSize) {
        if (!g_initialized || !wavData || dataSize < 44) {
            Logger::LogError("AudioManager: LoadWAV rejected input initialized=%d data=%d size=%lu",
                g_initialized ? 1 : 0, wavData ? 1 : 0, dataSize);
            return false;
        }

        if (g_pSourceVoice) {
            g_pSourceVoice->Stop(0);
            g_pSourceVoice->DestroyVoice();
            g_pSourceVoice = nullptr;
            g_sourceChannels = 0;
        }

        if (std::memcmp(wavData, "RIFF", 4) != 0 || std::memcmp(wavData + 8, "WAVE", 4) != 0) {
            Logger::LogError("AudioManager: Invalid WAV header");
            return false;
        }

        WAVEFORMATEX waveFormat = {};
        const unsigned char* audioData = nullptr;
        DWORD audioDataSize = 0;
        bool foundFmt = false;
        bool foundData = false;

        size_t offset = 12;
        while (offset + 8 <= dataSize) {
            const unsigned char* chunk = wavData + offset;
            DWORD chunkSize = *reinterpret_cast<const DWORD*>(chunk + 4);
            size_t chunkDataOffset = offset + 8;

            if (chunkDataOffset + chunkSize > dataSize) {
                Logger::LogError("AudioManager: Invalid WAV chunk %.4s size=%lu offset=%zu total=%lu",
                    reinterpret_cast<const char*>(chunk), chunkSize, offset, dataSize);
                return false;
            }

            if (std::memcmp(chunk, "fmt ", 4) == 0) {
                if (chunkSize < 16) {
                    Logger::LogError("AudioManager: WAV fmt chunk too small: %lu", chunkSize);
                    return false;
                }

                const unsigned char* fmt = wavData + chunkDataOffset;
                waveFormat.wFormatTag = *reinterpret_cast<const UInt16*>(fmt + 0);
                waveFormat.nChannels = *reinterpret_cast<const UInt16*>(fmt + 2);
                waveFormat.nSamplesPerSec = *reinterpret_cast<const DWORD*>(fmt + 4);
                waveFormat.nAvgBytesPerSec = *reinterpret_cast<const DWORD*>(fmt + 8);
                waveFormat.nBlockAlign = *reinterpret_cast<const UInt16*>(fmt + 12);
                waveFormat.wBitsPerSample = *reinterpret_cast<const UInt16*>(fmt + 14);
                waveFormat.cbSize = chunkSize >= 18 ? *reinterpret_cast<const UInt16*>(fmt + 16) : 0;
                foundFmt = true;
            } else if (std::memcmp(chunk, "data", 4) == 0) {
                audioData = wavData + chunkDataOffset;
                audioDataSize = chunkSize;
                foundData = true;
            }

            offset = chunkDataOffset + chunkSize + (chunkSize % 2);
        }

        if (!foundFmt || !foundData || !audioData || audioDataSize == 0) {
            Logger::LogError("AudioManager: Missing WAV fmt/data chunks fmt=%d data=%d audioSize=%lu",
                foundFmt ? 1 : 0, foundData ? 1 : 0, audioDataSize);
            return false;
        }

        if (waveFormat.wFormatTag != WAVE_FORMAT_PCM && waveFormat.wFormatTag != WAVE_FORMAT_IEEE_FLOAT) {
            Logger::LogError("AudioManager: Unsupported WAV format tag=%u", waveFormat.wFormatTag);
            return false;
        }

        if (waveFormat.nAvgBytesPerSec == 0) {
            waveFormat.nBlockAlign = waveFormat.nChannels * waveFormat.wBitsPerSample / 8;
            waveFormat.nAvgBytesPerSec = waveFormat.nSamplesPerSec * waveFormat.nBlockAlign;
        }

        HRESULT hr = g_pXAudio2->CreateSourceVoice(&g_pSourceVoice, &waveFormat);
        if (FAILED(hr)) {
            Logger::LogError("AudioManager: CreateSourceVoice failed (HRESULT=0x%08X format=%u channels=%u rate=%lu bits=%u)",
                static_cast<unsigned int>(hr),
                waveFormat.wFormatTag,
                waveFormat.nChannels,
                waveFormat.nSamplesPerSec,
                waveFormat.wBitsPerSample);
            return false;
        }

        g_sourceChannels = waveFormat.nChannels;
        g_lastAppliedPan = 99.0f;
        g_audioBuffer.assign(audioData, audioData + audioDataSize);

        XAUDIO2_BUFFER buffer = {};
        buffer.AudioBytes = audioDataSize;
        buffer.pAudioData = g_audioBuffer.data();
        buffer.Flags = XAUDIO2_END_OF_STREAM;

        hr = g_pSourceVoice->SubmitSourceBuffer(&buffer);
        if (FAILED(hr)) {
            Logger::LogError("AudioManager: SubmitSourceBuffer failed (HRESULT=0x%08X)", static_cast<unsigned int>(hr));
            g_pSourceVoice->DestroyVoice();
            g_pSourceVoice = nullptr;
            g_audioBuffer.clear();
            return false;
        }

        g_pSourceVoice->SetVolume(g_volume);
        ApplyCenteredOutputMatrix();
        Logger::LogInfo("AudioManager: Loaded WAV format=%u channels=%u rate=%lu bits=%u data=%lu",
            waveFormat.wFormatTag,
            waveFormat.nChannels,
            waveFormat.nSamplesPerSec,
            waveFormat.wBitsPerSample,
            audioDataSize);
        return true;
    }

    bool Play() {
        if (!g_pSourceVoice) {
            Logger::LogError("AudioManager: Play called with no source voice");
            return false;
        }

        HRESULT hr = g_pSourceVoice->Start(0, XAUDIO2_COMMIT_NOW);
        if (FAILED(hr)) {
            Logger::LogError("AudioManager: SourceVoice Start failed (HRESULT=0x%08X)", static_cast<unsigned int>(hr));
            return false;
        }

        g_isPlaying = true;
        g_isPaused = false;
        g_playStartTime = timeGetTime() / 1000.0;
        g_pauseStartTime = 0.0;
        g_totalPausedTime = 0.0;
        return true;
    }

    void Stop() {
        if (g_pSourceVoice) {
            g_pSourceVoice->Stop(0);
        }
        ApplyCenteredOutputMatrix();
        g_isPlaying = false;
        g_isPaused = false;
        g_pauseStartTime = 0.0;
        g_totalPausedTime = 0.0;
    }

    void Pause() {
        if (g_pSourceVoice && g_isPlaying && !g_isPaused) {
            g_pSourceVoice->Stop(0);
            g_pauseStartTime = timeGetTime() / 1000.0;
            g_isPaused = true;
        }
    }

    void Resume() {
        if (g_pSourceVoice && g_isPlaying && g_isPaused) {
            const double now = timeGetTime() / 1000.0;
            if (g_pauseStartTime > 0.0 && now >= g_pauseStartTime) {
                g_totalPausedTime += now - g_pauseStartTime;
            }
            g_pauseStartTime = 0.0;
            g_isPaused = false;
            g_pSourceVoice->Start(0, XAUDIO2_COMMIT_NOW);
        }
    }

    void Update(const Vector3& sourcePos, const Vector3& listenerPos,
                const Vector3& listenerForward, float listenerYaw) {
        if (!g_pSourceVoice) {
            return;
        }

        if (!g_3DPlaybackEnabled) {
            ApplyCenteredOutputMatrix();
            return;
        }

        const float dx = sourcePos.x - listenerPos.x;
        const float dy = sourcePos.y - listenerPos.y;
        const float horizontalDistance = std::sqrt(dx * dx + dy * dy);
        if (horizontalDistance < 1.0f) {
            ApplyPannedOutputMatrix(0.0f);
            return;
        }

        float rightX = std::cos(listenerYaw);
        float rightY = -std::sin(listenerYaw);
        if (g_cameraBasedAudio) {
            const float forwardLength = std::sqrt((listenerForward.x * listenerForward.x) + (listenerForward.y * listenerForward.y));
            if (forwardLength > 0.001f) {
                const float forwardX = listenerForward.x / forwardLength;
                const float forwardY = listenerForward.y / forwardLength;
                rightX = forwardY;
                rightY = -forwardX;
            }
        }
        const float rightComponent = (dx * rightX + dy * rightY) / horizontalDistance;
        constexpr float kMaxEarPan = 0.75f;
        const float pan = Clamp(rightComponent * g_3DPanStrength, -kMaxEarPan, kMaxEarPan);
        ApplyPannedOutputMatrix(pan);
    }

    void Set3DPlaybackEnabled(bool enabled) {
        if (g_3DPlaybackEnabled == enabled) {
            if (!enabled) {
                ApplyCenteredOutputMatrix();
            }
            return;
        }

        g_3DPlaybackEnabled = enabled;
        if (!g_3DPlaybackEnabled) {
            ApplyCenteredOutputMatrix();
        } else {
            g_lastAppliedPan = 99.0f;
        }
    }

    void SetCameraBasedAudio(bool enabled) {
        if (g_cameraBasedAudio == enabled) {
            return;
        }

        g_cameraBasedAudio = enabled;
        g_lastAppliedPan = 99.0f;
    }

    void Set3DPlaybackStrength(float strength) {
        strength = Clamp(strength, 0.1f, 10.0f);
        if (std::fabs(strength - g_3DPanStrength) < 0.001f) {
            return;
        }

        g_3DPanStrength = strength;
        g_lastAppliedPan = 99.0f;
    }

    void SetVolume(float volume) {
        g_volume = volume < 0.0f ? 0.0f : volume;
        if (g_volume > 2.0f) {
            g_volume = 2.0f;
        }

        if (!g_pSourceVoice) {
            return;
        }

        g_pSourceVoice->SetVolume(g_volume);
    }

    bool IsPlaying() {
        if (!g_pSourceVoice || !g_isPlaying) {
            return false;
        }

        XAUDIO2_VOICE_STATE state = {};
        g_pSourceVoice->GetState(&state);
        if (state.BuffersQueued == 0) {
            g_isPlaying = false;
            g_isPaused = false;
            g_pauseStartTime = 0.0;
            return false;
        }
        return true;
    }

    bool IsPaused() {
        return g_isPlaying && g_isPaused;
    }

    double GetCurrentPlayTime() {
        if (!g_isPlaying) {
            return 0.0;
        }
        const double now = g_isPaused && g_pauseStartTime > 0.0
            ? g_pauseStartTime
            : timeGetTime() / 1000.0;
        return now - g_playStartTime - g_totalPausedTime;
    }

    // Recording functions for voice input
    void StartRecording() {
        if (g_isRecording) return;

        // Set up wave format for recording (16-bit, 16kHz, mono - good for speech)
        WAVEFORMATEX wfx = {};
        wfx.wFormatTag = WAVE_FORMAT_PCM;
        wfx.nChannels = 1;
        wfx.nSamplesPerSec = 16000;
        wfx.wBitsPerSample = 16;
        wfx.nBlockAlign = wfx.nChannels * wfx.wBitsPerSample / 8;
        wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
        wfx.cbSize = 0;

        // Open wave input device
        MMRESULT result = waveInOpen(&g_hWaveIn, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL);
        if (result != MMSYSERR_NOERROR) {
            return;
        }

        // Clear previous recording
        g_recordBuffer.clear();

        // Prepare header
        ZeroMemory(&g_waveHeader, sizeof(WAVEHDR));
        g_waveHeader.lpData = g_recordChunk;
        g_waveHeader.dwBufferLength = sizeof(g_recordChunk);

        result = waveInPrepareHeader(g_hWaveIn, &g_waveHeader, sizeof(WAVEHDR));
        if (result != MMSYSERR_NOERROR) {
            waveInClose(g_hWaveIn);
            g_hWaveIn = nullptr;
            return;
        }

        // Add buffer to queue
        result = waveInAddBuffer(g_hWaveIn, &g_waveHeader, sizeof(WAVEHDR));
        if (result != MMSYSERR_NOERROR) {
            waveInUnprepareHeader(g_hWaveIn, &g_waveHeader, sizeof(WAVEHDR));
            waveInClose(g_hWaveIn);
            g_hWaveIn = nullptr;
            return;
        }

        // Start recording
        result = waveInStart(g_hWaveIn);
        if (result != MMSYSERR_NOERROR) {
            waveInUnprepareHeader(g_hWaveIn, &g_waveHeader, sizeof(WAVEHDR));
            waveInClose(g_hWaveIn);
            g_hWaveIn = nullptr;
            return;
        }

        g_isRecording = true;
    }

    void StopRecording() {
        if (!g_isRecording || !g_hWaveIn) return;

        // Stop recording
        waveInStop(g_hWaveIn);
        waveInReset(g_hWaveIn);

        // Copy recorded data
        if (g_waveHeader.dwBytesRecorded > 0) {
            g_recordBuffer.insert(g_recordBuffer.end(), 
                g_recordChunk, g_recordChunk + g_waveHeader.dwBytesRecorded);
        }

        // Cleanup
        waveInUnprepareHeader(g_hWaveIn, &g_waveHeader, sizeof(WAVEHDR));
        waveInClose(g_hWaveIn);
        g_hWaveIn = nullptr;
        g_isRecording = false;
    }

    bool IsRecording() {
        return g_isRecording;
    }

    std::string GetRecordedAudio() {
        if (g_recordBuffer.empty()) {
            return "";
        }

        // Return as base64 encoded WAV data
        // First, build WAV header
        std::vector<char> wavData;
        
        // RIFF header
        wavData.push_back('R'); wavData.push_back('I'); 
        wavData.push_back('F'); wavData.push_back('F');
        
        uint32_t fileSize = 36 + (uint32_t)g_recordBuffer.size();
        wavData.push_back(fileSize & 0xFF);
        wavData.push_back((fileSize >> 8) & 0xFF);
        wavData.push_back((fileSize >> 16) & 0xFF);
        wavData.push_back((fileSize >> 24) & 0xFF);
        
        wavData.push_back('W'); wavData.push_back('A'); 
        wavData.push_back('V'); wavData.push_back('E');
        
        // fmt chunk
        wavData.push_back('f'); wavData.push_back('m'); 
        wavData.push_back('t'); wavData.push_back(' ');
        
        uint32_t fmtSize = 16;
        wavData.push_back(fmtSize & 0xFF);
        wavData.push_back((fmtSize >> 8) & 0xFF);
        wavData.push_back((fmtSize >> 16) & 0xFF);
        wavData.push_back((fmtSize >> 24) & 0xFF);
        
        uint16_t audioFormat = 1;  // PCM
        wavData.push_back(audioFormat & 0xFF);
        wavData.push_back((audioFormat >> 8) & 0xFF);
        
        uint16_t numChannels = 1;
        wavData.push_back(numChannels & 0xFF);
        wavData.push_back((numChannels >> 8) & 0xFF);
        
        uint32_t sampleRate = 16000;
        wavData.push_back(sampleRate & 0xFF);
        wavData.push_back((sampleRate >> 8) & 0xFF);
        wavData.push_back((sampleRate >> 16) & 0xFF);
        wavData.push_back((sampleRate >> 24) & 0xFF);
        
        uint32_t byteRate = 32000;  // 16000 * 1 * 2
        wavData.push_back(byteRate & 0xFF);
        wavData.push_back((byteRate >> 8) & 0xFF);
        wavData.push_back((byteRate >> 16) & 0xFF);
        wavData.push_back((byteRate >> 24) & 0xFF);
        
        uint16_t blockAlign = 2;
        wavData.push_back(blockAlign & 0xFF);
        wavData.push_back((blockAlign >> 8) & 0xFF);
        
        uint16_t bitsPerSample = 16;
        wavData.push_back(bitsPerSample & 0xFF);
        wavData.push_back((bitsPerSample >> 8) & 0xFF);
        
        // data chunk
        wavData.push_back('d'); wavData.push_back('a'); 
        wavData.push_back('t'); wavData.push_back('a');
        
        uint32_t dataSize = (uint32_t)g_recordBuffer.size();
        wavData.push_back(dataSize & 0xFF);
        wavData.push_back((dataSize >> 8) & 0xFF);
        wavData.push_back((dataSize >> 16) & 0xFF);
        wavData.push_back((dataSize >> 24) & 0xFF);
        
        // Append audio data
        wavData.insert(wavData.end(), g_recordBuffer.begin(), g_recordBuffer.end());
        
        // Clear the recording buffer
        g_recordBuffer.clear();
        
        // Return as string (binary data)
        return std::string(wavData.begin(), wavData.end());
    }
}
