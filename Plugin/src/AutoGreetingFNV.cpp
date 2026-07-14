#include "AutoGreetingFNV.h"

#include "ActorPositionResolverFNV.h"
#include "GameLoop.h"
#include "HTTPManager.h"
#include "Logger.h"
#include "RuntimeGeneration.h"
#include "RuntimeSnapshot.h"
#include "SpatialAwarenessFNV.h"
#include "SpeakManager.h"

#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <utility>

namespace AutoGreetingFNV {
namespace {

struct PendingGreeting {
    bool active{false};
    std::string npc;
    std::string player;
    std::uint32_t npcFormId{0};
    std::uint64_t runtimeGeneration{0};
    std::chrono::steady_clock::time_point receivedAt{};
};

std::mutex g_mutex;
PendingGreeting g_pending;
constexpr auto kStabilizeDelay = std::chrono::seconds(2);
constexpr auto kExpiry = std::chrono::seconds(12);

std::string ExtractJsonString(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    const size_t keyPos = json.find(needle);
    if (keyPos == std::string::npos) return "";
    const size_t colonPos = json.find(':', keyPos + needle.size());
    if (colonPos == std::string::npos) return "";
    const size_t quotePos = json.find('"', colonPos + 1);
    if (quotePos == std::string::npos) return "";

    std::string value;
    bool escaped = false;
    for (size_t i = quotePos + 1; i < json.size(); ++i) {
        const char ch = json[i];
        if (escaped) {
            switch (ch) {
                case 'n': value.push_back('\n'); break;
                case 'r': value.push_back('\r'); break;
                case 't': value.push_back('\t'); break;
                default: value.push_back(ch); break;
            }
            escaped = false;
        } else if (ch == '\\') {
            escaped = true;
        } else if (ch == '"') {
            return value;
        } else {
            value.push_back(ch);
        }
    }
    return "";
}

std::uint32_t ParseFormId(const std::string& value) {
    if (value.empty()) return 0;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value.c_str(), &end, 0);
    return end != value.c_str() ? static_cast<std::uint32_t>(parsed) : 0;
}

std::string FormatFormId(std::uint32_t formId) {
    std::ostringstream stream;
    stream << "0x" << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << formId;
    return stream.str();
}

void DropLocked(const char* reason) {
    if (!g_pending.active) return;
    Logger::LogInfo("AutoGreetingFNV: dropped npc='%s' reason=%s",
        g_pending.npc.c_str(), reason ? reason : "cancelled");
    g_pending = PendingGreeting{};
}

} // namespace

void HandleServerResponse(const std::string& response, std::uint64_t runtimeGeneration) {
    if (response.find("\"auto_greeting\"") == std::string::npos ||
        response.find("\"schema\":\"dialectic.auto_greeting.v1\"") == std::string::npos) {
        return;
    }

    PendingGreeting pending;
    pending.npc = ExtractJsonString(response, "npc");
    pending.player = ExtractJsonString(response, "player");
    pending.npcFormId = ParseFormId(ExtractJsonString(response, "npc_refid"));
    pending.runtimeGeneration = runtimeGeneration;
    pending.receivedAt = std::chrono::steady_clock::now();
    pending.active = !pending.npc.empty() && pending.npcFormId != 0;
    if (!pending.active) {
        Logger::LogWarning("AutoGreetingFNV: ignored malformed server directive");
        return;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    g_pending = std::move(pending);
    Logger::LogInfo("AutoGreetingFNV: scheduled npc='%s' form=0x%08X generation=%llu",
        g_pending.npc.c_str(), g_pending.npcFormId,
        static_cast<unsigned long long>(g_pending.runtimeGeneration));
}

void Update() {
    PendingGreeting pending;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_pending.active) return;
        pending = g_pending;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now - pending.receivedAt < kStabilizeDelay) return;

    const auto cancel = [](const char* reason) {
        std::lock_guard<std::mutex> lock(g_mutex);
        DropLocked(reason);
    };
    if (now - pending.receivedAt > kExpiry) {
        cancel("expired");
        return;
    }
    if (!RuntimeGeneration::IsCurrent(pending.runtimeGeneration)) {
        cancel("runtime_generation_changed");
        return;
    }

    const RuntimeSnapshot::GameState game = RuntimeSnapshot::GetGameState();
    if (!game.valid || !game.inGame || game.loadingMenuOpen) {
        cancel("game_not_ready");
        return;
    }
    if (game.inMenu || game.paused || game.pipboyOpen || game.dialogueMenuOpen) {
        cancel("menu_open");
        return;
    }
    if (game.inCombat) {
        cancel("player_in_combat");
        return;
    }
    if (GameLoop::IsConversationActive() || SpeakManager::IsSpeaking() ||
        HTTPManager::IsStreamInProgress() || HTTPManager::HasPendingResponse()) {
        cancel("dialogue_or_input_active");
        return;
    }

    const auto actor = ActorPositionResolverFNV::ResolveActor(pending.npcFormId);
    RuntimeSnapshot::ActorState actorState;
    if (!actor.resolved || !ActorPositionResolverFNV::IsPositionInPlayerScene(actor) ||
        !ActorPositionResolverFNV::IsActorPositionFresh(pending.npcFormId, 3000) ||
        (actor.deadKnown && actor.isDead) || (actor.disabledKnown && actor.isDisabled) ||
        (actor.sceneBusyKnown && actor.sceneBusy) ||
        (RuntimeSnapshot::TryGetActor(pending.npcFormId, actorState) &&
         (actorState.inCombat || actorState.hostileToPlayer || actorState.dead))) {
        cancel("actor_not_available");
        return;
    }

    const auto playerPosition = ActorPositionResolverFNV::ResolvePlayer();
    const auto spatial = SpatialAwarenessFNV::Evaluate(playerPosition, actor);
    if (!playerPosition.resolved || !spatial.canCommunicate) {
        cancel("actor_cannot_hear_player");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_pending.active || g_pending.runtimeGeneration != pending.runtimeGeneration ||
            g_pending.npcFormId != pending.npcFormId) {
            return;
        }
        g_pending = PendingGreeting{};
    }

    const std::string player = pending.player.empty() ? "Player" : pending.player;
    const std::string refId = FormatFormId(pending.npcFormId);
    std::ostringstream payload;
    payload << "{"
            << "\"schema\":\"dialectic.auto_greeting.trigger.v1\","
            << "\"npc\":\"" << HTTPManager::EscapeJson(pending.npc) << "\","
            << "\"npc_id\":\"" << refId << "\","
            << "\"player\":\"" << HTTPManager::EscapeJson(player) << "\","
            << "\"target\":{\"name\":\"" << HTTPManager::EscapeJson(pending.npc)
            << "\",\"refid\":\"" << refId << "\"},"
            << "\"reason\":\"return_after_absence\","
            << "\"audience_snapshot\":{\"people\":\"|" << HTTPManager::EscapeJson(pending.npc)
            << "|" << HTTPManager::EscapeJson(player) << "|\",\"target_only\":true,"
            << "\"target_form_id\":\"" << refId << "\"},"
            << "\"game\":\"fnv\"}";

    Logger::LogInfo("AutoGreetingFNV: dispatching npc='%s' form=0x%08X",
        pending.npc.c_str(), pending.npcFormId);
    HTTPManager::SendEvent("auto_greeting", payload.str());
}

void Cancel(const char* reason) {
    std::lock_guard<std::mutex> lock(g_mutex);
    DropLocked(reason);
}

} // namespace AutoGreetingFNV
