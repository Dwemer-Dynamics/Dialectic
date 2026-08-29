// ActorPositionResolverFNV.cpp - FNV actor/player position resolution for spatial audio

#include "ActorPositionResolverFNV.h"
#include "Logger.h"
#include "NativeComparisonTelemetry.h"
#include "RuntimeSnapshot.h"
#include "SpatialPathProviderFNV.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <chrono>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

struct CachedPosition {
    ActorPositionResolverFNV::PositionResult position;
    std::chrono::steady_clock::time_point timestamp;
};

struct CachedDoorPosition {
    ActorPositionResolverFNV::DoorPosition position;
    std::chrono::steady_clock::time_point timestamp;
};

static std::mutex g_cacheMutex;
static std::unordered_map<uint32_t, CachedPosition> g_positionCache;
static std::unordered_set<uint32_t> g_latestBridgeActorIds;
static std::unordered_map<uint32_t, ActorPositionResolverFNV::PositionResult> g_latestBridgePositions;
static auto g_latestBridgeActorScanTime = std::chrono::steady_clock::time_point{};
static auto g_lastActorComparisonSample = std::chrono::steady_clock::time_point{};
static std::mutex g_doorCacheMutex;
static std::unordered_map<uint32_t, CachedDoorPosition> g_doorPositionCache;
static std::mutex g_cellInteriorCacheMutex;
static std::unordered_map<uint32_t, bool> g_cellInteriorCache;
static constexpr auto kPositionCacheTtl = std::chrono::seconds(30);
static const char* kActorPositionsPath = "Data\\NVSE\\Plugins\\dialectic_actor_positions.tmp";
static const char* kActorPositionsRelativePath = "NVSE\\Plugins\\dialectic_actor_positions.tmp";
static auto g_lastBridgeReadTime = std::chrono::steady_clock::time_point{};
static constexpr auto kBridgeReadInterval = std::chrono::milliseconds(1000);
static FILETIME g_lastBridgeWriteTime = {};
static std::string g_lastBridgePath;
static int g_lastBridgeLoadedCount = -1;
static auto g_lastBridgeLoadLogTime = std::chrono::steady_clock::time_point{};
static std::mutex g_playerStateMutex;
static bool g_playerSneaking = false;
static auto g_playerSneakingTimestamp = std::chrono::steady_clock::time_point{};
static bool g_playerSneakingMismatchLogged = false;

void RememberCellInterior(uint32_t cellFormId, bool isInterior) {
    if (cellFormId == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_cellInteriorCacheMutex);
    g_cellInteriorCache[cellFormId] = isInterior;
}

bool ApplyKnownCellInterior(ActorPositionResolverFNV::PositionResult& result) {
    if (!result.cellResolved || result.cellFormId == 0 || result.interiorKnown) {
        return result.interiorKnown;
    }

    std::lock_guard<std::mutex> lock(g_cellInteriorCacheMutex);
    const auto it = g_cellInteriorCache.find(result.cellFormId);
    if (it == g_cellInteriorCache.end()) {
        return false;
    }

    result.interiorKnown = true;
    result.isInterior = it->second;
    return true;
}

std::string Trim(const std::string& value) {
    const char* whitespace = " \t\r\n";
    const size_t start = value.find_first_not_of(whitespace);
    if (start == std::string::npos) {
        return "";
    }
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

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::vector<std::string> Split(const std::string& value, char delimiter) {
    std::vector<std::string> parts;
    std::string part;
    std::istringstream stream(value);
    while (std::getline(stream, part, delimiter)) {
        parts.push_back(part);
    }
    return parts;
}

std::string DecodeEscapedLineBreaks(std::string value) {
    size_t pos = 0;
    while ((pos = value.find("\\r", pos)) != std::string::npos) {
        value.replace(pos, 2, "\r");
        ++pos;
    }

    pos = 0;
    while ((pos = value.find("\\n", pos)) != std::string::npos) {
        value.replace(pos, 2, "\n");
        ++pos;
    }

    return value;
}

uint32_t ParseHexFormID(const std::string& value) {
    std::string cleaned = Trim(value);
    if (cleaned.rfind("0x", 0) == 0 || cleaned.rfind("0X", 0) == 0) {
        cleaned = cleaned.substr(2);
    }

    try {
        return static_cast<uint32_t>(std::stoul(cleaned, nullptr, 16));
    } catch (...) {
        return 0;
    }
}

float ParseFloat(const std::string& value, float fallback = 0.0f) {
    try {
        return std::stof(Trim(value));
    } catch (...) {
        return fallback;
    }
}

bool ParseBoolFlag(const std::string& value, bool fallback = false) {
    const std::string cleaned = Trim(value);
    if (cleaned == "1" || cleaned == "true" || cleaned == "True" || cleaned == "TRUE") {
        return true;
    }
    if (cleaned == "0" || cleaned == "false" || cleaned == "False" || cleaned == "FALSE") {
        return false;
    }
    return fallback;
}

int ParseInt(const std::string& value, int fallback = 0) {
    try {
        return std::stoi(Trim(value));
    } catch (...) {
        return fallback;
    }
}

SpatialPathProviderFNV::PathStatus ParsePathStatus(const std::string& value) {
    const std::string cleaned = Trim(value);
    if (cleaned == "success" || cleaned == "Success" || cleaned == "SUCCESS" || cleaned == "1") {
        return SpatialPathProviderFNV::PathStatus::Success;
    }
    if (cleaned == "no_path" || cleaned == "NoPath" || cleaned == "NO_PATH" || cleaned == "0") {
        return SpatialPathProviderFNV::PathStatus::NoPath;
    }
    return SpatialPathProviderFNV::PathStatus::Unavailable;
}

bool GetFileModifiedMetadata(const char* path, uint64_t& ageMs, FILETIME& writeTime) {
    WIN32_FILE_ATTRIBUTE_DATA attributes = {};
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &attributes)) {
        return false;
    }
    writeTime = attributes.ftLastWriteTime;

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

