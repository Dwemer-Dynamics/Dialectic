#include "AgentManager.h"
#include "HTTPManager.h"
#include "Config.h"
#include "VoiceSampleOverridesFNV.h"
#include "VoiceSampleResolverFNV.h"
#include "GameThreadDispatcher.h"
#include "RuntimeGeneration.h"
#include "RuntimeSnapshot.h"
#include "TaskManager.h"
#include "XNVSEAdapter.h"
#include "PlayerInventoryManagerFNV.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <vector>
#include <memory>
#include <mutex>
#include <string>
#include <sstream>
#include <iomanip>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <thread>
#include <chrono>
#include <future>

// Forward declare logging function
void Log(const char* fmt, ...);

namespace AgentManager {

    // Simple agent tracking without NVSE dependencies
    struct AIAgent {
        std::string actorName;
        uint32_t formID;
        bool isTalking;
        bool isLocked;
        bool isNarrator;
        float lastDistance;
        bool manuallyActivated;
        bool autoManaged;
        int boredEventsFired;

        AIAgent(const std::string& name, uint32_t id, RegistrationSource source, float distance)
            : actorName(name), formID(id), isTalking(false), 
              isLocked(false), isNarrator(false), lastDistance(distance),
              manuallyActivated(source == RegistrationSource::Manual),
              autoManaged(source == RegistrationSource::Auto),
              boredEventsFired(0) {}
    };

    static std::vector<std::shared_ptr<AIAgent>> g_agents;
    static std::unordered_map<uint32_t, std::string> g_registeredAgents; // formID -> name
    static std::mutex g_agentsMutex;
    static std::mutex g_voiceSampleUploadMutex;
    static std::unordered_set<std::string> g_attemptedVoiceSampleUploads;
    static std::mutex g_metadataHashMutex;
    static std::unordered_map<uint32_t, std::string> g_lastEquipmentHash;
    static std::unordered_map<uint32_t, std::string> g_lastInventoryHash;
    static bool g_initialized = false;
    static const char* kActorSnapshotFileName = "dialectic_actor_snapshot.tmp";
    static const char* kActorSnapshotRequestFileName = "dialectic_actor_snapshot_request.tmp";
    static const char* kActorSnapshotStatusFileName = "dialectic_actor_snapshot_status.txt";

    static bool IsPlayerReference(uint32_t refID) {
        return refID == 0x00000014;
    }

    void Initialize() {
        ClearActorSnapshotRequest();
        std::lock_guard<std::mutex> lock(g_agentsMutex);
        g_agents.clear();
        g_registeredAgents.clear();
        {
            std::lock_guard<std::mutex> voiceLock(g_voiceSampleUploadMutex);
            g_attemptedVoiceSampleUploads.clear();
        }
        {
            std::lock_guard<std::mutex> metadataLock(g_metadataHashMutex);
            g_lastEquipmentHash.clear();
            g_lastInventoryHash.clear();
        }
        g_initialized = true;
        Log("AgentManager: Initialized");
    }

    void Shutdown() {
        std::lock_guard<std::mutex> lock(g_agentsMutex);
        g_agents.clear();
        g_registeredAgents.clear();
        {
            std::lock_guard<std::mutex> voiceLock(g_voiceSampleUploadMutex);
            g_attemptedVoiceSampleUploads.clear();
        }
        {
            std::lock_guard<std::mutex> metadataLock(g_metadataHashMutex);
            g_lastEquipmentHash.clear();
            g_lastInventoryHash.clear();
        }
        g_initialized = false;
        Log("AgentManager: Shutdown");
    }

    static std::shared_ptr<AIAgent> FindAgentLocked(uint32_t formID) {
        for (auto& agent : g_agents) {
            if (agent && agent->formID == formID) {
                return agent;
            }
        }
        return nullptr;
    }
    
    bool IsAIAgent(uint32_t formID) {
        std::lock_guard<std::mutex> lock(g_agentsMutex);
        return g_registeredAgents.find(formID) != g_registeredAgents.end();
    }
    
    void RegisterAIAgent(uint32_t formID, const std::string& name) {
        RegisterAIAgent(formID, name, RegistrationSource::Auto, 0.0f);
    }

    void RegisterAIAgent(uint32_t formID, const std::string& name, RegistrationSource source, float distance) {
        std::lock_guard<std::mutex> lock(g_agentsMutex);
        if (formID == 0) {
            return;
        }

        std::shared_ptr<AIAgent> agent = FindAgentLocked(formID);
        if (!agent) {
            agent = std::make_shared<AIAgent>(name, formID, source, distance);
            g_agents.push_back(agent);
        } else {
            if (!name.empty()) {
                agent->actorName = name;
            }
            if (source == RegistrationSource::Manual) {
                agent->manuallyActivated = true;
            } else {
                agent->autoManaged = true;
            }
            if (distance > 0.0f) {
                agent->lastDistance = distance;
            }
        }

        g_registeredAgents[formID] = agent->actorName;
        Log("AgentManager: Registered AI agent: %s (0x%08X, manual=%d, auto=%d)",
            agent->actorName.c_str(), formID, agent->manuallyActivated ? 1 : 0, agent->autoManaged ? 1 : 0);
    }

    bool UnregisterAIAgent(uint32_t formID) {
        if (formID == 0) {
            return false;
        }

        std::string removedName;
        {
            std::lock_guard<std::mutex> lock(g_agentsMutex);
            auto registered = g_registeredAgents.find(formID);
            if (registered == g_registeredAgents.end()) {
                return false;
            }
            removedName = registered->second;
            g_registeredAgents.erase(registered);
            g_agents.erase(
                std::remove_if(g_agents.begin(), g_agents.end(),
                    [formID](const std::shared_ptr<AIAgent>& agent) {
                        return !agent || agent->formID == formID;
                    }),
                g_agents.end());
        }

        {
            std::lock_guard<std::mutex> metadataLock(g_metadataHashMutex);
            g_lastEquipmentHash.erase(formID);
            g_lastInventoryHash.erase(formID);
        }

        Log("AgentManager: Unregistered AI agent: %s (0x%08X)",
            removedName.empty() ? "<unknown>" : removedName.c_str(), formID);
        return true;
    }

    std::size_t UnregisterAllAIAgents() {
        std::size_t removed = 0;
        {
            std::lock_guard<std::mutex> lock(g_agentsMutex);
            removed = g_registeredAgents.size();
            g_registeredAgents.clear();
            g_agents.clear();
        }

        {
            std::lock_guard<std::mutex> metadataLock(g_metadataHashMutex);
            g_lastEquipmentHash.clear();
            g_lastInventoryHash.clear();
        }

        Log("AgentManager: Unregistered all AI agents: %zu", removed);
        return removed;
    }

    void MarkAgentSeen(uint32_t formID, float distance) {
        std::lock_guard<std::mutex> lock(g_agentsMutex);
        std::shared_ptr<AIAgent> agent = FindAgentLocked(formID);
        if (agent && distance > 0.0f) {
            agent->lastDistance = distance;
        }
    }

    bool IsManuallyActivated(uint32_t formID) {
        std::lock_guard<std::mutex> lock(g_agentsMutex);
        std::shared_ptr<AIAgent> agent = FindAgentLocked(formID);
        return agent && agent->manuallyActivated;
    }

    bool IsAutoManaged(uint32_t formID) {
        std::lock_guard<std::mutex> lock(g_agentsMutex);
        std::shared_ptr<AIAgent> agent = FindAgentLocked(formID);
        return agent && agent->autoManaged;
    }

