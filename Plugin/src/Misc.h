#pragma once

#include <string>
#include <chrono>
#include <mutex>
#include <cstdint>

namespace Misc {
    // String utilities
    std::string Trim(const std::string& str);
    std::string ToLower(const std::string& str);
    std::string ToUpper(const std::string& str);
    
    // Time utilities
    long long GetCurrentTimeMillis();
    std::string GetGameTimeStamp();
    
    // Game utilities
    std::string GetPlayerLocation();
    std::string GetPlayerName();
    uint32_t GetPlayerFormId();
    bool GetPlayerPitchDegrees(float& outPitchDegrees);
    
    // MD5 hashing
    std::string MD5Hash(const std::string& input);
}

// VoiceRecordControl - thread-safe singleton for controlling voice recording state.
class VoiceRecordControl {
private:
    VoiceRecordControl() : recording(false) {}
    
    VoiceRecordControl(const VoiceRecordControl&) = delete;
    VoiceRecordControl& operator=(const VoiceRecordControl&) = delete;
    
    bool recording;
    std::mutex mutex;

public:
    static VoiceRecordControl& getInstance() {
        static VoiceRecordControl instance;
        return instance;
    }
    
    bool getRecording() {
        std::lock_guard<std::mutex> lock(mutex);
        return recording;
    }
    
    void setRecording(bool value) {
        std::lock_guard<std::mutex> lock(mutex);
        recording = value;
    }
};