std::vector<std::string> BuildActorPositionBridgePaths() {
    std::vector<std::string> paths;
    paths.emplace_back(kActorPositionsPath);
    paths.emplace_back(kActorPositionsRelativePath);

    const char* localAppData = std::getenv("LOCALAPPDATA");
    if (localAppData && localAppData[0]) {
        const std::string mo2Overwrite = std::string(localAppData) + "\\ModOrganizer\\Fallout TTW\\overwrite\\";
        paths.push_back(mo2Overwrite + "NVSE\\Plugins\\dialectic_actor_positions.tmp");
        paths.push_back(mo2Overwrite + "Data\\NVSE\\Plugins\\dialectic_actor_positions.tmp");
    }

    return paths;
}

bool FindActorPositionBridgePath(std::string& path, uint64_t& ageMs, FILETIME& writeTime) {
    bool found = false;
    uint64_t bestAgeMs = std::numeric_limits<uint64_t>::max();
    FILETIME bestWriteTime = {};
    std::string bestPath;

    for (const auto& candidate : BuildActorPositionBridgePaths()) {
        uint64_t candidateAgeMs = 0;
        FILETIME candidateWriteTime = {};
        if (!GetFileModifiedMetadata(candidate.c_str(), candidateAgeMs, candidateWriteTime)) {
            continue;
        }

        if (!found || candidateAgeMs < bestAgeMs) {
            found = true;
            bestAgeMs = candidateAgeMs;
            bestWriteTime = candidateWriteTime;
            bestPath = candidate;
        }
    }

    if (!found) {
        return false;
    }

    path = bestPath;
    ageMs = bestAgeMs;
    writeTime = bestWriteTime;
    return true;
}

bool HasUsefulMetadataString(const std::string& value) {
    const std::string cleaned = Trim(value);
    return !cleaned.empty() &&
        cleaned != "Unknown" &&
        cleaned != "<no name>";
}

void CopyMissingString(std::string& target, const std::string& source) {
    if (!HasUsefulMetadataString(target) && HasUsefulMetadataString(source)) {
        target = source;
    }
}

std::string FormatFormId(uint32_t formId) {
    if (formId == 0) {
        return "";
    }

    char buffer[16] = {};
    snprintf(buffer, sizeof(buffer), "0x%08X", formId);
    return std::string(buffer);
}

void MergeCachedActorMetadata(ActorPositionResolverFNV::PositionResult& target,
                              const ActorPositionResolverFNV::PositionResult& cached) {
    if (!target.resolved || target.formId == 0 || target.formId != cached.formId) {
        return;
    }

    CopyMissingString(target.actorName, cached.actorName);
    CopyMissingString(target.baseId, cached.baseId);
    CopyMissingString(target.gender, cached.gender);
    CopyMissingString(target.race, cached.race);
    CopyMissingString(target.voiceId, cached.voiceId);
    CopyMissingString(target.voiceFormId, cached.voiceFormId);
    CopyMissingString(target.voiceName, cached.voiceName);

    if (target.level <= 0 && cached.level > 0) {
        target.level = cached.level;
    }
    if (!target.baseTypeKnown && cached.baseTypeKnown) {
        target.baseType = cached.baseType;
        target.baseTypeKnown = true;
    }
    if (!target.playerTeammateKnown && cached.playerTeammateKnown) {
        target.playerTeammateKnown = true;
        target.isPlayerTeammate = cached.isPlayerTeammate;
    }
    if (!target.sceneBusyKnown && cached.sceneBusyKnown) {
        target.sceneBusyKnown = true;
        target.sceneBusy = cached.sceneBusy;
    }
    if (!target.disabledKnown && cached.disabledKnown) {
        target.disabledKnown = true;
        target.isDisabled = cached.isDisabled;
    }
    if (!target.deadKnown && cached.deadKnown) {
        target.deadKnown = true;
        target.isDead = cached.isDead;
    }
    if (!target.actorHasLosToPlayerKnown && cached.actorHasLosToPlayerKnown) {
        target.actorHasLosToPlayerKnown = true;
        target.actorHasLosToPlayer = cached.actorHasLosToPlayer;
    }
    if (!target.playerHasLosToActorKnown && cached.playerHasLosToActorKnown) {
        target.playerHasLosToActorKnown = true;
        target.playerHasLosToActor = cached.playerHasLosToActor;
    }
}