    static std::string NormalizeName(const std::string& value) {
        std::string normalized = value;
        std::transform(normalized.begin(), normalized.end(), normalized.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        normalized.erase(normalized.begin(), std::find_if(normalized.begin(), normalized.end(),
            [](unsigned char ch) { return !std::isspace(ch); }));
        normalized.erase(std::find_if(normalized.rbegin(), normalized.rend(),
            [](unsigned char ch) { return !std::isspace(ch); }).base(), normalized.end());
        return normalized;
    }

    std::string GetAgentName(uint32_t formID) {
        std::lock_guard<std::mutex> lock(g_agentsMutex);
        std::shared_ptr<AIAgent> agent = FindAgentLocked(formID);
        return agent ? agent->actorName : "";
    }

    uint32_t FindAgentFormIdByName(const std::string& name) {
        const std::string target = NormalizeName(name);
        if (target.empty()) {
            return 0;
        }

        std::lock_guard<std::mutex> lock(g_agentsMutex);
        for (const auto& agent : g_agents) {
            if (agent && NormalizeName(agent->actorName) == target) {
                return agent->formID;
            }
        }
        return 0;
    }

    std::vector<std::pair<uint32_t, std::string>> GetRegisteredAgentSnapshot() {
        std::vector<std::pair<uint32_t, std::string>> snapshot;
        std::lock_guard<std::mutex> lock(g_agentsMutex);
        snapshot.reserve(g_agents.size());
        for (const auto& agent : g_agents) {
            if (agent && agent->formID != 0 && !agent->actorName.empty()) {
                snapshot.emplace_back(agent->formID, agent->actorName);
            }
        }
        return snapshot;
    }

    bool SelectLeastBoredNearbyAgent(float maxDistance, uint32_t& outFormID, std::string& outName) {
        outFormID = 0;
        outName.clear();

        std::vector<std::shared_ptr<AIAgent>> candidates;
        std::lock_guard<std::mutex> lock(g_agentsMutex);
        for (const auto& agent : g_agents) {
            if (!agent || agent->formID == 0 || agent->actorName.empty() || agent->isNarrator) {
                continue;
            }
            if (maxDistance > 0.0f && agent->lastDistance > 0.0f && agent->lastDistance > maxDistance) {
                continue;
            }
            candidates.push_back(agent);
        }

        if (candidates.empty()) {
            return false;
        }

        std::sort(candidates.begin(), candidates.end(),
            [](const std::shared_ptr<AIAgent>& left, const std::shared_ptr<AIAgent>& right) {
                if (left->boredEventsFired != right->boredEventsFired) {
                    return left->boredEventsFired < right->boredEventsFired;
                }
                return left->lastDistance < right->lastDistance;
            });

        outFormID = candidates.front()->formID;
        outName = candidates.front()->actorName;
        return outFormID != 0 && !outName.empty();
    }

    bool SelectLeastBoredNearbyAgent(
        const std::vector<std::pair<uint32_t, std::string>>& candidateRefs,
        uint32_t& outFormID,
        std::string& outName) {
        outFormID = 0;
        outName.clear();

        struct Candidate {
            std::shared_ptr<AIAgent> agent;
            size_t order = 0;
        };

        std::vector<Candidate> candidates;
        std::lock_guard<std::mutex> lock(g_agentsMutex);
        for (size_t i = 0; i < candidateRefs.size(); ++i) {
            const uint32_t formID = candidateRefs[i].first;
            if (formID == 0) {
                continue;
            }

            std::shared_ptr<AIAgent> agent = FindAgentLocked(formID);
            if (!agent || agent->actorName.empty() || agent->isNarrator) {
                continue;
            }

            if (!candidateRefs[i].second.empty()) {
                agent->actorName = candidateRefs[i].second;
            }
            candidates.push_back({ agent, i });
        }

        if (candidates.empty()) {
            return false;
        }

        std::sort(candidates.begin(), candidates.end(),
            [](const Candidate& left, const Candidate& right) {
                if (left.agent->boredEventsFired != right.agent->boredEventsFired) {
                    return left.agent->boredEventsFired < right.agent->boredEventsFired;
                }
                return left.order < right.order;
            });

        outFormID = candidates.front().agent->formID;
        outName = candidates.front().agent->actorName;
        return outFormID != 0 && !outName.empty();
    }

    void MarkBoredEventFired(uint32_t formID) {
        std::lock_guard<std::mutex> lock(g_agentsMutex);
        std::shared_ptr<AIAgent> agent = FindAgentLocked(formID);
        if (!agent) {
            return;
        }

        ++agent->boredEventsFired;
        if (agent->boredEventsFired > 5) {
            agent->boredEventsFired = 0;
        }
    }

    // Helper: Read memory at address (with safety checks)
    // Note: Removed __try/__except due to C++ object unwinding conflicts
    // Will use nullptr checks instead
    template<typename T>
    static T ReadMemorySafe(void* address, T defaultValue = T()) {
        if (!address) return defaultValue;
        // Direct read - caller must ensure valid pointer
        return *static_cast<T*>(address);
    }

    // Helper: Get C-string from memory (with safety checks)
    static std::string ReadStringSafe(void* address) {
        if (!address) return "";
        
        const char* str = static_cast<const char*>(address);
        if (!str) return "";
        
        // Limit string length for safety
        std::string result;
        for (int i = 0; i < 256 && str[i] != '\0'; ++i) {
            result += str[i];
        }
        return result;
    }

    static void ApplyFallbackDefaults(NPCData& data) {
        if (data.displayName.empty()) data.displayName = "Unknown NPC";
        if (data.baseName.empty()) data.baseName = "UnknownBase";
        if (data.gender.empty()) data.gender = "Unknown";
        if (data.race.empty()) data.race = "Unknown";
        if (data.strength <= 0) data.strength = 5;
        if (data.perception <= 0) data.perception = 5;
        if (data.endurance <= 0) data.endurance = 5;
        if (data.charisma <= 0) data.charisma = 5;
        if (data.intelligence <= 0) data.intelligence = 5;
        if (data.agility <= 0) data.agility = 5;
        if (data.luck <= 0) data.luck = 5;
        if (data.level <= 0) data.level = 1;
        if (data.health <= 0) data.health = 100.0f;
        if (data.healthMax <= 0) data.healthMax = data.health;
        if (data.actionPoints <= 0) data.actionPoints = 65.0f;
        if (data.actionPointsMax <= 0) data.actionPointsMax = data.actionPoints;
        if (data.scale <= 0.0f) data.scale = 1.0f;
    }

    static std::string Trim(const std::string& value) {
        const char* whitespace = " \t\r\n";
        const size_t start = value.find_first_not_of(whitespace);
        if (start == std::string::npos) return "";
        const size_t end = value.find_last_not_of(whitespace);
        std::string trimmed = value.substr(start, end - start + 1);
        while (trimmed.size() >= 2) {
            const std::string suffix = trimmed.substr(trimmed.size() - 2);
            if (suffix != "\\n" && suffix != "\\r") {
                break;
            }
            trimmed.erase(trimmed.size() - 2);
            const size_t nextEnd = trimmed.find_last_not_of(whitespace);
            if (nextEnd == std::string::npos) {
                return "";
            }
            trimmed.erase(nextEnd + 1);
        }
        return trimmed;
    }

    static bool IsMissingSnapshotValue(const std::string& value) {
        const std::string trimmed = Trim(value);
        std::string lower;
        lower.reserve(trimmed.size());
        for (char ch : trimmed) {
            lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        }
        return trimmed.empty()
            || lower == "unknown"
            || lower == "unknownbase";
    }

    static uint32_t ParseHexFormID(const std::string& value) {
        std::string cleaned = Trim(value);
        if (cleaned.starts_with("0x") || cleaned.starts_with("0X")) {
            cleaned = cleaned.substr(2);
        }

        try {
            return static_cast<uint32_t>(std::stoul(cleaned, nullptr, 16));
        } catch (...) {
            return 0;
        }
    }

    static int ParseInt(const std::string& value, int fallback = 0) {
        try {
            return std::stoi(Trim(value));
        } catch (...) {
            return fallback;
        }
    }

    static float ParseFloat(const std::string& value, float fallback = 0.0f) {
        try {
            return std::stof(Trim(value));
        } catch (...) {
            return fallback;
        }
    }

    struct SnapshotLine {
        std::string key;
        std::string value;
    };

    static std::vector<std::string> ActorSnapshotBridgePaths(const char* fileName) {
        std::vector<std::string> paths;
        if (!fileName || !fileName[0]) {
            return paths;
        }

        paths.push_back(std::string("Data\\NVSE\\Plugins\\") + fileName);
        paths.push_back(std::string("NVSE\\Plugins\\") + fileName);

        const char* localAppData = std::getenv("LOCALAPPDATA");
        if (localAppData && localAppData[0]) {
            const std::string mo2Overwrite = std::string(localAppData) + "\\ModOrganizer\\Fallout TTW\\overwrite\\";
            paths.push_back(mo2Overwrite + "NVSE\\Plugins\\" + fileName);
            paths.push_back(mo2Overwrite + "Data\\NVSE\\Plugins\\" + fileName);
        }

        return paths;
    }

    static void RemoveActorSnapshotBridgeFile(const char* fileName) {
        for (const auto& path : ActorSnapshotBridgePaths(fileName)) {
            std::remove(path.c_str());
        }
    }

    static std::vector<SnapshotLine> ReadSnapshotLinesFromPath(const std::string& path) {
        std::vector<SnapshotLine> lines;
        std::ifstream input(path, std::ios::binary);
        if (!input.is_open()) {
            return lines;
        }

        std::string line;
        while (std::getline(input, line)) {
            size_t separator = line.find('=');
            if (separator == std::string::npos) {
                continue;
            }

            std::string key = Trim(line.substr(0, separator));
            std::string value = Trim(line.substr(separator + 1));
            if (key.empty()) {
                continue;
            }

            std::transform(key.begin(), key.end(), key.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            lines.push_back({ key, value });
        }

        input.close();
        return lines;
    }

    static std::vector<SnapshotLine> ReadSnapshotLines() {
        for (const auto& path : ActorSnapshotBridgePaths(kActorSnapshotFileName)) {
            std::vector<SnapshotLine> lines = ReadSnapshotLinesFromPath(path);
            if (!lines.empty()) {
                Log("AgentManager: Read actor snapshot from %s (%zu fields)", path.c_str(), lines.size());
                return lines;
            }
        }

        return {};
    }

    static std::unordered_map<std::string, std::string> SnapshotFieldsFromLines(const std::vector<SnapshotLine>& lines) {
        std::unordered_map<std::string, std::string> fields;
        for (const auto& line : lines) {
            if (line.key != "inventory" && line.key != "equipment") {
                fields[line.key] = line.value;
            }
        }
        return fields;
    }

    static bool GetFileModifiedAgeMs(const char* path, uint64_t& ageMs) {
        WIN32_FILE_ATTRIBUTE_DATA attributes = {};
        if (!GetFileAttributesExA(path, GetFileExInfoStandard, &attributes)) {
            return false;
        }

        FILETIME currentFileTime = {};
        GetSystemTimeAsFileTime(&currentFileTime);

        ULARGE_INTEGER current = {};
        current.LowPart = currentFileTime.dwLowDateTime;
        current.HighPart = currentFileTime.dwHighDateTime;

        ULARGE_INTEGER modified = {};
        modified.LowPart = attributes.ftLastWriteTime.dwLowDateTime;
        modified.HighPart = attributes.ftLastWriteTime.dwHighDateTime;

        if (current.QuadPart <= modified.QuadPart) {
            ageMs = 0;
        } else {
            ageMs = (current.QuadPart - modified.QuadPart) / 10000;
        }
        return true;
    }

    static bool IsSnapshotFileFresh(const std::string& path, int maxAgeMs) {
        uint64_t ageMs = 0;
        if (!GetFileModifiedAgeMs(path.c_str(), ageMs)) {
            return false;
        }
        return ageMs <= static_cast<uint64_t>(maxAgeMs);
    }

    static bool FindFreshSnapshotPath(int maxAgeMs, std::string& outPath) {
        for (const auto& path : ActorSnapshotBridgePaths(kActorSnapshotFileName)) {
            if (IsSnapshotFileFresh(path, maxAgeMs)) {
                outPath = path;
                return true;
            }
        }

        outPath.clear();
        return false;
    }

    static uint32_t ReadSnapshotRefID() {
        auto snapshotLines = ReadSnapshotLines();
        if (snapshotLines.empty()) {
            return 0;
        }

        auto fields = SnapshotFieldsFromLines(snapshotLines);
        auto refIt = fields.find("refid");
        return (refIt != fields.end()) ? ParseHexFormID(refIt->second) : 0;
    }

    void RequestActorSnapshot(uint32_t refID, const std::string& npcName) {
        if (IsPlayerReference(refID)) {
            Log("AgentManager: Refused NPC actor snapshot request for player ref 0x%08X", refID);
            PlayerInventoryManagerFNV::ForceRefresh("snapshot_request_player_guard", 0);
            return;
        }
        RemoveActorSnapshotBridgeFile(kActorSnapshotFileName);
        RemoveActorSnapshotBridgeFile(kActorSnapshotStatusFileName);

        int written = 0;
        for (const auto& path : ActorSnapshotBridgePaths(kActorSnapshotRequestFileName)) {
            std::ofstream request(path, std::ios::binary | std::ios::trunc);
            if (!request.is_open()) {
                continue;
            }

            request << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << refID
                    << std::dec << "\n";
            request << npcName << "\n";
            request << ((refID >> 24) & 0xFF) << "\n";
            request << (refID & 0x00FFFFFF) << "\n";
            request.close();
            ++written;
        }

        if (written <= 0) {
            Log("AgentManager: Failed to write any actor snapshot request for 0x%08X", refID);
            return;
        }

        Log("AgentManager: Requested actor snapshot for %s (0x%08X) via %d bridge path(s)",
            npcName.c_str(), refID, written);
    }

    void ClearActorSnapshotRequest() {
        RemoveActorSnapshotBridgeFile(kActorSnapshotRequestFileName);
    }

    bool WaitForFreshActorSnapshot(uint32_t expectedRefID, int timeoutMs,
                                   const std::function<bool()>& cancelRequested) {
        if (expectedRefID == 0) {
            return false;
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() <= deadline) {
            if (cancelRequested && cancelRequested()) {
                Log("AgentManager: Actor snapshot wait cancelled for 0x%08X", expectedRefID);
                return false;
            }
            std::string snapshotPath;
            if (FindFreshSnapshotPath(timeoutMs, snapshotPath)) {
                uint32_t snapshotRefID = ReadSnapshotRefID();
                if (snapshotRefID == expectedRefID) {
                    Log("AgentManager: Found fresh actor snapshot for 0x%08X at %s", expectedRefID, snapshotPath.c_str());
                    return true;
                }

                if (snapshotRefID != 0) {
                    Log("AgentManager: Waiting for actor snapshot 0x%08X, current snapshot is 0x%08X",
                        expectedRefID, snapshotRefID);
                }
            }

            if (const TaskManager::CancellationToken* token = TaskManager::CurrentToken()) {
                if (!token->WaitFor(std::chrono::milliseconds(25))) return false;
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
        }

        Log("AgentManager: Timed out waiting for fresh actor snapshot for 0x%08X", expectedRefID);
        return false;
    }

    static std::vector<std::string> Split(const std::string& value, char delimiter) {
        std::vector<std::string> parts;
        std::string current;
        std::istringstream stream(value);
        while (std::getline(stream, current, delimiter)) {
            parts.push_back(current);
        }
        return parts;
    }

    static std::string ExtractBaseIdFromEquipment(const std::string& value) {
        size_t separator = value.find('^');
        if (separator == std::string::npos || separator + 1 >= value.size()) {
            return "";
        }
        return Trim(value.substr(separator + 1));
    }

    static bool EqualsIgnoreCase(const std::string& left, const std::string& right) {
        if (left.size() != right.size()) {
            return false;
        }

        for (size_t i = 0; i < left.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(left[i])) !=
                std::tolower(static_cast<unsigned char>(right[i]))) {
                return false;
            }
        }

        return true;
    }

    static bool IsInventoryBaseEquipped(const std::string& baseid, const NPCData& data) {
        if (baseid.empty()) {
            return false;
        }

        for (const auto& equippedItem : data.equipment) {
            if (!equippedItem.baseid.empty() && EqualsIgnoreCase(equippedItem.baseid, baseid)) {
                return true;
            }
        }

        const std::string equippedIds[] = {
            ExtractBaseIdFromEquipment(data.head),
            ExtractBaseIdFromEquipment(data.upperBody),
            ExtractBaseIdFromEquipment(data.leftHand),
            ExtractBaseIdFromEquipment(data.rightHand),
            ExtractBaseIdFromEquipment(data.hair),
            ExtractBaseIdFromEquipment(data.weapon),
            ExtractBaseIdFromEquipment(data.upperBodyAddon),
            ExtractBaseIdFromEquipment(data.lowerBodyAddon),
        };

        for (const auto& equippedId : equippedIds) {
            if (!equippedId.empty() && EqualsIgnoreCase(equippedId, baseid)) {
                return true;
            }
        }

        return false;
    }

    static const char* EquipmentSlotName(int slot) {
        switch (slot) {
            case 0: return "face";
            case 1: return "head";
            case 2: return "body";
            case 3: return "left_hand";
            case 4: return "right_hand";
            case 5: return "weapon";
            case 6: return "pip_boy";
            case 7: return "backpack";
            case 8: return "necklace";
            case 9: return "headband";
            case 10: return "hat";
            case 11: return "eyeglasses";
            case 12: return "nosering";
            case 13: return "earrings";
            case 14: return "mask";
            case 15: return "choker";
            case 16: return "mouth_object";
            case 17: return "body_addon_1";
            case 18: return "body_addon_2";
            case 19: return "body_addon_3";
            default: return "unknown";
        }
    }

    static void ApplyLegacyEquipmentField(NPCData& data, int slot, const std::string& value) {
        switch (slot) {
            case 0: data.head = value; break;
            case 1: data.hair = value; break;
            case 2: data.upperBody = value; break;
            case 3: data.leftHand = value; break;
            case 4: data.rightHand = value; break;
            case 5: data.weapon = value; break;
            case 17: data.upperBodyAddon = value; break;
            case 18: data.lowerBodyAddon = value; break;
            default: break;
        }
    }

    static void ApplySnapshotEquipment(const std::vector<SnapshotLine>& lines, NPCData& data) {
        data.equipment.clear();

        for (const auto& line : lines) {
            if (line.key != "equipment") {
                continue;
            }

            std::vector<std::string> parts = Split(line.value, '^');
            if (parts.size() < 3) {
                continue;
            }

            EquipmentItem item;
            item.slot = ParseInt(parts[0], -1);
            item.slotName = EquipmentSlotName(item.slot);
            item.name = Trim(parts[1]);
            item.baseid = Trim(parts[2]);
            if (parts.size() >= 4) {
                item.condition = ParseFloat(parts[3], -1.0f);
            }

            if (item.slot >= 0 && item.slot < 20 && !item.name.empty() && !item.baseid.empty()) {
                data.equipment.push_back(item);
                ApplyLegacyEquipmentField(data, item.slot, item.name + "^" + item.baseid);
            }
        }
    }

    static void ApplySnapshotInventory(const std::vector<SnapshotLine>& lines, NPCData& data) {
        data.inventory.clear();

        for (const auto& line : lines) {
            if (line.key != "inventory") {
                continue;
            }

            std::vector<std::string> parts = Split(line.value, '^');
            if (parts.size() < 3) {
                continue;
            }

            InventoryItem item;
            item.name = Trim(parts[0]);
            item.baseid = Trim(parts[1]);
            item.count = ParseInt(parts[2], 0);
            if (parts.size() >= 4) {
                item.type = ParseInt(parts[3], 0);
            }
            if (parts.size() >= 5) {
                item.condition = ParseFloat(parts[4], -1.0f);
            }
            if (parts.size() >= 6) {
                item.ammo = Trim(parts[5]);
            }
            if (parts.size() >= 7 && !Trim(parts[6]).empty()) {
                item.mods = Split(parts[6], ',');
            }

            item.equipped = IsInventoryBaseEquipped(item.baseid, data);
            if (!item.name.empty() && !item.baseid.empty() && item.count > 0) {
                data.inventory.push_back(item);
            }
        }
    }

    static void ApplySnapshotString(
        const std::unordered_map<std::string, std::string>& fields,
        const char* key,
        std::string& target) {
        auto it = fields.find(key);
        if (it != fields.end() && !IsMissingSnapshotValue(it->second)) {
            target = Trim(it->second);
        }
    }

    static void ApplySnapshotStringAny(
        const std::unordered_map<std::string, std::string>& fields,
        const std::vector<const char*>& keys,
        std::string& target) {
        for (const char* key : keys) {
            ApplySnapshotString(fields, key, target);
        }
    }

    static void ApplySnapshotInt(
        const std::unordered_map<std::string, std::string>& fields,
        const char* key,
        int& target) {
        auto it = fields.find(key);
        if (it != fields.end() && !it->second.empty()) {
            target = ParseInt(it->second, target);
        }
    }

    static void ApplySnapshotFloat(
        const std::unordered_map<std::string, std::string>& fields,
        const char* key,
        float& target) {
        auto it = fields.find(key);
        if (it != fields.end() && !it->second.empty()) {
            target = ParseFloat(it->second, target);
        }
    }

    static bool LoadSnapshotNPCData(uint32_t expectedRefID, NPCData& data, bool requireFreshSnapshot = false, int maxAgeMs = 1000) {
        if (requireFreshSnapshot) {
            std::string snapshotPath;
            if (!FindFreshSnapshotPath(maxAgeMs, snapshotPath)) {
                Log("AgentManager: Actor snapshot is not fresh enough for 0x%08X", expectedRefID);
                return false;
            }
        }

        auto snapshotLines = ReadSnapshotLines();
        if (snapshotLines.empty()) {
            return false;
        }
        auto fields = SnapshotFieldsFromLines(snapshotLines);

        auto refIt = fields.find("refid");
        uint32_t snapshotRefID = (refIt != fields.end()) ? ParseHexFormID(refIt->second) : 0;
        if (expectedRefID != 0 && snapshotRefID != expectedRefID) {
            Log("AgentManager: Ignoring stale actor snapshot for 0x%08X, expected 0x%08X",
                snapshotRefID, expectedRefID);
            return false;
        }

        if (snapshotRefID != 0) {
            data.refID = snapshotRefID;
        }
        ApplySnapshotString(fields, "name", data.displayName);
        ApplySnapshotString(fields, "baseid", data.baseName);
        ApplySnapshotString(fields, "gender", data.gender);
        ApplySnapshotString(fields, "race", data.race);
        ApplySnapshotString(fields, "voiceid", data.voiceId);
        ApplySnapshotString(fields, "voice_formid", data.voiceFormId);
        ApplySnapshotString(fields, "voice_name", data.voiceName);

        ApplySnapshotInt(fields, "strength", data.strength);
        ApplySnapshotInt(fields, "perception", data.perception);
        ApplySnapshotInt(fields, "endurance", data.endurance);
        ApplySnapshotInt(fields, "charisma", data.charisma);
        ApplySnapshotInt(fields, "intelligence", data.intelligence);
        ApplySnapshotInt(fields, "agility", data.agility);
        ApplySnapshotInt(fields, "luck", data.luck);

        ApplySnapshotInt(fields, "barter", data.barter);
        ApplySnapshotInt(fields, "energy_weapons", data.energyWeapons);
        ApplySnapshotInt(fields, "explosives", data.explosives);
        ApplySnapshotInt(fields, "guns", data.guns);
        ApplySnapshotInt(fields, "lockpick", data.lockpick);
        ApplySnapshotInt(fields, "medicine", data.medicine);
        ApplySnapshotInt(fields, "melee_weapons", data.meleeWeapons);
        ApplySnapshotInt(fields, "repair", data.repair);
        ApplySnapshotInt(fields, "science", data.science);
        ApplySnapshotInt(fields, "sneak", data.sneak);
        ApplySnapshotInt(fields, "speech", data.speech);
        ApplySnapshotInt(fields, "survival", data.survival);
        ApplySnapshotInt(fields, "unarmed", data.unarmed);

        ApplySnapshotStringAny(fields, { "head", "face" }, data.head);
        ApplySnapshotStringAny(fields, { "upper_body", "body" }, data.upperBody);
        ApplySnapshotString(fields, "left_hand", data.leftHand);
        ApplySnapshotString(fields, "right_hand", data.rightHand);
        ApplySnapshotStringAny(fields, { "hair", "head" }, data.hair);
        ApplySnapshotString(fields, "weapon", data.weapon);
        ApplySnapshotString(fields, "upper_body_addon", data.upperBodyAddon);
        ApplySnapshotString(fields, "lower_body_addon", data.lowerBodyAddon);
        ApplySnapshotEquipment(snapshotLines, data);

        ApplySnapshotInt(fields, "level", data.level);
        ApplySnapshotFloat(fields, "health", data.health);
        ApplySnapshotFloat(fields, "health_max", data.healthMax);
        ApplySnapshotFloat(fields, "action_points", data.actionPoints);
        ApplySnapshotFloat(fields, "action_points_max", data.actionPointsMax);
        ApplySnapshotFloat(fields, "scale", data.scale);
        ApplySnapshotInt(fields, "xp", data.xp);
        ApplySnapshotInt(fields, "karma", data.karma);
        ApplySnapshotInventory(snapshotLines, data);

        Log("AgentManager: Loaded actor snapshot for %s (0x%08X) with %zu equipment slots and %zu inventory items",
            data.displayName.c_str(), data.refID, data.equipment.size(), data.inventory.size());
        return true;
    }

    static NPCData CollectNPCDataWithSnapshotPolicy(uint32_t expectedRefID, bool requireFreshSnapshot, int maxAgeMs) {
        NPCData data;
        if (expectedRefID != 0) {
            data.refID = expectedRefID;
        }

        RuntimeSnapshot::ActorState nativeActor;
        if (expectedRefID != 0 && RuntimeSnapshot::TryGetActor(expectedRefID, nativeActor)) {
            auto formatFormId = [](uint32_t formId) {
                if (formId == 0) return std::string{};
                std::ostringstream out;
                out << "0x" << std::hex << std::setw(8) << std::setfill('0') << formId;
                return out.str();
            };

            data.displayName = nativeActor.name;
            data.baseName = formatFormId(nativeActor.baseFormId);
            data.gender = nativeActor.baseType == 0x2A ? (nativeActor.female ? "Female" : "Male") : "";
            data.race = nativeActor.raceName;
            data.voiceId = nativeActor.voiceName;
            data.voiceFormId = formatFormId(nativeActor.voiceFormId);
            data.voiceName = nativeActor.voiceName;
            data.level = nativeActor.level;
            data.health = nativeActor.health;
            data.healthMax = nativeActor.healthMax;
            data.actionPoints = nativeActor.actionPoints;
            data.actionPointsMax = nativeActor.actionPointsMax;
            data.scale = nativeActor.scale;

            data.equipment.reserve(nativeActor.equipment.size());
            for (const auto& nativeItem : nativeActor.equipment) {
                EquipmentItem item;
                item.slot = -1;
                item.slotName = "Equipped";
                item.name = nativeItem.name;
                item.baseid = formatFormId(nativeItem.baseFormId);
                item.condition = nativeItem.condition;
                data.equipment.push_back(std::move(item));
            }

            struct NativeInventoryCapture {
                bool ok{false};
                std::vector<XNVSEAdapter::NativeInventoryItem> items;
            };
            auto capture = std::make_shared<std::promise<NativeInventoryCapture>>();
            std::future<NativeInventoryCapture> future = capture->get_future();
            auto captureWork = [capture, expectedRefID]() {
                NativeInventoryCapture result;
                result.ok = XNVSEAdapter::CaptureNativeInventory(expectedRefID, result.items);
                capture->set_value(std::move(result));
            };

            if (GameThreadDispatcher::IsGameThread()) {
                captureWork();
            } else {
                const bool queued = GameThreadDispatcher::Enqueue(
                    "actor_inventory", "actor_inventory:" + std::to_string(expectedRefID),
                    RuntimeGeneration::Current(), std::move(captureWork),
                    [capture](const char*) {
                        try {
                            capture->set_value({});
                        } catch (...) {
                        }
                    });
                if (!queued) {
                    try {
                        capture->set_value({});
                    } catch (...) {
                    }
                }
            }

            const auto wait = std::chrono::milliseconds(std::max(50, maxAgeMs));
            if (future.wait_for(wait) == std::future_status::ready) {
                NativeInventoryCapture captured = future.get();
                if (captured.ok) {
                    data.inventory.reserve(captured.items.size());
                    for (const auto& nativeItem : captured.items) {
                        InventoryItem item;
                        item.name = nativeItem.name;
                        item.baseid = formatFormId(nativeItem.baseFormId);
                        item.count = nativeItem.count;
                        item.equipped = nativeItem.equipped;
                        item.type = nativeItem.type;
                        item.condition = nativeItem.condition;
                        data.inventory.push_back(std::move(item));
                    }
                }
            } else {
                Log("AgentManager: Native inventory capture timed out for 0x%08X after %dms",
                    expectedRefID, std::max(50, maxAgeMs));
            }

            ApplyFallbackDefaults(data);
            Log("AgentManager: Collected native actor snapshot for %s (0x%08X), equipment=%zu inventory=%zu",
                data.displayName.c_str(), expectedRefID, data.equipment.size(), data.inventory.size());
            return data;
        }

        if (LoadSnapshotNPCData(expectedRefID, data, requireFreshSnapshot, maxAgeMs)) {
            ApplyFallbackDefaults(data);
            return data;
        }

        ApplyFallbackDefaults(data);
        Log("AgentManager: Collected NPC data (fallback defaults; no usable actor snapshot)");
        return data;
    }

    // Collect NPC data from game memory or the script-runner snapshot.
    NPCData CollectNPCData(uint32_t expectedRefID) {
        return CollectNPCDataWithSnapshotPolicy(expectedRefID, false, 1000);
    }

    static std::string FormatNpcVoiceJson(const NPCData& data) {
        std::ostringstream json;
        json << "{";
        json << "\"type\":\"npc_voice\",";
        json << "\"actor_name\":\"" << HTTPManager::EscapeJson(data.displayName) << "\",";
        json << "\"actor_type\":\"npc\",";
        json << "\"refid\":\"";
        if (data.refID != 0) {
            json << "0x" << std::hex << std::setw(8) << std::setfill('0') << data.refID << std::dec;
        }
        json << "\",";
        json << "\"baseid\":\"" << HTTPManager::EscapeJson(data.baseName) << "\",";
        json << "\"voiceid\":\"" << HTTPManager::EscapeJson(data.voiceId) << "\",";
        json << "\"voice_formid\":\"" << HTTPManager::EscapeJson(data.voiceFormId) << "\",";
        json << "\"voice_name\":\"" << HTTPManager::EscapeJson(data.voiceName) << "\",";
        json << "\"source\":\"fnv_snapshot\"";
        json << "}";
        return json.str();
    }

    static bool IsTemporaryOrSilentVoice(const std::string& voiceId, const std::string& voiceName) {
        const std::string voiceKey = NormalizeName(voiceId + " " + voiceName);
        return voiceKey.find("nodialogue") != std::string::npos ||
            voiceKey.find("donotrecord") != std::string::npos ||
            voiceKey.find("nvdlec01femaleunquenodialogue") != std::string::npos ||
            voiceKey.find("nvdlc01femaleunquenodialogue") != std::string::npos;
    }

    static void SendNpcVoiceUpdate(const NPCData& data) {
        if (data.voiceId.empty() && data.voiceFormId.empty() && data.voiceName.empty()) {
            return;
        }

        if (IsTemporaryOrSilentVoice(data.voiceId, data.voiceName)) {
            Log("AgentManager: Skipping temporary/silent voice update for %s (voiceid=%s, voice_name=%s)",
                data.displayName.c_str(),
                data.voiceId.c_str(),
                data.voiceName.c_str());
            return;
        }

        std::string json = FormatNpcVoiceJson(data);
        TaskManager::Enqueue("gamedata", "npc_voice", RuntimeGeneration::Current(), false,
            std::chrono::seconds(30), [json](const TaskManager::CancellationToken& token) {
            if (!token.WaitFor(std::chrono::milliseconds(900))) return;
            std::string response = HTTPManager::SendJson("gamedata.php", json);
            if (response.empty()) {
                Log("AgentManager: NPC voice update returned empty response");
            } else {
                Log("AgentManager: NPC voice update response: %s",
                    response.length() > 100 ? response.substr(0, 100).c_str() : response.c_str());
            }
        });
    }

    static bool FindVoiceSampleForData(const NPCData& data, VoiceSampleOverridesFNV::VoiceSampleOverride& sample) {
        if (!IsMissingSnapshotValue(data.voiceId) &&
            VoiceSampleOverridesFNV::FindVoiceSampleOverride(data.voiceId, sample)) {
            return true;
        }

        if (!IsMissingSnapshotValue(data.voiceName) &&
            VoiceSampleOverridesFNV::FindVoiceSampleOverride(data.voiceName, sample)) {
            return true;
        }

        return false;
    }

    static void SendVoiceSampleUpload(const NPCData& data) {
        if (data.displayName.empty()) {
            return;
        }

        VoiceSampleOverridesFNV::VoiceSampleOverride sample;
        if (!FindVoiceSampleForData(data, sample)) {
            if (!IsMissingSnapshotValue(data.voiceId) || !IsMissingSnapshotValue(data.voiceName)) {
                Log("AgentManager: No voice sample override found for %s (voiceid=%s, voice_name=%s)",
                    data.displayName.c_str(),
                    data.voiceId.c_str(),
                    data.voiceName.c_str());
            }
            return;
        }

        const std::string originalName = VoiceSampleResolverFNV::BuildOriginalName(sample.voiceFile);
        const std::string uploadKey = NormalizeName(originalName);
        {
            std::lock_guard<std::mutex> lock(g_voiceSampleUploadMutex);
            if (g_attemptedVoiceSampleUploads.find(uploadKey) != g_attemptedVoiceSampleUploads.end()) {
                return;
            }
            g_attemptedVoiceSampleUploads.insert(uploadKey);
        }

        const std::string actorName = data.displayName;
        TaskManager::Enqueue("voice_sample", actorName, RuntimeGeneration::Current(), false,
            std::chrono::seconds(60), [actorName, originalName, uploadKey, sample](const TaskManager::CancellationToken& token) {
            if (!token.WaitFor(std::chrono::milliseconds(1200))) return;

            std::string audioData;
            std::string sourcePath;
            std::string resolveError;
            if (!VoiceSampleResolverFNV::ResolveAndRead(
                    sample.voiceFile, audioData, sourcePath, &resolveError)) {
                Log("AgentManager: Voice sample resolution failed for %s: %s (%s)",
                    actorName.c_str(),
                    sample.voiceFile.c_str(),
                    resolveError.c_str());
                std::lock_guard<std::mutex> lock(g_voiceSampleUploadMutex);
                g_attemptedVoiceSampleUploads.erase(uploadKey);
                return;
            }

            Log("AgentManager: Resolved voice sample for %s from %s (%zu bytes)",
                actorName.c_str(), sourcePath.c_str(), audioData.size());

            const std::string response = HTTPManager::UploadVoiceSample(
                audioData,
                actorName,
                originalName,
                sample.transcript);
            if (response.empty()) {
                Log("AgentManager: Voice sample upload returned empty response for %s", actorName.c_str());
                std::lock_guard<std::mutex> lock(g_voiceSampleUploadMutex);
                g_attemptedVoiceSampleUploads.erase(uploadKey);
            } else {
                Log("AgentManager: Voice sample upload completed for %s from %s",
                    actorName.c_str(),
                    sourcePath.c_str());
            }
        });
    }

    static std::string FormIdHex(uint32_t formId) {
        if (formId == 0) {
            return "";
        }
        std::ostringstream value;
        value << "0x" << std::hex << std::setw(8) << std::setfill('0') << formId << std::dec;
        return value.str();
    }

    static void AppendEquipmentSlotJson(std::ostringstream& json, const std::string& slot, const std::string& value, bool& firstSlot) {
        if (!firstSlot) {
            json << ",";
        }
        firstSlot = false;

        std::string name = value;
        std::string baseid;
        const size_t delimiter = value.find('^');
        if (delimiter != std::string::npos) {
            name = value.substr(0, delimiter);
            baseid = value.substr(delimiter + 1);
        }

        json << "\"" << slot << "\":{";
        json << "\"name\":\"" << HTTPManager::EscapeJson(Trim(name)) << "\",";
        json << "\"baseid\":\"" << HTTPManager::EscapeJson(Trim(baseid)) << "\"";
        json << "}";
    }

    static void AppendEquipmentItemJson(std::ostringstream& json, const EquipmentItem& item, bool& firstSlot) {
        if (!firstSlot) {
            json << ",";
        }
        firstSlot = false;

        json << "\"" << item.slotName << "\":{";
        json << "\"slot\":" << item.slot << ",";
        json << "\"name\":\"" << HTTPManager::EscapeJson(item.name) << "\",";
        json << "\"baseid\":\"" << HTTPManager::EscapeJson(item.baseid) << "\",";
        if (item.condition >= 0.0f) {
            json << "\"condition\":" << item.condition;
        } else {
            json << "\"condition\":null";
        }
        json << "}";
    }

    static void AppendEquipmentObjectJson(std::ostringstream& json, const NPCData& data) {
        json << "\"equipment\":{";
        bool firstSlot = true;
        if (!data.equipment.empty()) {
            for (const auto& item : data.equipment) {
                AppendEquipmentItemJson(json, item, firstSlot);
            }
        } else {
            AppendEquipmentSlotJson(json, "face", data.head, firstSlot);
            AppendEquipmentSlotJson(json, "body", data.upperBody, firstSlot);
            AppendEquipmentSlotJson(json, "left_hand", data.leftHand, firstSlot);
            AppendEquipmentSlotJson(json, "right_hand", data.rightHand, firstSlot);
            AppendEquipmentSlotJson(json, "head", data.hair, firstSlot);
            AppendEquipmentSlotJson(json, "weapon", data.weapon, firstSlot);
            AppendEquipmentSlotJson(json, "upper_body_addon", data.upperBodyAddon, firstSlot);
            AppendEquipmentSlotJson(json, "lower_body_addon", data.lowerBodyAddon, firstSlot);
        }
        json << "}";
    }

    static std::string FormatActorProfileJson(const NPCData& data) {
        std::ostringstream json;
        const bool hasTemporarySilentVoice = IsTemporaryOrSilentVoice(data.voiceId, data.voiceName);
        const std::string voiceId = hasTemporarySilentVoice ? "" : data.voiceId;
        const std::string voiceFormId = hasTemporarySilentVoice ? "" : data.voiceFormId;
        const std::string voiceName = hasTemporarySilentVoice ? "" : data.voiceName;

        json << "{";
        json << "\"schema\":\"dialectic.actor_profile.v1\",";
        json << "\"type\":\"actor_profile\",";
        json << "\"actor_name\":\"" << HTTPManager::EscapeJson(data.displayName) << "\",";
        json << "\"actor_type\":\"npc\",";
        json << "\"source\":\"fnv_snapshot\",";

        json << "\"identity\":{";
        json << "\"refid\":\"" << FormIdHex(data.refID) << "\",";
        json << "\"baseid\":\"" << HTTPManager::EscapeJson(data.baseName) << "\",";
        json << "\"gender\":\"" << HTTPManager::EscapeJson(data.gender) << "\",";
        json << "\"race\":\"" << HTTPManager::EscapeJson(data.race) << "\",";
        json << "\"voiceid\":\"" << HTTPManager::EscapeJson(voiceId) << "\",";
        json << "\"voice_formid\":\"" << HTTPManager::EscapeJson(voiceFormId) << "\",";
        json << "\"voice_name\":\"" << HTTPManager::EscapeJson(voiceName) << "\"";
        json << "},";

        json << "\"special\":{";
        json << "\"strength\":" << data.strength << ",";
        json << "\"perception\":" << data.perception << ",";
        json << "\"endurance\":" << data.endurance << ",";
        json << "\"charisma\":" << data.charisma << ",";
        json << "\"intelligence\":" << data.intelligence << ",";
        json << "\"agility\":" << data.agility << ",";
        json << "\"luck\":" << data.luck;
        json << "},";

        json << "\"skills\":{";
        json << "\"barter\":" << data.barter << ",";
        json << "\"energy_weapons\":" << data.energyWeapons << ",";
        json << "\"explosives\":" << data.explosives << ",";
        json << "\"guns\":" << data.guns << ",";
        json << "\"lockpick\":" << data.lockpick << ",";
        json << "\"medicine\":" << data.medicine << ",";
        json << "\"melee_weapons\":" << data.meleeWeapons << ",";
        json << "\"repair\":" << data.repair << ",";
        json << "\"science\":" << data.science << ",";
        json << "\"sneak\":" << data.sneak << ",";
        json << "\"speech\":" << data.speech << ",";
        json << "\"survival\":" << data.survival << ",";
        json << "\"unarmed\":" << data.unarmed;
        json << "},";

        json << "\"stats\":{";
        json << "\"level\":" << data.level << ",";
        json << "\"health\":" << data.health << ",";
        json << "\"health_max\":" << data.healthMax << ",";
        json << "\"action_points\":" << data.actionPoints << ",";
        json << "\"action_points_max\":" << data.actionPointsMax << ",";
        json << "\"scale\":" << data.scale << ",";
        json << "\"xp\":" << data.xp << ",";
        json << "\"karma\":" << data.karma;
        json << "},";

        AppendEquipmentObjectJson(json, data);
        json << ",";

        json << "\"factions\":[";
        for (size_t i = 0; i < data.factions.size(); ++i) {
            if (i > 0) {
                json << ",";
            }
            json << "{\"formid\":\"" << FormIdHex(data.factions[i].first) << "\",\"rank\":" << data.factions[i].second << "}";
        }
        json << "],";

        json << "\"reputation\":[";
        for (size_t i = 0; i < data.reputation.size(); ++i) {
            if (i > 0) {
                json << ",";
            }
            json << "{\"formid\":\"" << FormIdHex(data.reputation[i].first) << "\",\"value\":" << data.reputation[i].second << "}";
        }
        json << "]";

        json << "}";
        return json.str();
    }

    static void SendActorProfileUpdate(const NPCData& data) {
        std::string json = FormatActorProfileJson(data);
        TaskManager::Enqueue("gamedata", "actor_profile", RuntimeGeneration::Current(), true,
            std::chrono::seconds(30), [json](const TaskManager::CancellationToken& token) {
            if (token.IsCancellationRequested()) return;
            std::string response = HTTPManager::SendJson("gamedata.php", json);
            if (response.empty()) {
                Log("AgentManager: Actor profile update returned empty response");
            } else {
                Log("AgentManager: Actor profile update response: %s",
                    response.length() > 100 ? response.substr(0, 100).c_str() : response.c_str());
            }
        });
    }

    static std::string FormatEquipmentJson(const NPCData& data) {
        std::ostringstream json;
        json << "{";
        json << "\"type\":\"equipment\",";
        json << "\"actor_name\":\"" << HTTPManager::EscapeJson(data.displayName) << "\",";
        json << "\"actor_type\":\"npc\",";
        json << "\"refid\":\"" << FormIdHex(data.refID) << "\",";
        AppendEquipmentObjectJson(json, data);
        json << "}";
        return json.str();
    }

    static bool ShouldSendEquipmentUpdate(const NPCData& data, const std::string& payload) {
        if (data.refID == 0) {
            return true;
        }

        std::lock_guard<std::mutex> lock(g_metadataHashMutex);
        auto existing = g_lastEquipmentHash.find(data.refID);
        if (existing != g_lastEquipmentHash.end() && existing->second == payload) {
            return false;
        }

        g_lastEquipmentHash[data.refID] = payload;
        return true;
    }

    static void SendEquipmentUpdate(const NPCData& data) {
        if (data.equipment.empty()
            && data.head.empty()
            && data.upperBody.empty()
            && data.leftHand.empty()
            && data.rightHand.empty()
            && data.hair.empty()
            && data.weapon.empty()
            && data.upperBodyAddon.empty()
            && data.lowerBodyAddon.empty()) {
            return;
        }

        std::string json = FormatEquipmentJson(data);
        if (!ShouldSendEquipmentUpdate(data, json)) {
            return;
        }

        TaskManager::Enqueue("gamedata", "equipment", RuntimeGeneration::Current(), false,
            std::chrono::seconds(30), [json](const TaskManager::CancellationToken& token) {
            if (token.IsCancellationRequested()) return;
            std::string response = HTTPManager::SendJson("gamedata.php", json);
            if (response.empty()) {
                Log("AgentManager: Equipment update returned empty response");
            } else {
                Log("AgentManager: Equipment update response: %s",
                    response.length() > 100 ? response.substr(0, 100).c_str() : response.c_str());
            }
        });
    }

    static std::string FormatInventoryJson(const NPCData& data) {
        std::ostringstream json;
        json << "{";
        json << "\"type\":\"inventory\",";
        json << "\"actor_name\":\"" << HTTPManager::EscapeJson(data.displayName) << "\",";
        json << "\"actor_type\":\"npc\",";
        json << "\"refid\":\"";
        if (data.refID != 0) {
            json << "0x" << std::hex << std::setw(8) << std::setfill('0') << data.refID << std::dec;
        }
        json << "\",";
        json << "\"items\":[";

        for (size_t i = 0; i < data.inventory.size(); ++i) {
            const InventoryItem& item = data.inventory[i];
            if (i > 0) {
                json << ",";
            }

            json << "{";
            json << "\"name\":\"" << HTTPManager::EscapeJson(item.name) << "\",";
            json << "\"baseid\":\"" << HTTPManager::EscapeJson(item.baseid) << "\",";
            json << "\"count\":" << item.count << ",";
            json << "\"equipped\":" << (item.equipped ? "true" : "false") << ",";
            if (item.condition >= 0.0f) {
                json << "\"condition\":" << item.condition << ",";
            } else {
                json << "\"condition\":null,";
            }
            json << "\"type\":" << item.type << ",";
            if (!item.ammo.empty()) {
                json << "\"ammo\":\"" << HTTPManager::EscapeJson(item.ammo) << "\",";
            } else {
                json << "\"ammo\":null,";
            }
            json << "\"mods\":[";
            for (size_t modIndex = 0; modIndex < item.mods.size(); ++modIndex) {
                if (modIndex > 0) {
                    json << ",";
                }
                json << "\"" << HTTPManager::EscapeJson(Trim(item.mods[modIndex])) << "\"";
            }
            json << "]";
            json << "}";
        }

        json << "]";
        json << "}";
        return json.str();
    }

    static bool ShouldSendInventoryUpdate(const NPCData& data, const std::string& payload) {
        if (data.refID == 0) {
            return true;
        }

        std::lock_guard<std::mutex> lock(g_metadataHashMutex);
        auto existing = g_lastInventoryHash.find(data.refID);
        if (existing != g_lastInventoryHash.end() && existing->second == payload) {
            return false;
        }

        g_lastInventoryHash[data.refID] = payload;
        return true;
    }

    static void SendInventoryUpdate(const NPCData& data) {
        if (data.inventory.empty()) {
            return;
        }

        std::string json = FormatInventoryJson(data);
        if (!ShouldSendInventoryUpdate(data, json)) {
            return;
        }

        TaskManager::Enqueue("gamedata", "inventory", RuntimeGeneration::Current(), false,
            std::chrono::seconds(30), [json](const TaskManager::CancellationToken& token) {
            if (token.IsCancellationRequested()) return;
            std::string response = HTTPManager::SendJson("gamedata.php", json);
            if (response.empty()) {
                Log("AgentManager: Inventory update returned empty response");
            } else {
                Log("AgentManager: Inventory update response: %s",
                    response.length() > 100 ? response.substr(0, 100).c_str() : response.c_str());
            }
        });
    }

    void SendActorProfile(const std::string& npcName, uint32_t refID) {
        if (!g_initialized) {
            Log("AgentManager: Cannot send actor profile, not initialized");
            return;
        }

        if (IsPlayerReference(refID)) {
            Log("AgentManager: Routed player inventory away from NPC profile snapshot bridge");
            PlayerInventoryManagerFNV::ForceRefresh("actor_profile_player_guard", 0);
            return;
        }

        Log("AgentManager: SendActorProfile called for: %s (0x%08X)", npcName.c_str(), refID);

        constexpr int kSnapshotTimeoutMs = 600;
        RuntimeSnapshot::ActorState nativeActor;
        const bool hasNativeSnapshot = RuntimeSnapshot::TryGetActor(refID, nativeActor);
        bool hasFreshSnapshot = hasNativeSnapshot;
        if (!hasNativeSnapshot) {
            RequestActorSnapshot(refID, npcName);
            hasFreshSnapshot = WaitForFreshActorSnapshot(refID, kSnapshotTimeoutMs);
            ClearActorSnapshotRequest();
            if (!hasFreshSnapshot) {
                Log("AgentManager: Proceeding with actor profile fallback data for %s (0x%08X)", npcName.c_str(), refID);
            }
        }

        NPCData data = CollectNPCDataWithSnapshotPolicy(refID, true, kSnapshotTimeoutMs);
        data.displayName = npcName;
        if (data.refID == 0) {
            data.refID = refID;
        }

        SendActorProfileUpdate(data);
        SendNpcVoiceUpdate(data);
        SendVoiceSampleUpload(data);
        SendEquipmentUpdate(data);
        SendInventoryUpdate(data);
        Log("AgentManager: Sent actor profile update for: %s", npcName.c_str());
    }

    void SendActorProfile(const NPCData& data) {
        if (!g_initialized) {
            Log("AgentManager: Cannot send actor profile metadata, not initialized");
            return;
        }

        if (IsPlayerReference(data.refID)) {
            Log("AgentManager: Routed player metadata away from NPC profile snapshot bridge");
            PlayerInventoryManagerFNV::ForceRefresh("actor_metadata_player_guard", 0);
            return;
        }

        if (data.displayName.empty() || data.refID == 0) {
            Log("AgentManager: Cannot send actor profile metadata, missing name or refid");
            return;
        }

        Log("AgentManager: SendActorProfile metadata called for: %s (0x%08X)",
            data.displayName.c_str(), data.refID);

        SendActorProfileUpdate(data);
        SendNpcVoiceUpdate(data);
        SendVoiceSampleUpload(data);
        SendEquipmentUpdate(data);
        SendInventoryUpdate(data);
        Log("AgentManager: Sent actor metadata update for: %s", data.displayName.c_str());
    }

    void RefreshActorMetadata(uint32_t refID, const std::string& npcName) {
        if (!g_initialized || refID == 0) {
            return;
        }

        if (IsPlayerReference(refID)) {
            PlayerInventoryManagerFNV::ForceRefresh("metadata_refresh_player_guard", 0);
            return;
        }

        constexpr int kSnapshotTimeoutMs = 900;
        RuntimeSnapshot::ActorState nativeActor;
        const bool hasNativeSnapshot = RuntimeSnapshot::TryGetActor(refID, nativeActor);
        bool hasFreshSnapshot = hasNativeSnapshot;
        if (!hasNativeSnapshot) {
            RequestActorSnapshot(refID, npcName);
            hasFreshSnapshot = WaitForFreshActorSnapshot(refID, kSnapshotTimeoutMs);
            ClearActorSnapshotRequest();
            if (!hasFreshSnapshot) {
                Log("AgentManager: Metadata refresh using fallback snapshot for %s (0x%08X)",
                    npcName.c_str(), refID);
            }
        }

        NPCData data = CollectNPCDataWithSnapshotPolicy(refID, true, kSnapshotTimeoutMs);
        if (!npcName.empty()) {
            data.displayName = npcName;
        }
        if (data.refID == 0) {
            data.refID = refID;
        }

        SendActorProfileUpdate(data);
        SendNpcVoiceUpdate(data);
        SendVoiceSampleUpload(data);
        SendEquipmentUpdate(data);
        SendInventoryUpdate(data);
    }

    bool RefreshActorMetadataForPrompt(uint32_t refID, const std::string& npcName, int timeoutMs) {
        if (!g_initialized || refID == 0) {
            return false;
        }

        if (IsPlayerReference(refID)) {
            PlayerInventoryManagerFNV::MarkDirty("prompt_player_inventory", 0);
            return true;
        }

        (void)timeoutMs;
        RuntimeSnapshot::ActorState nativeActor;
        if (!RuntimeSnapshot::TryGetActor(refID, nativeActor)) {
            Log("AgentManager: Prompt metadata refresh skipped; no native actor snapshot for %s (0x%08X)",
                npcName.c_str(), refID);
            return false;
        }

        NPCData data = CollectNPCDataWithSnapshotPolicy(refID, true, 0);
        if (!npcName.empty()) {
            data.displayName = npcName;
        }
        if (data.refID == 0) {
            data.refID = refID;
        }

        SendActorProfileUpdate(data);
        SendEquipmentUpdate(data);
        SendInventoryUpdate(data);
        Log("AgentManager: Prompt metadata refresh queued asynchronously for %s (0x%08X) equipment=%zu inventory=%zu",
            data.displayName.c_str(), data.refID, data.equipment.size(), data.inventory.size());
        return true;
    }

    void RefreshRegisteredAgentVoices() {
        if (!g_initialized) {
            return;
        }

        const auto agents = GetRegisteredAgentSnapshot();
        std::size_t queued = 0;
        for (const auto& agent : agents) {
            const uint32_t refID = agent.first;
            if (refID == 0 || IsPlayerReference(refID)) {
                continue;
            }

            RuntimeSnapshot::ActorState nativeActor;
            if (!RuntimeSnapshot::TryGetActor(refID, nativeActor)) {
                continue;
            }

            NPCData data = CollectNPCDataWithSnapshotPolicy(refID, false, 1000);
            if (data.displayName.empty()) {
                data.displayName = agent.second;
            }
            if (data.refID == 0) {
                data.refID = refID;
            }
            if (data.voiceId.empty() && data.voiceFormId.empty() && data.voiceName.empty()) {
                continue;
            }

            SendNpcVoiceUpdate(data);
            ++queued;
        }
        Log("AgentManager: Refreshed voice mappings for %zu/%zu registered agents after voice sync",
            queued, agents.size());
    }
}

