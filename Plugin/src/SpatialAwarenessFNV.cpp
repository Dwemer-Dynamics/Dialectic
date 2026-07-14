// SpatialAwarenessFNV.cpp - FNV spatial audibility for Dialectic

#include "SpatialAwarenessFNV.h"

#include "Config.h"
#include "Logger.h"
#include "SpatialPathProviderFNV.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace SpatialAwarenessFNV {
namespace {

struct EvalCacheEntry {
    Result result;
    std::chrono::steady_clock::time_point expiresAt;
};

struct SpatialEvalLogSnapshot {
    std::string tier;
    std::string reason;
    int canCommunicate = 0;
    int volumeMilli = 0;
    int airDistanceTenths = 0;
    int maxDistanceTenths = 0;
    int pathDistanceTenths = 0;
    int pathRatioHundredths = 0;
    int navmeshPathUsed = 0;
    int navmeshPathFound = 0;
    int losFallbackUsed = 0;
    int hasLineOfSight = 0;
    int detectionLevel = 0;
    int openDoorCount = 0;
    int closedDoorCount = 0;

    bool operator==(const SpatialEvalLogSnapshot& other) const = default;
};

struct SpatialEvalLogState {
    SpatialEvalLogSnapshot snapshot;
    std::chrono::steady_clock::time_point lastEmit;
    bool initialized = false;
};

static std::mutex g_evalCacheMutex;
static std::unordered_map<std::uint64_t, EvalCacheEntry> g_evalCache;
static constexpr auto kEvalCacheTtl = std::chrono::milliseconds(750);
static constexpr std::size_t kEvalCacheMaxEntries = 4096;

static std::mutex g_spatialEvalLogMutex;
static std::unordered_map<std::uint64_t, SpatialEvalLogState> g_spatialEvalLogState;
static constexpr auto kSpatialEvalLogHeartbeat = std::chrono::seconds(15);
static constexpr auto kSpatialRoutineRejectHeartbeat = std::chrono::seconds(60);
static constexpr std::uint32_t kPlayerFormId = 0x00000014;

std::uint64_t EvalKey(std::uint32_t sourceFormId, std::uint32_t listenerFormId) {
    return (static_cast<std::uint64_t>(sourceFormId) << 32U) |
        static_cast<std::uint64_t>(listenerFormId);
}

float Distance(
    const ActorPositionResolverFNV::Vector3& a,
    const ActorPositionResolverFNV::Vector3& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
}

int QuantizeForLog(float value, float scale, int fallback) {
    if (!std::isfinite(value)) {
        return fallback;
    }

    return static_cast<int>(std::lround(value * scale));
}

SpatialEvalLogSnapshot BuildSpatialEvalLogSnapshot(const char* tier, const Result& result) {
    SpatialEvalLogSnapshot snapshot;
    snapshot.tier = tier ? tier : "";
    snapshot.reason = result.reason;
    snapshot.canCommunicate = result.canCommunicate ? 1 : 0;
    snapshot.volumeMilli = QuantizeForLog(result.volume, 1000.0f, -1);
    snapshot.airDistanceTenths = QuantizeForLog(result.airDistance, 10.0f, -1);
    snapshot.maxDistanceTenths = QuantizeForLog(result.maxDistance, 10.0f, -1);
    snapshot.pathDistanceTenths = QuantizeForLog(result.pathDistance, 10.0f, -1);
    snapshot.pathRatioHundredths = QuantizeForLog(result.pathRatio, 100.0f, -100);
    snapshot.navmeshPathUsed = result.navmeshPathUsed ? 1 : 0;
    snapshot.navmeshPathFound = result.navmeshPathFound ? 1 : 0;
    snapshot.losFallbackUsed = result.losFallbackUsed ? 1 : 0;
    snapshot.hasLineOfSight = result.hasLineOfSight ? 1 : 0;
    snapshot.detectionLevel = result.detectionLevel;
    snapshot.openDoorCount = result.openDoorCount;
    snapshot.closedDoorCount = result.closedDoorCount;
    return snapshot;
}

bool ShouldEmitSpatialEvalLog(
    const ActorPositionResolverFNV::PositionResult& source,
    const ActorPositionResolverFNV::PositionResult& listener,
    const char* tier,
    const Result& result) {
    const auto snapshot = BuildSpatialEvalLogSnapshot(tier, result);
    const auto now = std::chrono::steady_clock::now();
    const auto key = EvalKey(source.formId, listener.formId);
    const bool routineReject = !result.canCommunicate &&
        (result.reason == "too_far" ||
         result.reason == "path_unavailable" ||
         result.reason == "missing_cell" ||
         result.reason == "different_interior_cells" ||
         result.reason == "interior_exterior_boundary");

    std::lock_guard<std::mutex> lock(g_spatialEvalLogMutex);
    auto& state = g_spatialEvalLogState[key];
    const auto heartbeatInterval = routineReject
        ? kSpatialRoutineRejectHeartbeat
        : kSpatialEvalLogHeartbeat;
    const bool heartbeatDue = state.initialized &&
        (now - state.lastEmit) >= heartbeatInterval;
    const bool changed = !state.initialized || !(state.snapshot == snapshot);
    if (routineReject && !heartbeatDue) {
        state.snapshot = snapshot;
        if (!state.initialized) {
            state.lastEmit = now;
            state.initialized = true;
        }
        return false;
    }

    if (!changed && !heartbeatDue) {
        return false;
    }

    state.snapshot = snapshot;
    state.lastEmit = now;
    state.initialized = true;
    return true;
}

bool TryGetCachedResult(std::uint32_t sourceFormId, std::uint32_t listenerFormId, Result& result) {
    if (sourceFormId == 0 || listenerFormId == 0) {
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(g_evalCacheMutex);
    const auto it = g_evalCache.find(EvalKey(sourceFormId, listenerFormId));
    if (it == g_evalCache.end() || it->second.expiresAt <= now) {
        return false;
    }

    result = it->second.result;
    return true;
}

void CacheResult(std::uint32_t sourceFormId, std::uint32_t listenerFormId, const Result& result) {
    if (sourceFormId == 0 || listenerFormId == 0) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(g_evalCacheMutex);
    if (g_evalCache.size() >= kEvalCacheMaxEntries) {
        for (auto it = g_evalCache.begin(); it != g_evalCache.end();) {
            if (it->second.expiresAt <= now) {
                it = g_evalCache.erase(it);
            } else {
                ++it;
            }
        }
        if (g_evalCache.size() >= kEvalCacheMaxEntries) {
            g_evalCache.erase(g_evalCache.begin());
        }
    }

    g_evalCache[EvalKey(sourceFormId, listenerFormId)] = {
        result,
        now + kEvalCacheTtl
    };
}

std::string ActorLabel(const ActorPositionResolverFNV::PositionResult& position, const char* fallback) {
    if (!position.actorName.empty()) {
        return position.actorName;
    }

    if (position.formId != 0) {
        char buffer[32] = {};
        sprintf_s(buffer, "form_%08X", position.formId);
        return buffer;
    }

    return fallback ? fallback : "unknown";
}

bool IsInteriorForPair(
    const ActorPositionResolverFNV::PositionResult& actor,
    const ActorPositionResolverFNV::PositionResult& other,
    bool sameCell) {
    if (actor.interiorKnown) {
        return actor.isInterior;
    }

    // Script-cached actors do not always expose cell interior metadata yet.
    // If the other reference is known interior and both refs are in the same
    // cell, treat the pair as interior so the shorter hearing range applies.
    return sameCell && other.interiorKnown && other.isInterior;
}

float ClampFinite(float value, float minValue, float maxValue, float fallback);

bool TryGetLineOfSight(
    const ActorPositionResolverFNV::PositionResult& source,
    const ActorPositionResolverFNV::PositionResult& listener,
    bool& hasLineOfSight) {
    if (source.formId == kPlayerFormId && listener.playerHasLosToActorKnown) {
        hasLineOfSight = listener.playerHasLosToActor;
        return true;
    }

    if (listener.formId == kPlayerFormId && source.actorHasLosToPlayerKnown) {
        hasLineOfSight = source.actorHasLosToPlayer;
        return true;
    }

    if (SpatialPathProviderFNV::TryGetNativeLineOfSight(
            source.formId, listener.formId, hasLineOfSight)) {
        return true;
    }

    return false;
}

bool IsClosedDoorState(int state) {
    return state == 3 || state == 4;
}

bool IsOpenDoorState(int state) {
    return state == 1 || state == 2;
}

float GetDoorTriangulationTolerance(float airDistance) {
    const float percent = ClampFinite(
        Config::spatialDoorTriangulationPercentTolerance,
        0.0f,
        1.0f,
        0.20f);
    const float absolute = std::max(
        0.0f,
        ClampFinite(Config::spatialDoorTriangulationAbsoluteTolerance, 0.0f, 10000.0f, 80.0f));
    return std::max(absolute, airDistance * percent);
}

bool IsBetweenActors(
    const ActorPositionResolverFNV::Vector3& sourcePosition,
    const ActorPositionResolverFNV::Vector3& listenerPosition,
    const ActorPositionResolverFNV::Vector3& doorPosition,
    float airDistance) {
    const float sourceToDoor = Distance(sourcePosition, doorPosition);
    const float listenerToDoor = Distance(listenerPosition, doorPosition);
    const float combinedDistance = sourceToDoor + listenerToDoor;
    const float tolerance = GetDoorTriangulationTolerance(airDistance);
    return combinedDistance >= (airDistance - tolerance) &&
        combinedDistance <= (airDistance + tolerance);
}

void ApplyDoorCounts(
    Result& result,
    const ActorPositionResolverFNV::PositionResult& source,
    const ActorPositionResolverFNV::PositionResult& listener,
    bool useInteriorDistance) {
    if (!useInteriorDistance || !result.sameCell || !source.cellResolved) {
        return;
    }

    const auto doors = ActorPositionResolverFNV::GetRecentDoorPositions();
    for (const auto& door : doors) {
        if (!door.resolved || !door.cellResolved || door.cellFormId != source.cellFormId) {
            continue;
        }

        if (!IsBetweenActors(source.position, listener.position, door.position, result.airDistance)) {
            continue;
        }

        if (IsClosedDoorState(door.openState)) {
            ++result.closedDoorCount;
        } else if (IsOpenDoorState(door.openState)) {
            ++result.openDoorCount;
        }
    }
}

float ClampFinite(float value, float minValue, float maxValue, float fallback) {
    if (!std::isfinite(value)) {
        return fallback;
    }

    return std::clamp(value, minValue, maxValue);
}

} // namespace

void InvalidateCache() {
    {
        std::lock_guard<std::mutex> lock(g_evalCacheMutex);
        g_evalCache.clear();
    }
    SpatialPathProviderFNV::InvalidateCache();
}

Result Evaluate(
    const ActorPositionResolverFNV::PositionResult& source,
    const ActorPositionResolverFNV::PositionResult& listener) {
    Result cached;
    if (TryGetCachedResult(source.formId, listener.formId, cached)) {
        return cached;
    }

    Result result;
    result.sourceResolved = source.resolved;
    result.listenerResolved = listener.resolved;

    const auto finalize = [&](const char* tier) -> Result {
        if (ShouldEmitSpatialEvalLog(source, listener, tier, result)) {
            Logger::LogDebug(
                "[SPATIAL_FNV] %s -> %s | tier=%s | reason=%s | can=%d | volume=%.3f | distance=%.1f | max=%.1f | pathDist=%.1f | pathRatio=%.2f | pathUsed=%d | pathFound=%d | losFallback=%d | los=%d | detect=%d | openDoors=%d | closedDoors=%d",
                ActorLabel(source, "source").c_str(),
                ActorLabel(listener, "listener").c_str(),
                tier ? tier : "",
                result.reason.c_str(),
                result.canCommunicate ? 1 : 0,
                result.volume,
                result.airDistance,
                result.maxDistance,
                result.pathDistance,
                result.pathRatio,
                result.navmeshPathUsed ? 1 : 0,
                result.navmeshPathFound ? 1 : 0,
                result.losFallbackUsed ? 1 : 0,
                result.hasLineOfSight ? 1 : 0,
                result.detectionLevel,
                result.openDoorCount,
                result.closedDoorCount);
        }
        CacheResult(source.formId, listener.formId, result);
        return result;
    };

    if (!Config::spatialAudioEnabled) {
        result.canCommunicate = true;
        result.volume = 1.0f;
        result.reason = "spatial_disabled";
        if (source.resolved && listener.resolved) {
            result.airDistance = Distance(source.position, listener.position);
            result.sameCell = source.cellResolved && listener.cellResolved &&
                source.cellFormId == listener.cellFormId;
        }
        return finalize("tier0_disabled");
    }

    if (!source.resolved) {
        result.reason = source.reason.empty() ? "source_unresolved" : "source_" + source.reason;
        return finalize("tier0_source_unresolved");
    }

    if (!listener.resolved) {
        result.reason = listener.reason.empty() ? "listener_unresolved" : "listener_" + listener.reason;
        return finalize("tier0_listener_unresolved");
    }

    if (source.formId != 0 && source.formId == listener.formId) {
        result.reason = "same_actor";
        return finalize("tier0_same_actor");
    }

    if (!source.cellResolved || !listener.cellResolved) {
        result.reason = "missing_cell";
        return finalize("tier0_missing_cell");
    }

    result.sameCell = source.cellFormId == listener.cellFormId;

    if (source.interiorKnown && listener.interiorKnown &&
        source.isInterior != listener.isInterior) {
        result.reason = "interior_exterior_boundary";
        return finalize("tier0_worldspace_boundary");
    }

    if (source.interiorKnown && listener.interiorKnown &&
        source.isInterior && listener.isInterior && !result.sameCell) {
        result.reason = "different_interior_cells";
        return finalize("tier0_cell_boundary");
    }

    result.airDistance = Distance(source.position, listener.position);
    if (!std::isfinite(result.airDistance)) {
        result.reason = "invalid_distance";
        return finalize("tier1_invalid_distance");
    }

    if (Config::spatialMaxAirDistance > 0.0f &&
        result.airDistance > Config::spatialMaxAirDistance) {
        result.reason = "too_far";
        return finalize("tier1_too_far");
    }

    const bool sourceInterior = IsInteriorForPair(source, listener, result.sameCell);
    const bool listenerInterior = IsInteriorForPair(listener, source, result.sameCell);
    const bool useInteriorDistance = sourceInterior && listenerInterior && result.sameCell;

    result.maxDistance = useInteriorDistance
        ? Config::spatialInteriorHearingDistance
        : Config::spatialExteriorHearingDistance;

    const bool playerSpeaker = source.formId == kPlayerFormId;
    const float autoHearingDistance = Config::spatialAutoHearingDistance > 0.0f
        ? Config::spatialAutoHearingDistance
        : Config::spatialImmediateDistance;
    if (playerSpeaker && autoHearingDistance > 0.0f &&
        result.airDistance <= autoHearingDistance) {
        result.canCommunicate = true;
        result.volume = 1.0f;
        result.reason = "immediate_proximity";
        return finalize("tier1_auto_hearing_radius");
    }

    if (result.maxDistance > 0.0f && result.airDistance > result.maxDistance) {
        result.reason = "too_far";
        return finalize("tier1_hearing_range");
    }

    ApplyDoorCounts(result, source, listener, useInteriorDistance);
    if (result.closedDoorCount > 0) {
        result.reason = "closed_door_between";
        return finalize("tier3_closed_door");
    }

    if (Config::spatialImmediateDistance > 0.0f &&
        result.airDistance <= Config::spatialImmediateDistance &&
        result.openDoorCount == 0) {
        result.canCommunicate = true;
        result.volume = 1.0f;
        result.reason = "immediate_proximity";
        return finalize("tier1_immediate_pass");
    }

    const float maxDistance = std::max(result.maxDistance, 1.0f);
    const float rawDistanceFactor = std::clamp(1.0f - (result.airDistance / maxDistance), 0.0f, 1.0f);
    const float distanceScaler = std::max(0.25f, Config::spatialDistanceScaler);
    const float scaledDistanceFactor = std::pow(rawDistanceFactor, distanceScaler);
    const float distanceFactor = std::clamp(
        scaledDistanceFactor,
        ClampFinite(Config::spatialMinDistanceFactor, 0.0f, 1.0f, 0.1f),
        1.0f);
    const float environmentModifier = useInteriorDistance
        ? ClampFinite(Config::spatialInteriorBaseModifier, 0.0f, 2.0f, 1.0f)
        : ClampFinite(Config::spatialExteriorBaseModifier, 0.0f, 2.0f, 0.7f);
    const float openDoorModifier = std::pow(
        ClampFinite(Config::spatialOpenDoorPenaltyBase, 0.0f, 1.0f, 0.85f),
        static_cast<float>(result.openDoorCount));

    bool hasLineOfSight = false;
    const bool losKnown = TryGetLineOfSight(source, listener, hasLineOfSight);
    result.losFallbackUsed = losKnown;
    result.losQueryOk = losKnown;
    result.hasLineOfSight = losKnown && hasLineOfSight;

    if (result.hasLineOfSight) {
        result.volume = std::clamp(distanceFactor * environmentModifier * openDoorModifier, 0.0f, 1.0f);
        result.reason = result.openDoorCount > 0 ? "open_door_muffled" : "line_of_sight_clear";

        if (result.volume < Config::spatialMinimumAudibleVolume) {
            result.reason = "too_quiet";
            return finalize("tier4_los_too_quiet");
        }

        result.canCommunicate = true;
        return finalize("tier4_los_pass");
    }

    float pathModifier = 1.0f;
    const auto pathResult = SpatialPathProviderFNV::EvaluatePath(source, listener);
    result.openDoorCount = std::max(result.openDoorCount, pathResult.openDoorCount);
    result.closedDoorCount = std::max(result.closedDoorCount, pathResult.closedDoorCount);
    if (pathResult.closedDoorCount > 0) {
        result.navmeshPathUsed = pathResult.nativeGraphUsed;
        result.navmeshPathFound = true;
        result.reason = "closed_door_between";
        return finalize("tier5_native_closed_door");
    }
    if (pathResult.status == SpatialPathProviderFNV::PathStatus::NoPath) {
        result.navmeshPathUsed = true;
        result.navmeshPathFound = false;
        result.reason = "navmesh_no_path";
        return finalize("tier5_navmesh_no_path");
    }

    if (pathResult.status == SpatialPathProviderFNV::PathStatus::Success &&
        std::isfinite(pathResult.pathDistance) && pathResult.pathDistance >= 0.0f) {
        result.navmeshPathUsed = true;
        result.navmeshPathFound = true;
        result.pathDistance = pathResult.pathDistance;

        if (result.airDistance > 0.001f) {
            result.pathRatio = result.pathDistance / result.airDistance;

            if (result.pathRatio >= ClampFinite(Config::spatialPathRatioReject, 1.0f, 100.0f, 4.0f)) {
                result.reason = "path_ratio_blocked";
                return finalize("tier5_ratio_blocked");
            }

            if (result.pathRatio >= ClampFinite(Config::spatialPathRatioDistanceReject, 1.0f, 100.0f, 2.5f) &&
                result.airDistance >= ClampFinite(Config::spatialPathRatioDistanceRejectMinAir, 0.0f, 100000.0f, 500.0f)) {
                result.reason = "path_ratio_distance_blocked";
                return finalize("tier5_ratio_distance_blocked");
            }

            const float complexityStart = ClampFinite(Config::spatialPathComplexityStartRatio, 1.0f, 100.0f, 1.2f);
            if (result.pathRatio > complexityStart) {
                const float complexityScale = ClampFinite(Config::spatialPathComplexityScale, 0.01f, 10.0f, 0.6f);
                const float complexityMin = ClampFinite(Config::spatialPathComplexityMin, 0.0f, 1.0f, 0.3f);
                pathModifier = std::clamp(
                    1.0f / std::max(result.pathRatio * complexityScale, 0.01f),
                    complexityMin,
                    1.0f);
            }
        }
    } else if (!Config::spatialAllowPathUnavailableFallback) {
        result.reason = "path_unavailable";
        return finalize("tier5_path_unavailable");
    }

    const float obstructionModifier = losKnown
        ? ClampFinite(Config::spatialAroundCornerPenalty, 0.0f, 1.0f, 0.60f)
        : 1.0f;

    result.volume = std::clamp(
        distanceFactor * environmentModifier * openDoorModifier * obstructionModifier * pathModifier,
        0.0f,
        1.0f);
    if (result.navmeshPathFound) {
        result.reason = result.openDoorCount > 0 ? "open_door_muffled" : "path_fallback_clear";
    } else {
        result.reason = losKnown
            ? "line_of_sight_blocked_muffled"
            : (result.openDoorCount > 0 ? "open_door_muffled" : "distance_fallback_clear");
    }

    if (result.volume < Config::spatialMinimumAudibleVolume) {
        result.reason = "too_quiet";
        return finalize("tier8_too_quiet");
    }

    result.canCommunicate = true;
    return finalize("tier8_distance_pass");
}

} // namespace SpatialAwarenessFNV
