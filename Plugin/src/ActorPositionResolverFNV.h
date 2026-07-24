// ActorPositionResolverFNV.h - FNV actor/player position resolution for spatial audio

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ActorPositionResolverFNV {

struct Vector3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct PositionResult {
    bool resolved = false;
    uint32_t formId = 0;
    std::string actorName;
    std::string baseId;
    std::string gender;
    std::string race;
    std::string voiceId;
    std::string voiceFormId;
    std::string voiceName;
    int level = 0;
    int baseType = 0;
    bool baseTypeKnown = false;
    bool playerTeammateKnown = false;
    bool isPlayerTeammate = false;
    bool sceneBusyKnown = false;
    bool sceneBusy = false;
    bool disabledKnown = false;
    bool isDisabled = false;
    bool deadKnown = false;
    bool isDead = false;
    Vector3 position;
    float yaw = 0.0f;
    bool yawResolved = false;
    uint32_t cellFormId = 0;
    bool cellResolved = false;
    uint32_t worldspaceFormId = 0;
    bool worldspaceResolved = false;
    bool interiorKnown = false;
    bool isInterior = false;
    bool actorHasLosToPlayerKnown = false;
    bool actorHasLosToPlayer = false;
    bool playerHasLosToActorKnown = false;
    bool playerHasLosToActor = false;
    std::string source;
    std::string reason;
};

struct DoorPosition {
    bool resolved = false;
    uint32_t formId = 0;
    Vector3 position;
    uint32_t cellFormId = 0;
    bool cellResolved = false;
    int openState = 0;
};

PositionResult ResolvePlayer();
PositionResult ResolveCurrentCrosshairActor();
PositionResult ResolveActor(uint32_t formId);
bool IsPositionInPlayerScene(const PositionResult& position);
bool IsActorInPlayerScene(uint32_t formId);
bool IsActorPositionFresh(uint32_t formId, int maxAgeMs);
bool IsActorInLatestScan(uint32_t formId, int maxAgeMs = 5000);
std::vector<PositionResult> GetRecentActorPositions();
std::vector<DoorPosition> GetRecentDoorPositions();
void RememberActorPosition(const PositionResult& position);
void RememberPlayerSneaking(bool sneaking);
bool IsPlayerSneaking();
void InvalidateCache();

} // namespace ActorPositionResolverFNV
