#include "NPCDetector.h"

#include "Config.h"
#include "Logger.h"
#include "RuntimeSnapshot.h"

#include <algorithm>
#include <chrono>

namespace NPCDetector {
namespace {

bool g_initialized = false;

NPCInfo FromActor(const RuntimeSnapshot::ActorState& actor) {
    NPCInfo info;
    info.formId = actor.formId;
    info.name = actor.name.empty() ? "Unknown NPC" : actor.name;
    info.distance = actor.distanceToPlayer;
    info.isCreature = actor.creature;
    info.isDead = actor.dead;
    info.isHostile = actor.hostileToPlayer;
    info.isValid = actor.formId != 0 && !actor.deleted && actor.loaded3D;
    return info;
}

} // namespace

void Initialize() {
    g_initialized = true;
    Logger::LogInfo("NPCDetector: using native runtime actor snapshot");
}

void Shutdown() {
    g_initialized = false;
}

NPCInfo GetCrosshairNPC() {
    if (!g_initialized) {
        return {};
    }
    RuntimeSnapshot::GameState game;
    if (!RuntimeSnapshot::TryGetFreshGameState(game, std::chrono::milliseconds(500)) ||
        game.crosshairFormId == 0) {
        return {};
    }
    RuntimeSnapshot::ActorState actor;
    return RuntimeSnapshot::TryGetActor(game.crosshairFormId, actor)
        ? FromActor(actor) : NPCInfo{};
}

std::vector<NPCInfo> GetNearbyNPCs(float maxDistance) {
    std::vector<NPCInfo> npcs;
    if (!g_initialized || maxDistance < 0.0f) {
        return npcs;
    }
    RuntimeSnapshot::GameState game;
    if (!RuntimeSnapshot::TryGetFreshGameState(game, std::chrono::milliseconds(500))) {
        return npcs;
    }
    for (const RuntimeSnapshot::ActorState& actor : RuntimeSnapshot::GetActors()) {
        if (!RuntimeSnapshot::IsActorInScene(actor, game)) {
            continue;
        }
        NPCInfo info = FromActor(actor);
        if (info.isValid && info.distance <= maxDistance) {
            npcs.push_back(std::move(info));
        }
    }
    std::sort(npcs.begin(), npcs.end(), [](const NPCInfo& left, const NPCInfo& right) {
        if (left.distance != right.distance) {
            return left.distance < right.distance;
        }
        return left.formId < right.formId;
    });
    return npcs;
}

NPCInfo GetClosestNPC(float maxDistance) {
    NPCInfo crosshair = GetCrosshairNPC();
    if (crosshair.isValid && !crosshair.isDead && crosshair.distance <= maxDistance) {
        return crosshair;
    }
    const auto nearby = GetNearbyNPCs(maxDistance);
    const auto candidate = std::find_if(nearby.begin(), nearby.end(), [](const NPCInfo& npc) {
        return npc.isValid && !npc.isDead;
    });
    return candidate != nearby.end() ? *candidate : NPCInfo{};
}

bool IsActor(uint32_t formId) {
    RuntimeSnapshot::ActorState actor;
    return formId != 0 && RuntimeSnapshot::TryGetActor(formId, actor) &&
        !actor.deleted && actor.loaded3D;
}

bool IsExcluded(uint32_t formId, const std::string& name) {
    if (Config::IsFormIDExcluded(formId)) {
        Logger::LogDebug("NPCDetector: FormID 0x%08X is excluded", formId);
        return true;
    }
    if (Config::IsNPCExcluded(name)) {
        Logger::LogDebug("NPCDetector: NPC name '%s' is excluded", name.c_str());
        return true;
    }
    return false;
}

} // namespace NPCDetector
