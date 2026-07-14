#pragma once

#include <string>
#include <vector>

namespace VoiceSampleOverridesFNV {
    struct VoiceSampleOverride {
        std::string voiceType;
        std::string voiceFile;
        std::string transcript;
        std::string sourceFile;
    };

    bool FindVoiceSampleOverride(const std::string& voiceType, VoiceSampleOverride& result);
    std::vector<VoiceSampleOverride> GetAllVoiceSampleOverrides();
    void LoadVoiceCSVData();
    void ReloadVoiceCSVData();
}
