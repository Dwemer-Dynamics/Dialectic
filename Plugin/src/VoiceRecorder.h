// VoiceRecorder.h - voice recording system for Dialectic

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace VoiceRecorder {

// Callback type for when recording completes and STT result is received
using STTCallback = std::function<void(const std::string& transcribedText)>;
using OpenMicCallback = std::function<void()>;

struct ServiceStatus {
    bool recording{false};
    bool openMicMonitoring{false};
    bool recordWorkerActive{false};
    bool openMicWorkerActive{false};
    bool openMicMuted{false};
    std::uint64_t heartbeatAgeMs{0};
    std::uint64_t captureWorkersStarted{0};
    std::uint64_t captureWorkersCompleted{0};
    std::uint64_t monitorWorkersStarted{0};
    std::uint64_t monitorWorkersCompleted{0};
};

// Start voice recording in a background thread
// Parameters:
//   boundKey - Virtual key code to monitor for release
//   callback - Function to call with transcribed text
void StartRecording(int boundKey, STTCallback callback, int silenceStopMs = -1);

// Check if currently recording
bool IsRecording();

// Stop recording (can be called externally to abort)
void StopRecording();

// Stop capture, monitoring, and pending STT work, then join device workers.
void Shutdown();
ServiceStatus GetServiceStatus();

// Resolve the current Windows default capture device name for diagnostics.
std::string GetCurrentRecordingDeviceName();
std::string GetCurrentRecordingDeviceDisplayName();

// Open mic monitoring. The monitor releases the capture device before
// invoking the callback so the full STT recorder can own the microphone.
void StartOpenMicMonitoring(OpenMicCallback onVoiceDetected);
void StopOpenMicMonitoring();
bool IsOpenMicMonitoring();
void UpdateOpenMicSettings(float sensitivity, float endDelaySeconds, bool muted);

} // namespace VoiceRecorder