void ApplyCachedActorMetadata(ActorPositionResolverFNV::PositionResult& position) {
    if (!position.resolved || position.formId == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_cacheMutex);
    const auto it = g_positionCache.find(position.formId);
    if (it == g_positionCache.end()) {
        return;
    }

    MergeCachedActorMetadata(position, it->second.position);
}

std::string FormatNativeFormId(uint32_t formId) {
    char buffer[16] = {};
    std::snprintf(buffer, sizeof(buffer), "0x%08X", formId);
    return buffer;
}

ActorPositionResolverFNV::PositionResult PositionFromNativeActor(
    const RuntimeSnapshot::ActorState& actor, const char* source) {
    ActorPositionResolverFNV::PositionResult result;
    result.resolved = actor.formId != 0 && actor.loaded3D && !actor.deleted;
    result.formId = actor.formId;
    result.actorName = actor.name;
    result.baseId = actor.baseFormId == 0 ? "" : FormatNativeFormId(actor.baseFormId);
    result.baseType = actor.baseType;
    result.baseTypeKnown = actor.baseType != 0;
    result.gender = actor.baseType == 0x2A ? (actor.female ? "Female" : "Male") : "";
    result.race = actor.raceName;
    result.voiceId = actor.voiceName;
    result.voiceFormId = actor.voiceFormId == 0 ? "" : FormatNativeFormId(actor.voiceFormId);
    result.voiceName = actor.voiceName;
    result.level = actor.level;
    result.playerTeammateKnown = true;
    result.isPlayerTeammate = actor.playerTeammate;
    result.disabledKnown = true;
    result.isDisabled = actor.deleted;
    result.deadKnown = true;
    result.isDead = actor.dead;
    result.position = {actor.x, actor.y, actor.z};
    result.yaw = actor.yaw;
    result.yawResolved = true;
    result.cellFormId = actor.cellFormId;
    result.cellResolved = actor.cellFormId != 0;
    result.worldspaceFormId = actor.worldspaceFormId;
    result.worldspaceResolved = actor.worldspaceFormId != 0;
    result.interiorKnown = true;
    result.isInterior = actor.interior;
    result.source = source ? source : "native_actor_registry";
    result.reason = result.resolved ? "resolved_native" :
        (actor.deleted ? "native_deleted" : "native_3d_unloaded");
    ApplyCachedActorMetadata(result);
    return result;
}

void CachePosition(const ActorPositionResolverFNV::PositionResult& position) {
    if (!position.resolved || position.formId == 0) {
        return;
    }

    ActorPositionResolverFNV::PositionResult merged = position;
    std::lock_guard<std::mutex> lock(g_cacheMutex);
    const auto it = g_positionCache.find(position.formId);
    if (it != g_positionCache.end()) {
        MergeCachedActorMetadata(merged, it->second.position);
    }
    g_positionCache[position.formId] = { merged, std::chrono::steady_clock::now() };
}

void CacheDoorPosition(const ActorPositionResolverFNV::DoorPosition& position) {
    if (!position.resolved || position.formId == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_doorCacheMutex);
    g_doorPositionCache[position.formId] = { position, std::chrono::steady_clock::now() };
}

