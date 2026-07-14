// SpatialAwarenessFNV.h - FNV spatial audibility for Dialectic

#pragma once

#include "ActorPositionResolverFNV.h"

#include <string>

namespace SpatialAwarenessFNV {

struct Result {
    bool canCommunicate = false;
    float volume = 0.0f;
    float airDistance = 0.0f;
    float pathDistance = -1.0f;
    float pathRatio = -1.0f;
    float maxDistance = 0.0f;
    int detectionLevel = -999;
    int openDoorCount = 0;
    int closedDoorCount = 0;
    bool navmeshPathUsed = false;
    bool navmeshPathFound = false;
    bool losFallbackUsed = false;
    bool losQueryOk = false;
    bool hasLineOfSight = false;
    bool sourceResolved = false;
    bool listenerResolved = false;
    bool sameCell = false;
    std::string reason = "unknown";
};

Result Evaluate(
    const ActorPositionResolverFNV::PositionResult& source,
    const ActorPositionResolverFNV::PositionResult& listener);
void InvalidateCache();

} // namespace SpatialAwarenessFNV
