#include "VoiceSampleOverridesFNV.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

void Log(const char* fmt, ...);

namespace VoiceSampleOverridesFNV {
    namespace {
        static std::unordered_map<std::string, VoiceSampleOverride> g_voiceCache;
        static bool g_voiceCacheLoaded = false;
        static std::mutex g_voiceCacheMutex;

        std::string Trim(const std::string& value) {
            const char* whitespace = " \t\r\n";
            const size_t start = value.find_first_not_of(whitespace);
            if (start == std::string::npos) {
                return "";
            }
            const size_t end = value.find_last_not_of(whitespace);
            return value.substr(start, end - start + 1);
        }

        std::string ToLower(std::string value) {
            std::transform(value.begin(), value.end(), value.begin(),
                [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            return value;
        }

        std::string NormalizeVoiceType(const std::string& value) {
            return ToLower(Trim(value));
        }

        bool EndsWith(const std::string& value, const std::string& suffix) {
            if (value.size() < suffix.size()) {
                return false;
            }
            return std::equal(suffix.rbegin(), suffix.rend(), value.rbegin(),
                [](char a, char b) {
                    return std::tolower(static_cast<unsigned char>(a)) ==
                           std::tolower(static_cast<unsigned char>(b));
                });
        }

        std::vector<std::string> ParseCsvLine(const std::string& line) {
            std::vector<std::string> columns;
            std::string column;
            bool inQuotes = false;

            for (size_t i = 0; i < line.size(); ++i) {
                const char ch = line[i];
                if (ch == '"') {
                    if (inQuotes && i + 1 < line.size() && line[i + 1] == '"') {
                        column.push_back('"');
                        ++i;
                    } else {
                        inQuotes = !inQuotes;
                    }
                    continue;
                }

                if (ch == ',' && !inQuotes) {
                    columns.push_back(Trim(column));
                    column.clear();
                    continue;
                }

                column.push_back(ch);
            }

            columns.push_back(Trim(column));
            return columns;
        }

        std::vector<std::string> FindVoiceCSVFiles(const std::string& directoryPath) {
            std::vector<std::string> builtInFiles;
            std::vector<std::string> overrideFiles;

            try {
                if (!std::filesystem::exists(directoryPath)) {
                    Log("VoiceSampleOverridesFNV: Directory does not exist: %s", directoryPath.c_str());
                    return {};
                }

                for (const auto& entry : std::filesystem::directory_iterator(directoryPath)) {
                    if (!entry.is_regular_file()) {
                        continue;
                    }

                    const std::string filename = entry.path().filename().string();
                    if (!EndsWith(filename, "_voices.csv")) {
                        continue;
                    }

                    if (ToLower(filename) == "fallout_builtin_voices.csv") {
                        builtInFiles.push_back(entry.path().string());
                    } else {
                        overrideFiles.push_back(entry.path().string());
                    }
                }
            } catch (const std::exception& e) {
                Log("VoiceSampleOverridesFNV: Error scanning %s: %s", directoryPath.c_str(), e.what());
                return {};
            }

            std::sort(builtInFiles.begin(), builtInFiles.end());
            std::sort(overrideFiles.begin(), overrideFiles.end());
            builtInFiles.insert(builtInFiles.end(), overrideFiles.begin(), overrideFiles.end());
            return builtInFiles;
        }

        std::unordered_map<std::string, VoiceSampleOverride> ParseVoiceCSV(const std::string& filePath) {
            std::unordered_map<std::string, VoiceSampleOverride> voiceMap;
            std::ifstream file(filePath);
            if (!file.is_open()) {
                Log("VoiceSampleOverridesFNV: Failed to open voice CSV: %s", filePath.c_str());
                return voiceMap;
            }

            std::string line;
            int lineNumber = 0;
            bool firstDataLine = true;
            while (std::getline(file, line)) {
                ++lineNumber;
                if (Trim(line).empty()) {
                    continue;
                }

                std::vector<std::string> columns = ParseCsvLine(line);
                if (columns.empty()) {
                    continue;
                }

                if (firstDataLine && NormalizeVoiceType(columns[0]) == "voicetype") {
                    firstDataLine = false;
                    continue;
                }
                firstDataLine = false;

                if (columns.size() < 2) {
                    Log("VoiceSampleOverridesFNV: Skipping malformed line %d in %s", lineNumber, filePath.c_str());
                    continue;
                }

                VoiceSampleOverride mapping;
                mapping.voiceType = NormalizeVoiceType(columns[0]);
                mapping.voiceFile = Trim(columns[1]);
                mapping.transcript = (columns.size() >= 3) ? Trim(columns[2]) : "";
                mapping.sourceFile = std::filesystem::path(filePath).filename().string();

                if (mapping.voiceType.empty() || mapping.voiceFile.empty()) {
                    Log("VoiceSampleOverridesFNV: Skipping empty voice mapping line %d in %s", lineNumber, filePath.c_str());
                    continue;
                }

                voiceMap[mapping.voiceType] = mapping;
            }

            Log("VoiceSampleOverridesFNV: Parsed %zu voice mappings from %s", voiceMap.size(), filePath.c_str());
            return voiceMap;
        }
    }

    void LoadVoiceCSVData() {
        std::lock_guard<std::mutex> lock(g_voiceCacheMutex);
        if (g_voiceCacheLoaded) {
            return;
        }

        g_voiceCache.clear();
        const std::string dialecticPath = "Data\\Dialectic";
        const std::vector<std::string> voiceFiles = FindVoiceCSVFiles(dialecticPath);
        if (voiceFiles.empty()) {
            Log("VoiceSampleOverridesFNV: No *_voices.csv files found in %s", dialecticPath.c_str());
            g_voiceCacheLoaded = true;
            return;
        }

        int totalMappings = 0;
        for (const std::string& filePath : voiceFiles) {
            auto fileMap = ParseVoiceCSV(filePath);
            for (const auto& mapping : fileMap) {
                auto existing = g_voiceCache.find(mapping.first);
                if (existing != g_voiceCache.end()) {
                    Log("VoiceSampleOverridesFNV: Duplicate voice type %s replaced %s with %s",
                        mapping.first.c_str(),
                        existing->second.voiceFile.c_str(),
                        mapping.second.voiceFile.c_str());
                }
                g_voiceCache[mapping.first] = mapping.second;
                ++totalMappings;
            }
        }

        g_voiceCacheLoaded = true;
        Log("VoiceSampleOverridesFNV: Loaded %zu unique voice mappings from %zu files (%d total rows)",
            g_voiceCache.size(),
            voiceFiles.size(),
            totalMappings);
    }

    bool FindVoiceSampleOverride(const std::string& voiceType, VoiceSampleOverride& result) {
        if (!g_voiceCacheLoaded) {
            LoadVoiceCSVData();
        }

        const std::string key = NormalizeVoiceType(voiceType);
        if (key.empty()) {
            return false;
        }

        std::lock_guard<std::mutex> lock(g_voiceCacheMutex);
        auto it = g_voiceCache.find(key);
        if (it == g_voiceCache.end()) {
            return false;
        }

        result = it->second;
        Log("VoiceSampleOverridesFNV: Found voice mapping for %s -> %s",
            key.c_str(),
            result.voiceFile.c_str());
        return true;
    }

    std::vector<VoiceSampleOverride> GetAllVoiceSampleOverrides() {
        if (!g_voiceCacheLoaded) {
            LoadVoiceCSVData();
        }

        std::vector<VoiceSampleOverride> mappings;
        std::lock_guard<std::mutex> lock(g_voiceCacheMutex);
        mappings.reserve(g_voiceCache.size());
        for (const auto& pair : g_voiceCache) {
            mappings.push_back(pair.second);
        }

        std::sort(mappings.begin(), mappings.end(),
            [](const VoiceSampleOverride& left, const VoiceSampleOverride& right) {
                return left.voiceType < right.voiceType;
            });
        return mappings;
    }

    void ReloadVoiceCSVData() {
        std::lock_guard<std::mutex> lock(g_voiceCacheMutex);
        g_voiceCache.clear();
        g_voiceCacheLoaded = false;
        Log("VoiceSampleOverridesFNV: Voice CSV cache cleared");
    }
}
