// NearbyActorsFNV.cpp - Structured nearby actor snapshots for DialecticServer prompts

#include "NearbyActorsFNV.h"

#include "ActorEligibilityFNV.h"
#include "ActorPositionResolverFNV.h"
#include "AutoGreetingFNV.h"
#include "AgentManager.h"
#include "Config.h"
#include "HTTPManager.h"
#include "Logger.h"
#include "Misc.h"
#include "RuntimeGeneration.h"
#include "TaskManager.h"
#include "SpatialAwarenessFNV.h"
#include "WorldContextFNV.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace NearbyActorsFNV {
namespace {

struct NearbyActor {
    ActorPositionResolverFNV::PositionResult position;
    SpatialAwarenessFNV::Result spatial;
    bool manuallyActivated = false;
    bool autoManaged = false;
    bool eligible = false;
    bool autoEligible = false;
    std::string eligibilityReason;
    std::string status;
};

static std::chrono::steady_clock::time_point g_lastSendTime;
static std::chrono::steady_clock::time_point g_lastCheckTime;
static std::chrono::steady_clock::time_point g_lastEmptySelectionLogTime;
static std::string g_lastSentSignature;
static std::mutex g_sendMutex;
static constexpr auto kChangeCheckInterval = std::chrono::milliseconds(500);

std::string Trim(const std::string& value) {
    const char* whitespace = " \t\r\n";
    const size_t start = value.find_first_not_of(whitespace);
    if (start == std::string::npos) {
        return "";
    }
    const size_t end = value.find_last_not_of(whitespace);
    return value.substr(start, end - start + 1);
}

bool IsUsableActorName(const std::string& name) {
    const std::string trimmed = Trim(name);
    return !trimmed.empty() &&
        trimmed != "<no name>" &&
        trimmed != "Player" &&
        trimmed != "Courier" &&
        trimmed != "Prisoner";
}

std::string FormatFormId(uint32_t formId) {
    if (formId == 0) {
        return "";
    }

    std::ostringstream stream;
    stream << "0x" << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << formId;
    return stream.str();
}

ActorEligibilityFNV::Metadata EligibilityMetadataFromPosition(
    const ActorPositionResolverFNV::PositionResult& position) {
    ActorEligibilityFNV::Metadata metadata;
    metadata.name = position.actorName;
    metadata.race = position.race;
    metadata.voiceId = position.voiceId;
    metadata.voiceName = position.voiceName;
    metadata.baseId = position.baseId;
    metadata.baseType = position.baseType;
    metadata.baseTypeKnown = position.baseTypeKnown;
    metadata.isCreature = position.baseTypeKnown && position.baseType == 0x2B;
    metadata.isCreatureKnown = position.baseTypeKnown;
    return metadata;
}

std::string StatusFromSpatial(const SpatialAwarenessFNV::Result& spatial, float maxDistance) {
    if (spatial.canCommunicate) {
        if (spatial.volume > 0.0f && spatial.volume < 0.45f) {
            return "can hear you, muffled";
        }
        return "can hear you";
    }

    if (spatial.airDistance > maxDistance) {
        return "far away";
    }

    if (spatial.reason == "closed_door_between" || spatial.reason == "line_of_sight_blocked_muffled") {
        return "can't hear you clearly";
    }

    if (!spatial.reason.empty() && spatial.reason != "unknown") {
        return spatial.reason;
    }

    return "can't hear you clearly";
}

std::vector<NearbyActor> BuildNearbyActors() {
    std::vector<NearbyActor> result;
    int skippedUnresolved = 0;
    int skippedUnnamed = 0;
    int skippedScene = 0;
    int skippedIneligible = 0;
    int skippedDistance = 0;

    const auto player = ActorPositionResolverFNV::ResolvePlayer();
    if (!player.resolved) {
        Logger::LogDebug("NearbyActorsFNV: player position unresolved (%s)", player.reason.c_str());
        return result;
    }

    std::vector<ActorPositionResolverFNV::PositionResult> positions =
        ActorPositionResolverFNV::GetRecentActorPositions();

    const float maxDistance = Config::nearbyActorsMaxDistance > 0.0f
        ? Config::nearbyActorsMaxDistance
        : 2400.0f;

    for (const auto& position : positions) {
        if (!position.resolved || position.formId == 0 || position.formId == 0x00000014) {
            ++skippedUnresolved;
            continue;
        }
        if (!IsUsableActorName(position.actorName)) {
            ++skippedUnnamed;
            continue;
        }
        if (!ActorPositionResolverFNV::IsPositionInPlayerScene(position)) {
            ++skippedScene;
            continue;
        }

        NearbyActor actor;
        actor.position = position;
        actor.spatial = SpatialAwarenessFNV::Evaluate(player, position);
        actor.manuallyActivated = AgentManager::IsManuallyActivated(position.formId);
        actor.autoManaged = AgentManager::IsAutoManaged(position.formId);
        actor.eligible = ActorEligibilityFNV::IsRechatAllowed(
            EligibilityMetadataFromPosition(position),
            actor.manuallyActivated,
            &actor.eligibilityReason);
        actor.autoEligible = ActorEligibilityFNV::IsAutoActivationAllowed(
            EligibilityMetadataFromPosition(position), nullptr);

        if (position.disabledKnown && position.isDisabled) {
            actor.eligible = false;
            actor.eligibilityReason = "actor is disabled";
        }
        if (position.deadKnown && position.isDead) {
            actor.eligible = false;
            actor.eligibilityReason = "actor is dead";
        }

        if (!actor.eligible) {
            ++skippedIneligible;
            continue;
        }

        if (!actor.spatial.canCommunicate &&
            actor.spatial.airDistance > maxDistance &&
            !actor.manuallyActivated &&
            !actor.autoManaged) {
            ++skippedDistance;
            continue;
        }

        actor.status = StatusFromSpatial(actor.spatial, maxDistance);
        result.push_back(actor);
    }

    std::sort(result.begin(), result.end(), [](const NearbyActor& a, const NearbyActor& b) {
        return a.spatial.airDistance < b.spatial.airDistance;
    });

    if (Config::nearbyActorsMaxActors > 0 &&
        result.size() > static_cast<size_t>(Config::nearbyActorsMaxActors)) {
        result.resize(static_cast<size_t>(Config::nearbyActorsMaxActors));
    }

    const auto now = std::chrono::steady_clock::now();
    if (result.empty() && !positions.empty() &&
        (g_lastEmptySelectionLogTime.time_since_epoch().count() == 0 ||
         now - g_lastEmptySelectionLogTime >= std::chrono::seconds(5))) {
        Logger::LogInfo("NearbyActorsFNV: no actors selected scanned=%zu unresolved=%d unnamed=%d scene=%d ineligible=%d distance=%d playerResolved=%d",
            positions.size(),
            skippedUnresolved,
            skippedUnnamed,
            skippedScene,
            skippedIneligible,
            skippedDistance,
            player.resolved ? 1 : 0);
        g_lastEmptySelectionLogTime = now;
    }

    return result;
}

std::vector<ActorPositionResolverFNV::PositionResult> BuildPartyMembers() {
    std::vector<ActorPositionResolverFNV::PositionResult> result;
    std::vector<ActorPositionResolverFNV::PositionResult> positions =
        ActorPositionResolverFNV::GetRecentActorPositions();

    for (const auto& position : positions) {
        if (!position.resolved ||
            position.formId == 0 ||
            position.formId == 0x00000014 ||
            (position.deadKnown && position.isDead) ||
            (position.disabledKnown && position.isDisabled) ||
            !position.playerTeammateKnown ||
            !position.isPlayerTeammate ||
            !IsUsableActorName(position.actorName)) {
            continue;
        }
        if (!ActorPositionResolverFNV::IsPositionInPlayerScene(position)) {
            continue;
        }
        result.push_back(position);
    }

    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return a.actorName < b.actorName;
    });

    return result;
}