void RefreshPositionCacheFromBridge() {
    const auto now = std::chrono::steady_clock::now();
    if (g_lastBridgeReadTime.time_since_epoch().count() != 0 &&
        now - g_lastBridgeReadTime < kBridgeReadInterval) {
        return;
    }
    g_lastBridgeReadTime = now;

    uint64_t ageMs = 0;
    FILETIME writeTime = {};
    std::string bridgePath;
    if (!FindActorPositionBridgePath(bridgePath, ageMs, writeTime)) {
        static int missingLogCounter = 0;
        if (++missingLogCounter % 40 == 1) {
            Logger::LogDebug("ActorPositionResolverFNV: Spatial bridge file missing; checked default and MO2 overwrite paths");
        }
        return;
    }

    if (ageMs > 3000) {
        static int staleLogCounter = 0;
        if (++staleLogCounter % 40 == 1) {
            Logger::LogDebug("ActorPositionResolverFNV: Spatial bridge file stale (%llu ms): %s",
                static_cast<unsigned long long>(ageMs), bridgePath.c_str());
        }
        return;
    }

    if (bridgePath == g_lastBridgePath &&
        CompareFileTime(&writeTime, &g_lastBridgeWriteTime) == 0) {
        return;
    }

    std::ifstream input(bridgePath, std::ios::binary);
    if (!input.is_open()) {
        return;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    std::istringstream lines(DecodeEscapedLineBreaks(buffer.str()));

    int loaded = 0;
    std::unordered_set<uint32_t> latestBridgeActorIds;
    std::unordered_map<uint32_t, ActorPositionResolverFNV::PositionResult> latestBridgePositions;
    std::unordered_map<uint32_t, bool> disabledFlags;
    std::unordered_map<uint32_t, bool> deadFlags;
    std::string line;
    while (std::getline(lines, line)) {
        line = Trim(line);
        if (line.rfind("player_sneaking=", 0) == 0) {
            std::lock_guard<std::mutex> playerStateLock(g_playerStateMutex);
            g_playerSneaking = Trim(line.substr(16)) == "1";
            g_playerSneakingTimestamp = std::chrono::steady_clock::now();
            continue;
        }

        if (line.rfind("position=", 0) != 0) {
            if (line.rfind("disabled=", 0) == 0) {
                std::vector<std::string> disabledParts = Split(line.substr(9), '^');
                if (disabledParts.size() >= 2) {
                    const uint32_t formId = ParseHexFormID(disabledParts[0]);
                    if (formId != 0) {
                        disabledFlags[formId] = ParseBoolFlag(disabledParts[1]);
                    }
                }
                continue;
            }

            if (line.rfind("dead=", 0) == 0) {
                std::vector<std::string> deadParts = Split(line.substr(5), '^');
                if (deadParts.size() >= 2) {
                    const uint32_t formId = ParseHexFormID(deadParts[0]);
                    if (formId != 0) {
                        deadFlags[formId] = ParseBoolFlag(deadParts[1]);
                    }
                }
                continue;
            }

            if (line.rfind("path=", 0) == 0) {
                std::vector<std::string> pathParts = Split(line.substr(5), '^');
                if (pathParts.size() >= 5) {
                    const uint32_t sourceFormId = ParseHexFormID(pathParts[0]);
                    const uint32_t listenerFormId = ParseHexFormID(pathParts[1]);
                    const auto status = ParsePathStatus(pathParts[2]);
                    const float airDistance = ParseFloat(pathParts[3], 0.0f);
                    const float pathDistance = ParseFloat(pathParts[4], -1.0f);
                    SpatialPathProviderFNV::RememberScriptPathResult(
                        sourceFormId,
                        listenerFormId,
                        status,
                        airDistance,
                        pathDistance);
                }
                continue;
            }

            if (line.rfind("door=", 0) != 0) {
                continue;
            }

            std::vector<std::string> doorParts = Split(line.substr(5), '^');
            if (doorParts.size() < 5) {
                continue;
            }

            ActorPositionResolverFNV::DoorPosition door;
            door.formId = ParseHexFormID(doorParts[0]);
            door.position.x = ParseFloat(doorParts[1]);
            door.position.y = ParseFloat(doorParts[2]);
            door.position.z = ParseFloat(doorParts[3]);
            door.openState = ParseInt(doorParts[4]);
            if (doorParts.size() >= 6) {
                door.cellFormId = ParseHexFormID(doorParts[5]);
                door.cellResolved = door.cellFormId != 0;
            }
            door.resolved = door.formId != 0;

            if (door.resolved) {
                CacheDoorPosition(door);
            }
            continue;
        }

        std::vector<std::string> parts = Split(line.substr(9), '^');
        if (parts.size() < 6) {
            continue;
        }

        ActorPositionResolverFNV::PositionResult position;
        position.source = "script_bridge";
        position.reason = "script_bridge";
        position.formId = ParseHexFormID(parts[0]);
        position.actorName = Trim(parts[1]);
        position.position.x = ParseFloat(parts[2]);
        position.position.y = ParseFloat(parts[3]);
        position.position.z = ParseFloat(parts[4]);
        position.yaw = ParseFloat(parts[5]);
        position.yawResolved = true;
        if (parts.size() >= 7) {
            position.cellFormId = ParseHexFormID(parts[6]);
            position.cellResolved = position.cellFormId != 0;
        }
        if (parts.size() >= 8) {
            position.voiceId = Trim(parts[7]);
        }
        if (parts.size() >= 9) {
            position.voiceFormId = Trim(parts[8]);
        }
        if (parts.size() >= 10) {
            position.voiceName = Trim(parts[9]);
        }
        if (parts.size() >= 11) {
            position.baseId = Trim(parts[10]);
        }
        if (parts.size() >= 12) {
            position.gender = Trim(parts[11]);
        }
        if (parts.size() >= 13) {
            position.race = Trim(parts[12]);
        }
        if (parts.size() >= 14) {
            position.interiorKnown = true;
            position.isInterior = ParseBoolFlag(parts[13]);
        }
        if (parts.size() >= 15) {
            position.actorHasLosToPlayerKnown = true;
            position.actorHasLosToPlayer = ParseBoolFlag(parts[14]);
        }
        if (parts.size() >= 16) {
            position.playerHasLosToActorKnown = true;
            position.playerHasLosToActor = ParseBoolFlag(parts[15]);
        }
        if (parts.size() >= 17) {
            position.baseType = ParseInt(parts[16]);
            position.baseTypeKnown = position.baseType > 0;
        }
        if (parts.size() >= 18) {
            position.level = ParseInt(parts[17]);
        }
        if (parts.size() >= 19) {
            position.playerTeammateKnown = true;
            position.isPlayerTeammate = ParseBoolFlag(parts[18]);
        }
        if (parts.size() >= 20) {
            position.sceneBusyKnown = true;
            position.sceneBusy = ParseBoolFlag(parts[19]);
        }
        if (parts.size() >= 21) {
            position.disabledKnown = true;
            position.isDisabled = ParseBoolFlag(parts[20]);
        } else {
            const auto disabledIt = disabledFlags.find(position.formId);
            if (disabledIt != disabledFlags.end()) {
                position.disabledKnown = true;
                position.isDisabled = disabledIt->second;
            }
        }
        const auto deadIt = deadFlags.find(position.formId);
        if (deadIt != deadFlags.end()) {
            position.deadKnown = true;
            position.isDead = deadIt->second;
        }
        ApplyKnownCellInterior(position);
        position.resolved = position.formId != 0;

        if (position.resolved) {
            CachePosition(position);
            latestBridgeActorIds.insert(position.formId);
            latestBridgePositions[position.formId] = position;
            ++loaded;
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_cacheMutex);
        g_latestBridgeActorIds = std::move(latestBridgeActorIds);
        g_latestBridgePositions = std::move(latestBridgePositions);
        g_latestBridgeActorScanTime = now;
    }

    g_lastBridgeWriteTime = writeTime;
    g_lastBridgePath = bridgePath;
    if (loaded > 0) {
        const bool countChanged = loaded != g_lastBridgeLoadedCount;
        const bool heartbeatDue = g_lastBridgeLoadLogTime.time_since_epoch().count() == 0 ||
            now - g_lastBridgeLoadLogTime >= std::chrono::seconds(15);
        if (countChanged || heartbeatDue) {
            Logger::LogInfo("ActorPositionResolverFNV: Loaded %d positions from script bridge path=%s",
                loaded,
                bridgePath.c_str());
            g_lastBridgeLoadLogTime = now;
            g_lastBridgeLoadedCount = loaded;
        }
    }
}

ActorPositionResolverFNV::PositionResult ResolveCached(uint32_t formId) {
    std::lock_guard<std::mutex> lock(g_cacheMutex);
    auto it = g_positionCache.find(formId);
    if (it == g_positionCache.end()) {
        ActorPositionResolverFNV::PositionResult result;
        result.source = "position_cache";
        result.formId = formId;
        result.reason = "cache_miss";
        return result;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now - it->second.timestamp > kPositionCacheTtl) {
        g_positionCache.erase(it);
        ActorPositionResolverFNV::PositionResult result;
        result.source = "position_cache";
        result.formId = formId;
        result.reason = "cache_expired";
        return result;
    }

    ActorPositionResolverFNV::PositionResult result = it->second.position;
    if (result.source.empty()) {
        result.source = "position_cache";
    }
    result.reason = "cached_position";
    return result;
}

} // namespace

