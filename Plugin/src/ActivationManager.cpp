// ActivationManager.cpp - AI agent activation policy for Dialectic

#include "ActivationManager.h"
#include "AgentManager.h"
#include "ActorEligibilityFNV.h"
#include "ActorPositionResolverFNV.h"
#include "Config.h"
#include "Console.h"
#include "Logger.h"
#include "NPCDetector.h"
#include "SpatialAwarenessFNV.h"
#include "TargetManager.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <unordered_map>
#include <string>
#include <vector>

namespace ActivationManager {

struct ActivationCandidate {
    uint32_t formId = 0;
    std::string name;
    float distance = 0.0f;
    bool isActor = false;
    bool isAlive = false;
    bool isHostile = false;
    bool isCreature = false;
    bool creatureKnown = false;
    bool valid = false;
    bool lightweightMetadata = false;
    int baseType = 0;
    bool baseTypeKnown = false;
    std::string baseId;
    std::string gender;
    std::string race;
    std::string voiceId;
    std::string voiceFormId;
    std::string voiceName;
    bool sceneBusyKnown = false;
    bool sceneBusy = false;
    bool deadKnown = false;
    bool isDead = false;
    bool disabledKnown = false;
    bool isDisabled = false;
};

static bool g_initialized = false;
static uint32_t g_lastAutoTargetFormId = 0;
static std::chrono::steady_clock::time_point g_lastAutoUpdateTime = {};
static std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> g_lastProfileRefreshTime;
static constexpr auto kAutoUpdateInterval = std::chrono::milliseconds(250);
static constexpr auto kAutoProfileRefreshInterval = std::chrono::seconds(60);

static const char* SourceName(ActivationSource source) {
    return source == ActivationSource::Manual ? "manual" : "auto";
}

static float MaxAutoActivationDistance() {
    return std::max(Config::distanceActivatingNpcInterior, Config::distanceActivatingNpcExterior);
}

static bool HasUsefulMetadata(const std::string& value) {
    return !value.empty() &&
        value != "Unknown" &&
        value != "<no name>";
}

static void ApplyPositionMetadata(
    ActivationCandidate& candidate,
    const ActorPositionResolverFNV::PositionResult& position) {
    if (!position.resolved) {
        return;
    }

    if (candidate.name.empty() && !position.actorName.empty()) {
        candidate.name = position.actorName;
    }
    candidate.baseId = position.baseId;
    candidate.gender = position.gender;
    candidate.race = position.race;
    candidate.voiceId = position.voiceId;
    candidate.voiceFormId = position.voiceFormId;
    candidate.voiceName = position.voiceName;
    candidate.baseType = position.baseType;
    candidate.baseTypeKnown = position.baseTypeKnown;
    candidate.sceneBusyKnown = position.sceneBusyKnown;
    candidate.sceneBusy = position.sceneBusy;
    candidate.deadKnown = position.deadKnown;
    candidate.isDead = position.isDead;
    candidate.disabledKnown = position.disabledKnown;
    candidate.isDisabled = position.isDisabled;
    if (position.deadKnown && position.isDead) {
        candidate.isAlive = false;
    }
    if (position.baseTypeKnown) {
        candidate.isCreature = position.baseType == 0x2B;
        candidate.creatureKnown = true;
    }
}

static ActorEligibilityFNV::Metadata ToEligibilityMetadata(const ActivationCandidate& candidate) {
    ActorEligibilityFNV::Metadata metadata;
    metadata.name = candidate.name;
    metadata.race = candidate.race;
    metadata.voiceId = candidate.voiceId;
    metadata.voiceName = candidate.voiceName;
    metadata.baseId = candidate.baseId;
    metadata.baseType = candidate.baseType;
    metadata.baseTypeKnown = candidate.baseTypeKnown;
    metadata.isCreature = candidate.isCreature;
    metadata.isCreatureKnown = candidate.creatureKnown;
    return metadata;
}

static bool HasStrictEligibilityMetadata(const ActivationCandidate& candidate) {
    return candidate.creatureKnown ||
        candidate.baseTypeKnown ||
        HasUsefulMetadata(candidate.race) ||
        HasUsefulMetadata(candidate.voiceId) ||
        HasUsefulMetadata(candidate.voiceName);
}

static ActivationCandidate FromTargetInfo(const TargetManager::TargetInfo& target) {
    ActivationCandidate candidate;
    candidate.formId = target.formId;
    candidate.name = target.name;
    candidate.distance = target.distance;
    candidate.isActor = target.isActor;
    candidate.isAlive = target.isAlive;
    candidate.isHostile = target.isHostile;
    candidate.valid = target.formId != 0 && target.isActor && target.isAlive && !target.name.empty();
    if (candidate.valid) {
        ApplyPositionMetadata(candidate, ActorPositionResolverFNV::ResolveActor(candidate.formId));
    }
    return candidate;
}

static ActivationCandidate FromNPCInfo(const NPCDetector::NPCInfo& npc) {
    ActivationCandidate candidate;
    candidate.formId = npc.formId;
    candidate.name = npc.name;
    candidate.distance = npc.distance;
    candidate.isActor = npc.isValid;
    candidate.isAlive = !npc.isDead;
    candidate.isHostile = npc.isHostile;
    candidate.isCreature = npc.isCreature;
    candidate.creatureKnown = true;
    candidate.baseType = npc.isCreature ? 0x2B : 0x2A;
    candidate.baseTypeKnown = true;
    candidate.valid = npc.isValid && npc.formId != 0 && !npc.isDead && !npc.name.empty();
    return candidate;
}

static float DistanceBetween(
    const ActorPositionResolverFNV::Vector3& a,
    const ActorPositionResolverFNV::Vector3& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
}

static ActivationCandidate FromPositionResult(
    const ActorPositionResolverFNV::PositionResult& position,
    const ActorPositionResolverFNV::PositionResult& player) {
    ActivationCandidate candidate;
    candidate.formId = position.formId;
    candidate.name = position.actorName;
    candidate.distance = player.resolved ? DistanceBetween(player.position, position.position) : 0.0f;
    candidate.isActor = position.resolved;
    candidate.isAlive = true;
    candidate.isHostile = false;
    candidate.valid = position.resolved && position.formId != 0 && position.formId != 0x00000014 && !position.actorName.empty();
    candidate.lightweightMetadata = true;
    ApplyPositionMetadata(candidate, position);
    return candidate;
}

static ActivationCandidate FindCurrentCandidate() {
    ActivationCandidate candidate = FromTargetInfo(TargetManager::GetCurrentTarget());
    if (candidate.valid) {
        return candidate;
    }

    NPCDetector::NPCInfo crosshairNPC = NPCDetector::GetCrosshairNPC();
    candidate = FromNPCInfo(crosshairNPC);
    if (candidate.valid) {
        TargetManager::SetCurrentTarget(candidate.formId, candidate.name, true);
    }

    return candidate;
}

static bool ShouldSkipCandidate(
    const ActivationCandidate& candidate,
    ActivationSource source,
    std::string& reason) {
    if (!candidate.valid || !candidate.isActor || candidate.formId == 0) {
        reason = "no valid NPC target";
        return true;
    }

    if (!candidate.isAlive) {
        reason = "target is dead";
        return true;
    }

    if (candidate.deadKnown && candidate.isDead) {
        reason = "target is dead";
        return true;
    }

    if (candidate.disabledKnown && candidate.isDisabled) {
        reason = "target is disabled";
        return true;
    }

    if (Config::IsNPCExcluded(candidate.name)) {
        reason = "name is excluded";
        return true;
    }

    if (Config::IsFormIDExcluded(candidate.formId)) {
        reason = "form id is excluded";
        return true;
    }

    if (Config::sceneSafetyEnabled && candidate.sceneBusyKnown && candidate.sceneBusy) {
        reason = "actor is currently controlled by a scene/dialogue package";
        return true;
    }

    if (source == ActivationSource::Auto) {
        if (candidate.isHostile && !Config::autoAddHostile) {
            reason = "hostile NPCs are disabled";
            return true;
        }

        if (!Config::autoAddCreatures) {
            const ActorEligibilityFNV::Metadata metadata = ToEligibilityMetadata(candidate);
            std::string eligibilityReason;
            if (ActorEligibilityFNV::IsClearlyDisallowedCreature(metadata, &eligibilityReason)) {
                reason = eligibilityReason;
                return true;
            }

            if (HasStrictEligibilityMetadata(candidate) &&
                !ActorEligibilityFNV::IsAutoActivationAllowed(metadata, &eligibilityReason)) {
                reason = eligibilityReason;
                return true;
            }
        }

        const float maxDistance = MaxAutoActivationDistance();
        if (candidate.distance > 0.0f && candidate.distance > maxDistance) {
            reason = "target is outside auto activation distance";
            return true;
        }
    }

    return false;
}

static AgentManager::RegistrationSource ToRegistrationSource(ActivationSource source) {
    return source == ActivationSource::Manual
        ? AgentManager::RegistrationSource::Manual
        : AgentManager::RegistrationSource::Auto;
}

static AgentManager::NPCData ToNPCData(const ActivationCandidate& candidate) {
    AgentManager::NPCData data;
    data.displayName = candidate.name;
    data.refID = candidate.formId;
    data.baseName = candidate.baseId;
    data.gender = candidate.gender;
    data.race = candidate.race;
    data.voiceId = candidate.voiceId;
    data.voiceFormId = candidate.voiceFormId;
    data.voiceName = candidate.voiceName;
    return data;
}

static bool ShouldRefreshProfile(uint32_t formId, std::chrono::steady_clock::time_point now);

static bool ActivateCandidate(const ActivationCandidate& candidate, ActivationSource source) {
    ActorPositionResolverFNV::RememberActorPosition(
        ActorPositionResolverFNV::ResolveActor(candidate.formId));

    const bool alreadyAgent = AgentManager::IsAIAgent(candidate.formId);
    const bool alreadyManual = AgentManager::IsManuallyActivated(candidate.formId);

    AgentManager::RegisterAIAgent(candidate.formId, candidate.name, ToRegistrationSource(source), candidate.distance);
    TargetManager::RegisterAIAgent(candidate.formId, candidate.name);

    if (alreadyAgent) {
        AgentManager::MarkAgentSeen(candidate.formId, candidate.distance);

        if (source == ActivationSource::Manual && !alreadyManual) {
            Logger::LogInfo("ActivationManager: Manual activation upgraded existing agent %s (0x%08X)",
                candidate.name.c_str(), candidate.formId);
            AgentManager::SendActorProfile(candidate.name, candidate.formId);
            g_lastProfileRefreshTime[candidate.formId] = std::chrono::steady_clock::now();
            Console::Print("[Dialectic] Manually activated AI agent: %s", candidate.name.c_str());
            return true;
        }

        Logger::LogDebug("ActivationManager: %s activation ignored; %s (0x%08X) is already an AI agent",
            SourceName(source), candidate.name.c_str(), candidate.formId);
        if (source == ActivationSource::Manual) {
            Console::Print("[Dialectic] %s is already an AI agent", candidate.name.c_str());
        }
        return false;
    }

    Logger::LogInfo("ActivationManager: %s activating %s (0x%08X)",
        SourceName(source), candidate.name.c_str(), candidate.formId);

    if (candidate.lightweightMetadata && source == ActivationSource::Auto) {
        AgentManager::SendActorProfile(ToNPCData(candidate));
    } else {
        AgentManager::SendActorProfile(candidate.name, candidate.formId);
    }
    g_lastProfileRefreshTime[candidate.formId] = std::chrono::steady_clock::now();

    if (source == ActivationSource::Manual) {
        Console::Print("[Dialectic] Manually activated AI agent: %s", candidate.name.c_str());
    }

    return true;
}

static void RefreshManagedCandidateIfDue(const ActivationCandidate& candidate) {
    if (!candidate.valid || candidate.formId == 0 || !AgentManager::IsAIAgent(candidate.formId)) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (!ShouldRefreshProfile(candidate.formId, now)) {
        return;
    }

    if (candidate.lightweightMetadata) {
        AgentManager::SendActorProfile(ToNPCData(candidate));
    } else {
        AgentManager::SendActorProfile(candidate.name, candidate.formId);
    }
}

static bool ShouldRefreshProfile(uint32_t formId, std::chrono::steady_clock::time_point now) {
    if (formId == 0) {
        return false;
    }

    auto it = g_lastProfileRefreshTime.find(formId);
    if (it == g_lastProfileRefreshTime.end()) {
        g_lastProfileRefreshTime[formId] = now;
        return true;
    }

    if (now - it->second >= kAutoProfileRefreshInterval) {
        it->second = now;
        return true;
    }

    return false;
}

static bool IsWithinDirectAutoHearingRadius(const SpatialAwarenessFNV::Result& spatial) {
    const float autoHearingDistance = Config::spatialAutoHearingDistance > 0.0f
        ? Config::spatialAutoHearingDistance
        : 560.0f;
    return autoHearingDistance > 0.0f && spatial.airDistance > 0.0f &&
        spatial.airDistance <= autoHearingDistance;
}

void Initialize() {
    g_lastAutoTargetFormId = 0;
    g_lastAutoUpdateTime = {};
    g_lastProfileRefreshTime.clear();
    g_initialized = true;
    Logger::LogInfo("ActivationManager: Initialized");
}

void Shutdown() {
    g_initialized = false;
    g_lastAutoTargetFormId = 0;
    g_lastAutoUpdateTime = {};
    g_lastProfileRefreshTime.clear();
    Logger::LogInfo("ActivationManager: Shutdown");
}

bool ActivateCurrentTarget(ActivationSource source) {
    if (!g_initialized) {
        Logger::LogWarning("ActivationManager: Activation requested before initialization");
        return false;
    }

    ActivationCandidate candidate = FindCurrentCandidate();
    std::string reason;
    if (ShouldSkipCandidate(candidate, source, reason)) {
        Logger::LogInfo("ActivationManager: Skipping %s activation: %s", SourceName(source), reason.c_str());
        if (source == ActivationSource::Manual) {
            Console::Print("[Dialectic] Cannot activate target: %s", reason.c_str());
        }
        return false;
    }

    return ActivateCandidate(candidate, source);
}

bool ActivateActor(uint32_t formId, ActivationSource source) {
    if (!g_initialized || formId == 0) {
        return false;
    }

    const auto player = ActorPositionResolverFNV::ResolvePlayer();
    const auto position = ActorPositionResolverFNV::ResolveActor(formId);
    ActivationCandidate candidate = FromPositionResult(position, player);
    std::string reason;
    if (ShouldSkipCandidate(candidate, source, reason)) {
        Logger::LogInfo("ActivationManager: Skipping managed %s activation for 0x%08X: %s",
            SourceName(source), formId, reason.c_str());
        return false;
    }
    return ActivateCandidate(candidate, source);
}

std::vector<std::pair<uint32_t, std::string>> GetNearbyManageableActors() {
    std::vector<std::pair<uint32_t, std::string>> result;
    if (!g_initialized) {
        return result;
    }

    const auto player = ActorPositionResolverFNV::ResolvePlayer();
    if (!player.resolved) {
        return result;
    }

    struct CandidateDistance {
        uint32_t formId;
        std::string name;
        float distance;
    };
    std::vector<CandidateDistance> candidates;
    const float maxDistance = MaxAutoActivationDistance();
    for (const auto& position : ActorPositionResolverFNV::GetRecentActorPositions()) {
        ActivationCandidate candidate = FromPositionResult(position, player);
        std::string reason;
        if (ShouldSkipCandidate(candidate, ActivationSource::Manual, reason)) {
            continue;
        }
        if (!ActorPositionResolverFNV::IsPositionInPlayerScene(position) ||
            (maxDistance > 0.0f && candidate.distance > maxDistance)) {
            continue;
        }
        candidates.push_back({ candidate.formId, candidate.name, candidate.distance });
    }

    std::sort(candidates.begin(), candidates.end(),
        [](const CandidateDistance& left, const CandidateDistance& right) {
            return left.distance < right.distance;
        });
    result.reserve(candidates.size());
    for (const auto& candidate : candidates) {
        result.emplace_back(candidate.formId, candidate.name);
    }
    return result;
}

std::size_t ActivateNearbyActors(ActivationSource source) {
    std::size_t activated = 0;
    for (const auto& actor : GetNearbyManageableActors()) {
        if (AgentManager::IsAIAgent(actor.first)) {
            continue;
        }
        if (ActivateActor(actor.first, source)) {
            ++activated;
        }
    }
    return activated;
}

bool DeactivateActor(uint32_t formId) {
    if (!AgentManager::UnregisterAIAgent(formId)) {
        return false;
    }
    TargetManager::UnregisterAIAgent(formId);
    g_lastProfileRefreshTime.erase(formId);
    return true;
}

std::size_t DeactivateAllActors() {
    const auto agents = AgentManager::GetRegisteredAgentSnapshot();
    for (const auto& agent : agents) {
        TargetManager::UnregisterAIAgent(agent.first);
        g_lastProfileRefreshTime.erase(agent.first);
    }
    return AgentManager::UnregisterAllAIAgents();
}

static void ActivateNearbySpatialCandidates() {
    ActorPositionResolverFNV::PositionResult player = ActorPositionResolverFNV::ResolvePlayer();
    if (!player.resolved) {
        Logger::LogDebug("ActivationManager: Nearby spatial activation skipped; player position unresolved (%s)",
            player.reason.c_str());
        return;
    }

    std::vector<ActorPositionResolverFNV::PositionResult> positions =
        ActorPositionResolverFNV::GetRecentActorPositions();
    std::sort(positions.begin(), positions.end(),
        [&player](const auto& left, const auto& right) {
            const float leftDistance = player.resolved
                ? DistanceBetween(player.position, left.position)
                : 0.0f;
            const float rightDistance = player.resolved
                ? DistanceBetween(player.position, right.position)
                : 0.0f;
            return leftDistance < rightDistance;
        });

    int activated = 0;
    int refreshed = 0;
    const auto now = std::chrono::steady_clock::now();
    constexpr int kMaxNearbyActivationsPerUpdate = 6;
    constexpr int kMaxProfileRefreshesPerUpdate = 2;
    for (const auto& position : positions) {
        if (activated >= kMaxNearbyActivationsPerUpdate) {
            break;
        }

        ActivationCandidate candidate = FromPositionResult(position, player);
        const auto spatial = SpatialAwarenessFNV::Evaluate(player, position);
        const bool directAutoHearing = IsWithinDirectAutoHearingRadius(spatial);
        if (!spatial.canCommunicate && !directAutoHearing) {
            continue;
        }

        std::string reason;
        if (ShouldSkipCandidate(candidate, ActivationSource::Auto, reason)) {
            continue;
        }

        if (AgentManager::IsAIAgent(candidate.formId)) {
            AgentManager::MarkAgentSeen(candidate.formId, candidate.distance);
            if (refreshed < kMaxProfileRefreshesPerUpdate &&
                ShouldRefreshProfile(candidate.formId, now)) {
                AgentManager::SendActorProfile(ToNPCData(candidate));
                ++refreshed;
            }
            continue;
        }

        if (ActivateCandidate(candidate, ActivationSource::Auto)) {
            ++activated;
        }
    }
}

void Update() {
    if (!g_initialized) {
        return;
    }

    if (!Config::autoActivateEnabled) {
        g_lastAutoTargetFormId = 0;
        g_lastAutoUpdateTime = {};
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (g_lastAutoUpdateTime.time_since_epoch().count() != 0 &&
        now - g_lastAutoUpdateTime < kAutoUpdateInterval) {
        return;
    }
    g_lastAutoUpdateTime = now;

    ActivationCandidate candidate = FindCurrentCandidate();
    if (!candidate.valid) {
        g_lastAutoTargetFormId = 0;
        ActivateNearbySpatialCandidates();
        return;
    }

    if (candidate.formId == g_lastAutoTargetFormId) {
        ActorPositionResolverFNV::RememberActorPosition(
            ActorPositionResolverFNV::ResolveActor(candidate.formId));
        if (AgentManager::IsAIAgent(candidate.formId)) {
            AgentManager::MarkAgentSeen(candidate.formId, candidate.distance);
            RefreshManagedCandidateIfDue(candidate);
        }
        ActivateNearbySpatialCandidates();
        return;
    }

    g_lastAutoTargetFormId = candidate.formId;
    if (AgentManager::IsAIAgent(candidate.formId)) {
        AgentManager::MarkAgentSeen(candidate.formId, candidate.distance);
        RefreshManagedCandidateIfDue(candidate);
        ActivateNearbySpatialCandidates();
        return;
    }

    ActivateCurrentTarget(ActivationSource::Auto);
    ActivateNearbySpatialCandidates();
}

} // namespace ActivationManager