std::string BuildSignature(const std::vector<NearbyActor>& actors) {
    std::ostringstream signature;
    signature << "generation=" << RuntimeGeneration::Current() << ";";
    for (const auto& actor : actors) {
        signature << actor.position.formId << ":"
                  << actor.status << ":"
                  << static_cast<int>(std::floor(actor.spatial.airDistance / 50.0f)) << "|";
    }
    signature << "party:";
    for (const auto& member : BuildPartyMembers()) {
        signature << member.formId << "|";
    }
    return signature.str();
}

void AppendBool(std::ostringstream& json, const char* key, bool value) {
    json << "\"" << key << "\":" << (value ? "true" : "false");
}

std::string BuildJson(const std::vector<NearbyActor>& actors) {
    const long long localTs = Misc::GetCurrentTimeMillis() / 1000;
    const long long gameTs = WorldContextFNV::GetGameTimestamp();
    std::string playerName = Trim(Misc::GetPlayerName());
    if (playerName.empty()) {
        playerName = Trim(Config::playerName);
    }
    if (playerName.empty()) {
        playerName = "Player";
    }

    std::ostringstream json;
    json << "{";
    json << "\"schema\":\"dialectic.nearby_actors.v1\",";
    json << "\"type\":\"nearby_actors\",";
    json << "\"game\":\"fnv\",";
    json << "\"ts\":" << localTs << ",";
    json << "\"gamets\":" << (gameTs > 0 ? gameTs : 0) << ",";
    json << "\"runtime_generation\":" << RuntimeGeneration::Current() << ",";
    json << "\"player\":\"" << HTTPManager::EscapeJson(playerName) << "\",";
    json << "\"actors\":[";

    bool first = true;
    for (const auto& actor : actors) {
        const auto& position = actor.position;
        if (!first) {
            json << ",";
        }
        first = false;

        json << "{";
        json << "\"name\":\"" << HTTPManager::EscapeJson(position.actorName) << "\",";
        json << "\"refid\":\"" << HTTPManager::EscapeJson(FormatFormId(position.formId)) << "\",";
        json << "\"baseid\":\"" << HTTPManager::EscapeJson(position.baseId) << "\",";
        json << "\"race\":\"" << HTTPManager::EscapeJson(position.race) << "\",";
        json << "\"gender\":\"" << HTTPManager::EscapeJson(position.gender) << "\",";
        json << "\"voiceid\":\"" << HTTPManager::EscapeJson(position.voiceId) << "\",";
        json << "\"voice_formid\":\"" << HTTPManager::EscapeJson(position.voiceFormId) << "\",";
        json << "\"voice_name\":\"" << HTTPManager::EscapeJson(position.voiceName) << "\",";
        json << "\"cell_formid\":\"" << HTTPManager::EscapeJson(FormatFormId(position.cellFormId)) << "\",";
        json << "\"worldspace_formid\":\"" << HTTPManager::EscapeJson(FormatFormId(position.worldspaceFormId)) << "\",";
        json << "\"x\":" << position.position.x << ",";
        json << "\"y\":" << position.position.y << ",";
        json << "\"z\":" << position.position.z << ",";
        json << "\"yaw\":" << position.yaw << ",";
        json << "\"distance\":" << actor.spatial.airDistance << ",";
        json << "\"volume\":" << actor.spatial.volume << ",";
        json << "\"status\":\"" << HTTPManager::EscapeJson(actor.status) << "\",";
        AppendBool(json, "can_hear_player", actor.spatial.canCommunicate);
        json << ",";
        AppendBool(json, "eligible", actor.eligible);
        json << ",";
        AppendBool(json, "auto_eligible", actor.autoEligible);
        json << ",";
        AppendBool(json, "manual", actor.manuallyActivated);
        json << ",";
        AppendBool(json, "auto_managed", actor.autoManaged);
        json << ",";
        AppendBool(json, "is_player_teammate", position.playerTeammateKnown && position.isPlayerTeammate);
        json << ",";
        AppendBool(json, "is_interior", position.interiorKnown && position.isInterior);
        json << ",";
        AppendBool(json, "los_to_player", position.actorHasLosToPlayerKnown && position.actorHasLosToPlayer);
        json << ",";
        AppendBool(json, "player_los_to_actor", position.playerHasLosToActorKnown && position.playerHasLosToActor);
        json << ",";
        AppendBool(json, "disabled_known", position.disabledKnown);
        json << ",";
        AppendBool(json, "disabled", position.disabledKnown && position.isDisabled);
        json << ",";
        AppendBool(json, "dead_known", position.deadKnown);
        json << ",";
        AppendBool(json, "dead", position.deadKnown && position.isDead);
        json << ",";
        json << "\"spatial_reason\":\"" << HTTPManager::EscapeJson(actor.spatial.reason) << "\",";
        json << "\"base_type\":" << position.baseType;
        json << "}";
    }

    json << "],";
    json << "\"party_members\":[";

    bool firstPartyMember = true;
    for (const auto& position : BuildPartyMembers()) {
        if (!firstPartyMember) {
            json << ",";
        }
        firstPartyMember = false;

        json << "{";
        json << "\"name\":\"" << HTTPManager::EscapeJson(position.actorName) << "\",";
        json << "\"refid\":\"" << HTTPManager::EscapeJson(FormatFormId(position.formId)) << "\",";
        json << "\"baseid\":\"" << HTTPManager::EscapeJson(position.baseId) << "\",";
        json << "\"race\":\"" << HTTPManager::EscapeJson(position.race) << "\",";
        json << "\"gender\":\"" << HTTPManager::EscapeJson(position.gender) << "\",";
        json << "\"voiceid\":\"" << HTTPManager::EscapeJson(position.voiceId) << "\",";
        json << "\"voice_formid\":\"" << HTTPManager::EscapeJson(position.voiceFormId) << "\",";
        json << "\"voice_name\":\"" << HTTPManager::EscapeJson(position.voiceName) << "\",";
        json << "\"cell_formid\":\"" << HTTPManager::EscapeJson(FormatFormId(position.cellFormId)) << "\",";
        json << "\"level\":" << position.level << ",";
        AppendBool(json, "is_player_teammate", true);
        json << "}";
    }

    json << "]";
    json << "}";
    return json.str();
}