namespace ActorPositionResolverFNV {

PositionResult ResolvePlayer() {
    RuntimeSnapshot::GameState native;
    if (RuntimeSnapshot::TryGetFreshGameState(native, std::chrono::milliseconds(500)) &&
        native.inGame && native.playerFormId != 0) {
        PositionResult result;
        result.resolved = native.player3DLoaded;
        result.formId = native.playerFormId;
        result.position = {native.playerX, native.playerY, native.playerZ};
        result.yaw = native.playerYaw;
        result.yawResolved = true;
        result.cellFormId = native.cellFormId;
        result.cellResolved = native.cellFormId != 0;
        result.worldspaceFormId = native.worldspaceFormId;
        result.worldspaceResolved = native.worldspaceFormId != 0;
        result.interiorKnown = native.cellFormId != 0;
        result.isInterior = native.worldspaceFormId == 0;
        result.source = "native_player_snapshot";
        result.reason = result.resolved ? "resolved_native" : "native_player_3d_unloaded";
        return result;
    }

    PositionResult result;
    result.source = "native_player_snapshot";
    result.reason = "native_player_snapshot_unavailable";
    return result;
}

PositionResult ResolveCurrentCrosshairActor() {
    RuntimeSnapshot::GameState native;
    if (RuntimeSnapshot::TryGetFreshGameState(native, std::chrono::milliseconds(500)) &&
        native.crosshairFormId != 0) {
        RuntimeSnapshot::ActorState actor;
        if (RuntimeSnapshot::TryGetActor(native.crosshairFormId, actor)) {
            PositionResult result = PositionFromNativeActor(actor, "native_crosshair");
            RememberActorPosition(result);
            return result;
        }
    }

    PositionResult result;
    result.source = "native_crosshair";
    result.reason = native.crosshairFormId == 0
        ? "crosshair_not_found" : "crosshair_actor_not_in_native_registry";
    return result;
}

PositionResult ResolveActor(uint32_t formId) {
    if (formId == 0) {
        PositionResult result;
        result.source = "actor_lookup";
        result.reason = "zero_formid";
        return result;
    }

    RuntimeSnapshot::GameState nativeState;
    const bool nativeFresh = RuntimeSnapshot::TryGetFreshGameState(
        nativeState, std::chrono::milliseconds(500));
    RuntimeSnapshot::ActorState nativeActor;
    if (RuntimeSnapshot::TryGetActor(formId, nativeActor)) {
        PositionResult native = PositionFromNativeActor(nativeActor, "native_actor_registry");
        if (native.resolved) {
            RememberActorPosition(native);
            return native;
        }
    }

    if (nativeFresh) {
        RefreshPositionCacheFromBridge();
        {
            std::lock_guard<std::mutex> lock(g_cacheMutex);
            const auto now = std::chrono::steady_clock::now();
            const auto bridgeActor = g_latestBridgePositions.find(formId);
            if (g_latestBridgeActorScanTime.time_since_epoch().count() != 0 &&
                now - g_latestBridgeActorScanTime <= kPositionCacheTtl &&
                bridgeActor != g_latestBridgePositions.end()) {
                return bridgeActor->second;
            }
        }

        PositionResult result;
        result.source = "native_actor_registry";
        result.formId = formId;
        result.reason = "actor_not_in_current_native_scene";
        return result;
    }

    PositionResult crosshair = ResolveCurrentCrosshairActor();
    if (crosshair.resolved && crosshair.formId == formId) {
        crosshair.source = "crosshair_match";
        RememberActorPosition(crosshair);
        return crosshair;
    }

    RefreshPositionCacheFromBridge();

    PositionResult cached = ResolveCached(formId);
    if (cached.resolved) {
        return cached;
    }

    PositionResult result;
    result.source = "actor_lookup";
    result.formId = formId;
    result.reason = crosshair.resolved ? "unsupported_ref_lookup" : cached.reason;
    Logger::LogDebug("ActorPositionResolverFNV: Could not resolve actor 0x%08X (%s)",
        formId, result.reason.c_str());
    return result;
}

bool IsPositionInPlayerScene(const PositionResult& position) {
    if (!position.resolved || position.formId == 0) {
        return false;
    }

    const PositionResult player = ResolvePlayer();
    if (!player.resolved) {
        return false;
    }

    const bool playerInterior = player.interiorKnown && player.isInterior;
    const bool actorInterior = position.interiorKnown && position.isInterior;

    if (player.cellResolved && position.cellResolved) {
        if (player.cellFormId == position.cellFormId) {
            return true;
        }

        // Interior transitions are hard scene boundaries. Without this, an
        // actor cached seconds ago in another cell can still pass spatial checks.
        if (playerInterior || actorInterior) {
            return false;
        }

        if (player.worldspaceResolved && position.worldspaceResolved &&
            player.worldspaceFormId != position.worldspaceFormId) {
            return false;
        }

        return true;
    }

    if (playerInterior || actorInterior) {
        return false;
    }

    return true;
}

bool IsActorInPlayerScene(uint32_t formId) {
    const PositionResult position = ResolveActor(formId);
    return IsPositionInPlayerScene(position);
}

bool IsActorPositionFresh(uint32_t formId, int maxAgeMs) {
    if (formId == 0 || maxAgeMs < 0) {
        return false;
    }

    RuntimeSnapshot::GameState native;
    RuntimeSnapshot::ActorState actor;
    if (RuntimeSnapshot::TryGetFreshGameState(native, std::chrono::milliseconds(maxAgeMs))) {
        return RuntimeSnapshot::TryGetActor(formId, actor) &&
            RuntimeSnapshot::IsActorInScene(actor, native);
    }

    RefreshPositionCacheFromBridge();

    std::lock_guard<std::mutex> lock(g_cacheMutex);
    auto it = g_positionCache.find(formId);
    if (it == g_positionCache.end()) {
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    return now - it->second.timestamp <= std::chrono::milliseconds(maxAgeMs);
}

bool IsActorInLatestScan(uint32_t formId, int maxAgeMs) {
    if (formId == 0 || maxAgeMs < 0) {
        return false;
    }

    RuntimeSnapshot::GameState native;
    RuntimeSnapshot::ActorState actor;
    if (RuntimeSnapshot::TryGetFreshGameState(native, std::chrono::milliseconds(maxAgeMs))) {
        return RuntimeSnapshot::TryGetActor(formId, actor) &&
            RuntimeSnapshot::IsActorInScene(actor, native);
    }

    RefreshPositionCacheFromBridge();

    std::lock_guard<std::mutex> lock(g_cacheMutex);
    if (g_latestBridgeActorScanTime.time_since_epoch().count() == 0) {
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now - g_latestBridgeActorScanTime > std::chrono::milliseconds(maxAgeMs)) {
        return false;
    }

    return g_latestBridgeActorIds.find(formId) != g_latestBridgeActorIds.end();
}

std::vector<PositionResult> GetRecentActorPositions() {
    RuntimeSnapshot::GameState nativeState;
    const bool nativeFresh = RuntimeSnapshot::TryGetFreshGameState(
        nativeState, std::chrono::milliseconds(500));
    RefreshPositionCacheFromBridge();
    const std::vector<RuntimeSnapshot::ActorState> nativeActors = RuntimeSnapshot::GetActors();
    if (nativeFresh) {
        std::vector<PositionResult> nativePositions;
        nativePositions.reserve(nativeActors.size());
        for (const RuntimeSnapshot::ActorState& actor : nativeActors) {
            if (!RuntimeSnapshot::IsActorInScene(actor, nativeState)) {
                continue;
            }
            PositionResult result = PositionFromNativeActor(actor, "native_actor_registry");
            if (result.resolved) {
                CachePosition(result);
                nativePositions.push_back(std::move(result));
            }
        }
        const auto comparisonNow = std::chrono::steady_clock::now();
        if (g_lastActorComparisonSample.time_since_epoch().count() == 0 ||
            comparisonNow - g_lastActorComparisonSample >= std::chrono::seconds(10)) {
            g_lastActorComparisonSample = comparisonNow;
            std::unordered_map<uint32_t, PositionResult> bridgePositions;
            bool bridgeFresh = false;
            {
                std::lock_guard<std::mutex> lock(g_cacheMutex);
                bridgeFresh = g_latestBridgeActorScanTime.time_since_epoch().count() != 0 &&
                    comparisonNow - g_latestBridgeActorScanTime <= std::chrono::seconds(3);
                if (bridgeFresh) bridgePositions = g_latestBridgePositions;
            }
            if (bridgeFresh) {
            std::unordered_map<uint32_t, const PositionResult*> nativeByFormId;
            for (const PositionResult& position : nativePositions) {
                nativeByFormId[position.formId] = &position;
            }
            std::size_t bridgeOnly = 0;
            std::size_t drifted = 0;
            for (const auto& entry : bridgePositions) {
                const auto native = nativeByFormId.find(entry.first);
                if (native == nativeByFormId.end()) {
                    ++bridgeOnly;
                    continue;
                }
                const float dx = native->second->position.x - entry.second.position.x;
                const float dy = native->second->position.y - entry.second.position.y;
                const float dz = native->second->position.z - entry.second.position.z;
                if (std::sqrt(dx * dx + dy * dy + dz * dz) > 10.0f) ++drifted;
            }
            std::ostringstream detail;
            detail << "native=" << nativePositions.size()
                   << " bridge=" << bridgePositions.size()
                   << " bridge_only=" << bridgeOnly
                   << " drifted=" << drifted;
            NativeComparisonTelemetry::Record("actor_registry",
                bridgeOnly == 0 && drifted == 0
                    ? NativeComparisonTelemetry::Result::Match
                    : NativeComparisonTelemetry::Result::Mismatch,
                detail.str());
            } else {
                NativeComparisonTelemetry::Record(
                    "actor_registry", NativeComparisonTelemetry::Result::Unavailable,
                    "bridge snapshot unavailable");
            }
        }

        std::unordered_set<uint32_t> nativeFormIds;
        nativeFormIds.reserve(nativePositions.size());
        for (const PositionResult& position : nativePositions) {
            nativeFormIds.insert(position.formId);
        }

        std::unordered_map<uint32_t, PositionResult> freshBridgePositions;
        {
            std::lock_guard<std::mutex> lock(g_cacheMutex);
            if (g_latestBridgeActorScanTime.time_since_epoch().count() != 0 &&
                comparisonNow - g_latestBridgeActorScanTime <= kPositionCacheTtl) {
                freshBridgePositions = g_latestBridgePositions;
            }
        }
        for (const auto& entry : freshBridgePositions) {
            PositionResult fallback = entry.second;
            if (nativeFormIds.find(fallback.formId) != nativeFormIds.end() ||
                !fallback.resolved ||
                !IsPositionInPlayerScene(fallback)) {
                continue;
            }
            CachePosition(fallback);
            nativePositions.push_back(std::move(fallback));
        }
        return nativePositions;
    }

    std::vector<PositionResult> positions;
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(g_cacheMutex);
    for (auto it = g_positionCache.begin(); it != g_positionCache.end();) {
        if (now - it->second.timestamp > kPositionCacheTtl) {
            it = g_positionCache.erase(it);
            continue;
        }

        positions.push_back(it->second.position);
        ++it;
    }

    return positions;
}

std::vector<DoorPosition> GetRecentDoorPositions() {
    RuntimeSnapshot::GameState nativeState;
    const bool nativeFresh = RuntimeSnapshot::TryGetFreshGameState(
        nativeState, std::chrono::milliseconds(500));
    if (!nativeFresh) {
        RefreshPositionCacheFromBridge();
    }

    constexpr std::uint8_t kDoorBaseType = 28;
    std::unordered_map<std::uint32_t, DoorPosition> merged;
    for (const RuntimeSnapshot::ReferenceState& reference : RuntimeSnapshot::GetReferences()) {
        if (reference.baseType != kDoorBaseType || reference.formId == 0 ||
            reference.deleted || reference.taken) {
            continue;
        }
        DoorPosition door;
        door.resolved = true;
        door.formId = reference.formId;
        door.position = {reference.x, reference.y, reference.z};
        door.cellFormId = reference.cellFormId;
        door.cellResolved = reference.cellFormId != 0;
        door.openState = reference.openStateKnown ? reference.openState : 0;
        merged[door.formId] = door;
    }

    if (nativeFresh) {
        std::vector<DoorPosition> positions;
        positions.reserve(merged.size());
        for (auto& entry : merged) {
            positions.push_back(std::move(entry.second));
        }
        return positions;
    }

    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(g_doorCacheMutex);
        for (auto it = g_doorPositionCache.begin(); it != g_doorPositionCache.end();) {
            if (now - it->second.timestamp > kPositionCacheTtl) {
                it = g_doorPositionCache.erase(it);
                continue;
            }

            auto native = merged.find(it->first);
            if (native == merged.end() || native->second.openState == 0) {
                merged[it->first] = it->second.position;
            }
            ++it;
        }
    }

    std::vector<DoorPosition> positions;
    positions.reserve(merged.size());
    for (auto& entry : merged) {
        positions.push_back(std::move(entry.second));
    }
    return positions;
}

void RememberActorPosition(const PositionResult& position) {
    CachePosition(position);
}

void RememberPlayerSneaking(bool sneaking) {
    std::lock_guard<std::mutex> playerStateLock(g_playerStateMutex);
    g_playerSneaking = sneaking;
    g_playerSneakingTimestamp = std::chrono::steady_clock::now();
}

bool IsPlayerSneaking() {
    // PlayerRef.IsSneaking is the authoritative FNV script result. Prefer its
    // fresh bridge value because PlayerMover movement bit 9 can remain set
    // while the player is visibly standing in some TTW animation states.
    RefreshPositionCacheFromBridge();

    bool bridgeAvailable = false;
    bool bridgeSneaking = false;
    {
        std::lock_guard<std::mutex> playerStateLock(g_playerStateMutex);
        const auto now = std::chrono::steady_clock::now();
        bridgeAvailable = g_playerSneakingTimestamp.time_since_epoch().count() != 0 &&
            now - g_playerSneakingTimestamp <= std::chrono::seconds(3);
        bridgeSneaking = g_playerSneaking;
    }

    RuntimeSnapshot::GameState native;
    const bool nativeAvailable = RuntimeSnapshot::TryGetFreshGameState(native, std::chrono::milliseconds(500));

    if (bridgeAvailable) {
        bool shouldLogMismatch = false;
        {
            std::lock_guard<std::mutex> playerStateLock(g_playerStateMutex);
            const bool mismatch = nativeAvailable && native.playerSneaking != bridgeSneaking;
            shouldLogMismatch = mismatch && !g_playerSneakingMismatchLogged;
            g_playerSneakingMismatchLogged = mismatch;
        }
        if (shouldLogMismatch) {
            Logger::LogWarning(
                "ActorPositionResolverFNV: Native player sneaking state disagrees with PlayerRef.IsSneaking; using bridge native=%d bridge=%d",
                native.playerSneaking ? 1 : 0,
                bridgeSneaking ? 1 : 0);
        }
        return bridgeSneaking;
    }

    {
        std::lock_guard<std::mutex> playerStateLock(g_playerStateMutex);
        g_playerSneakingMismatchLogged = false;
    }
    return nativeAvailable ? native.playerSneaking : false;
}

void InvalidateCache() {
    {
        std::lock_guard<std::mutex> lock(g_cacheMutex);
        g_positionCache.clear();
        g_latestBridgeActorIds.clear();
        g_latestBridgePositions.clear();
        g_latestBridgeActorScanTime = {};
        g_lastActorComparisonSample = {};
    }
    {
        std::lock_guard<std::mutex> lock(g_doorCacheMutex);
        g_doorPositionCache.clear();
    }
    g_lastBridgeReadTime = {};
    g_lastBridgeWriteTime = {};
    g_lastBridgePath.clear();
    g_lastBridgeLoadedCount = -1;
    {
        std::lock_guard<std::mutex> playerStateLock(g_playerStateMutex);
        g_playerSneaking = false;
        g_playerSneakingTimestamp = {};
        g_playerSneakingMismatchLogged = false;
    }
    Logger::LogInfo("ActorPositionResolverFNV: invalidated actor, door, and bridge comparison caches");
}

} // namespace ActorPositionResolverFNV
