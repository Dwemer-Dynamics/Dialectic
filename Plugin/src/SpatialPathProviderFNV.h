// SpatialPathProviderFNV.h - Optional FNV path-distance bridge for spatial awareness

#pragma once

#include "ActorPositionResolverFNV.h"
#include "RuntimeSnapshot.h"

#include <cstdint>

namespace SpatialPathProviderFNV {

enum class PathStatus {
    Unavailable,
    NoPath,
    Success
};

struct PathResult {
    PathStatus status = PathStatus::Unavailable;
    float airDistance = 0.0f;
    float pathDistance = -1.0f;
    int openDoorCount = 0;
    int closedDoorCount = 0;
    bool nativeGraphUsed = false;
};

struct NativeGraphStatus {
    std::uint32_t cellFormId = 0;
    std::uint64_t generation = 0;
    std::size_t nodes = 0;
    std::size_t edges = 0;
    bool ready = false;
    bool building = false;
};

void PublishNativeScene(const RuntimeSnapshot::NavSceneState& scene);
NativeGraphStatus GetNativeGraphStatus();

PathResult EvaluatePath(
    const ActorPositionResolverFNV::PositionResult& source,
    const ActorPositionResolverFNV::PositionResult& listener);

void RememberScriptPathResult(
    std::uint32_t sourceFormId,
    std::uint32_t listenerFormId,
    PathStatus status,
    float airDistance,
    float pathDistance);

bool TryGetNativeLineOfSight(std::uint32_t sourceFormId,
                             std::uint32_t listenerFormId,
                             bool& hasLineOfSight);

void InvalidateCache();

} // namespace SpatialPathProviderFNV