void SendActors(std::vector<NearbyActor> actors) {
    const std::string json = BuildJson(actors);
    const std::uint64_t runtimeGeneration = RuntimeGeneration::Current();
    TaskManager::Enqueue("gamedata", "nearby_actors", runtimeGeneration, false,
        std::chrono::seconds(30), [json, count = actors.size(), runtimeGeneration](const TaskManager::CancellationToken& token) {
        if (token.IsCancellationRequested()) return;
        const auto start = std::chrono::steady_clock::now();
        const std::string response = HTTPManager::SendJson("gamedata.php", json);
        const long long elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        Logger::LogInfo("[PERF] NearbyActorsFNV send elapsed_ms=%lld actors=%zu bytes=%zu response_empty=%d",
            elapsedMs,
            count,
            json.size(),
            response.empty() ? 1 : 0);
        if (response.empty()) {
            Logger::LogWarning("NearbyActorsFNV: nearby_actors update for %zu actors returned empty response", count);
        } else {
            AutoGreetingFNV::HandleServerResponse(response, runtimeGeneration);
        }
    });
}

} // namespace

void SendNow(bool force) {
    if (!Config::nearbyActorsEnabled) {
        return;
    }

    const std::vector<NearbyActor> actors = BuildNearbyActors();
    const std::string signature = BuildSignature(actors);
    std::lock_guard<std::mutex> lock(g_sendMutex);
    const bool changed = signature != g_lastSentSignature;
    if (!force && Config::nearbyActorsSendOnChange && !changed) {
        return;
    }

    g_lastSendTime = std::chrono::steady_clock::now();
    g_lastSentSignature = signature;
    SendActors(actors);
}

void Update() {
    if (!Config::nearbyActorsEnabled) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const float configuredSeconds = Config::nearbyActorsUpdateSeconds > 0.5f
        ? Config::nearbyActorsUpdateSeconds
        : 5.0f;
    const auto heartbeat = std::chrono::milliseconds(static_cast<int>(configuredSeconds * 1000.0f));

    bool due = false;
    {
        std::lock_guard<std::mutex> lock(g_sendMutex);
        due = g_lastSendTime.time_since_epoch().count() == 0 ||
            now - g_lastSendTime >= heartbeat;
    }

    if (due) {
        SendNow(false);
    }
}

} // namespace NearbyActorsFNV
