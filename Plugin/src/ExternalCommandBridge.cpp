#include "ExternalCommandBridge.h"

#include "ActionManager.h"
#include "ExternalCommandRegistry.h"
#include "GameThreadDispatcher.h"
#include "HTTPManager.h"
#include "Interaction.h"
#include "Logger.h"
#include "MultiplayerSharing.h"
#include "RuntimeGeneration.h"
#include "RuntimeSnapshot.h"
#include "XNVSEAdapter.h"

#include <chrono>
#include <string>
#include <utility>

namespace ExternalCommandBridge {
namespace {

using ExternalCommandRegistry::PendingRequest;

constexpr const char* kDispatchType = "external_command";

ExternalCommandRegistry::Registry g_registry;
std::chrono::steady_clock::time_point g_lastUpdate;

std::string Trim(const std::string& value) {
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    const std::size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

void ReportFailure(const PendingRequest& request, const std::string& reason) {
    Logger::LogWarning("[EXTERNAL_COMMAND] failed id=%u command=%s actor=0x%08X reason=%s",
        request.requestId, request.command.c_str(), request.actorFormId, reason.c_str());
    ActionManager::SendExternalCommandResult(request.command, request.speaker, request.actorFormId,
        request.parameter, request.bridge, request.requestId, false,
        request.command + " failed because " + reason + ".");
}

void FailPending(std::uint32_t requestId, const char* reason) {
    PendingRequest request;
    if (g_registry.Cancel(requestId, request)) {
        ReportFailure(request, reason);
    }
}

bool ActorInCurrentScene(std::uint32_t actorFormId, std::string* actorName = nullptr) {
    RuntimeSnapshot::GameState gameState;
    RuntimeSnapshot::ActorState actor;
    if (!RuntimeSnapshot::TryGetFreshGameState(gameState, std::chrono::milliseconds(500)) ||
        !gameState.inGame || gameState.loadingMenuOpen ||
        actorFormId == 0 || actorFormId == gameState.playerFormId ||
        !RuntimeSnapshot::TryGetActor(actorFormId, actor) ||
        actor.deleted || actor.dead || !actor.loaded3D ||
        !RuntimeSnapshot::IsActorInScene(actor, gameState)) {
        return false;
    }
    if (actorName) *actorName = actor.name;
    return true;
}

void DispatchOnGameThread(std::uint32_t requestId) {
    // The request stays pending during dispatch so a synchronous handler can complete it.
    PendingRequest request;
    if (!g_registry.Get(requestId, request)) {
        return;
    }
    if (!RuntimeGeneration::IsCurrent(request.generation)) {
        g_registry.Cancel(requestId, request);
        Logger::LogInfo("[EXTERNAL_COMMAND] dropped stale id=%u command=%s", requestId, request.command.c_str());
        return;
    }
    if (!ActorInCurrentScene(request.actorFormId)) {
        FailPending(requestId, "speaker_not_in_current_scene");
        return;
    }

    std::size_t scriptHandlers = 0;
    const bool dispatched = XNVSEAdapter::DispatchExternalCommandEvent(request.actorFormId,
        request.bridge, request.command, request.parameter, request.requestId, scriptHandlers);
    Logger::LogInfo("[EXTERNAL_COMMAND] dispatched id=%u bridge=%s command=%s actor=0x%08X handlers=%zu ok=%d",
        request.requestId, request.bridge.c_str(), request.command.c_str(), request.actorFormId,
        scriptHandlers, dispatched ? 1 : 0);
    if (!dispatched) {
        FailPending(request.requestId, "dispatch_failed");
    } else if (scriptHandlers == 0) {
        FailPending(request.requestId, "bridge_handler_missing");
    }
}

} // namespace

bool Initialize() {
    return XNVSEAdapter::RegisterExternalCommandEvent();
}

bool HandleServerCommand(const std::string& command,
                         const std::string& parameter,
                         const std::string& speaker,
                         std::uint32_t speakerFormId,
                         std::uint64_t runtimeGeneration,
                         const char* source) {
    if (MultiplayerSharing::IsListener() || !Interaction::Allowed()) return false;
    if (!RuntimeGeneration::IsCurrent(runtimeGeneration)) {
        Logger::LogInfo("%s: dropped stale external command %s", source, command.c_str());
        return false;
    }

    const std::string cleanParameter = Trim(parameter);
    PendingRequest request;
    const auto begin = g_registry.Begin(command, cleanParameter, speaker, speakerFormId,
        runtimeGeneration, std::chrono::steady_clock::now(), request);
    if (begin == ExternalCommandRegistry::BeginResult::Duplicate) {
        Logger::LogInfo("%s: suppressed duplicate external command %s actor=0x%08X",
            source, command.c_str(), speakerFormId);
        return true;
    }
    if (begin != ExternalCommandRegistry::BeginResult::Accepted) {
        Logger::LogWarning("%s: rejected external command %s actor=0x%08X reason=%s",
            source, command.c_str(), speakerFormId, ExternalCommandRegistry::ToString(begin));
        std::string bridge;
        std::string action;
        ExternalCommandRegistry::ParseCommand(command, bridge, action);
        ActionManager::SendExternalCommandResult(command, speaker, speakerFormId, cleanParameter,
            bridge, 0, false,
            command + " failed because " + ExternalCommandRegistry::ToString(begin) + ".");
        return false;
    }

    const std::uint32_t requestId = request.requestId;
    const bool queued = GameThreadDispatcher::Enqueue(kDispatchType,
        "external_command:" + std::to_string(requestId), runtimeGeneration,
        [requestId]() { DispatchOnGameThread(requestId); },
        [requestId](const char* reason) {
            PendingRequest dropped;
            if (g_registry.Cancel(requestId, dropped)) {
                Logger::LogInfo("[EXTERNAL_COMMAND] dropped id=%u command=%s reason=%s",
                    requestId, dropped.command.c_str(), reason ? reason : "unknown");
            }
        });
    if (!queued) {
        FailPending(requestId, "dispatch_queue_full");
        return false;
    }
    Logger::LogInfo("%s: accepted external command id=%u %s actor=0x%08X",
        source, requestId, command.c_str(), speakerFormId);
    return true;
}

int RegisterBridge(const std::string& name, std::uint32_t scriptFormId) {
    const std::uint32_t owner = ExternalCommandRegistry::OwnerKeyFromScriptFormId(scriptFormId);
    const auto result = g_registry.RegisterBridge(Trim(name), owner);
    const bool accepted = result == ExternalCommandRegistry::RegisterResult::Registered ||
        result == ExternalCommandRegistry::RegisterResult::AlreadyOwned;
    if (accepted) {
        Logger::LogInfo("[EXTERNAL_COMMAND] register bridge=%s owner=0x%08X result=%s",
            name.c_str(), owner, ExternalCommandRegistry::ToString(result));
    } else {
        Logger::LogWarning("[EXTERNAL_COMMAND] register bridge=%s owner=0x%08X result=%s",
            name.c_str(), owner, ExternalCommandRegistry::ToString(result));
    }
    return accepted ? 1 : 0;
}

int CompleteRequest(std::uint32_t actorFormId,
                    const std::string& bridge,
                    int requestId,
                    bool succeeded,
                    const std::string& result) {
    if (requestId <= 0) return 0;
    PendingRequest request;
    const auto completion = g_registry.Complete(Trim(bridge), static_cast<std::uint32_t>(requestId),
        actorFormId, request);
    if (completion != ExternalCommandRegistry::CompleteResult::Completed) {
        Logger::LogWarning("[EXTERNAL_COMMAND] completion rejected id=%d bridge=%s actor=0x%08X reason=%s",
            requestId, bridge.c_str(), actorFormId, ExternalCommandRegistry::ToString(completion));
        return 0;
    }
    if (!RuntimeGeneration::IsCurrent(request.generation)) {
        Logger::LogInfo("[EXTERNAL_COMMAND] completion ignored for stale id=%d", requestId);
        return 0;
    }

    std::string text = Trim(result);
    if (text.size() > ExternalCommandRegistry::kMaxTextLength) {
        text.resize(ExternalCommandRegistry::kMaxTextLength);
    }
    if (text.empty()) {
        text = request.command + (succeeded ? " completed." : " failed.");
    }
    Logger::LogInfo("[EXTERNAL_COMMAND] completed id=%u command=%s actor=0x%08X status=%s",
        request.requestId, request.command.c_str(), actorFormId, succeeded ? "completed" : "failed");
    ActionManager::SendExternalCommandResult(request.command, request.speaker, request.actorFormId,
        request.parameter, request.bridge, request.requestId, succeeded, text);
    return 1;
}

int IsRequestPending(int requestId) {
    return requestId > 0 && g_registry.IsPending(static_cast<std::uint32_t>(requestId)) ? 1 : 0;
}

int SendPluginEvent(std::uint32_t actorFormId,
                    const std::string& bridge,
                    const std::string& name,
                    const std::string& data) {
    if (MultiplayerSharing::IsListener() || !Interaction::Allowed()) return 0;
    const std::string registered = g_registry.RegisteredBridgeName(Trim(bridge));
    const std::string eventName = Trim(name);
    std::string actorName;
    const bool actorValid = actorFormId == 0 || ActorInCurrentScene(actorFormId, &actorName);
    if (registered.empty() || !ExternalCommandRegistry::IsValidEventName(eventName) ||
        data.size() > ExternalCommandRegistry::kMaxTextLength || !actorValid ||
        !g_registry.AllowPluginEvent(registered, std::chrono::steady_clock::now())) {
        Logger::LogWarning("[EXTERNAL_COMMAND] plugin event rejected bridge=%s name=%s actor=0x%08X chars=%zu",
            bridge.c_str(), eventName.c_str(), actorFormId, data.size());
        return 0;
    }
    HTTPManager::SendEvent("pluginevent", ExternalCommandRegistry::BuildPluginEventPayload(
        registered, eventName, data, actorName, actorFormId));
    return 1;
}

void Update() {
    const auto now = std::chrono::steady_clock::now();
    if (g_lastUpdate.time_since_epoch().count() != 0 && now - g_lastUpdate < std::chrono::milliseconds(250)) {
        return;
    }
    g_lastUpdate = now;
    for (const PendingRequest& stale : g_registry.TakeStale(RuntimeGeneration::Current())) {
        Logger::LogInfo("[EXTERNAL_COMMAND] cancelled id=%u command=%s reason=runtime_generation_changed",
            stale.requestId, stale.command.c_str());
    }
    for (const PendingRequest& expired : g_registry.TakeExpired(now)) {
        ReportFailure(expired, "timed_out");
    }
}

void CancelAll(const char* reason) {
    GameThreadDispatcher::CancelByType(kDispatchType, reason ? reason : "external_command_cancel");
    for (const PendingRequest& cancelled : g_registry.TakeAll()) {
        Logger::LogInfo("[EXTERNAL_COMMAND] cancelled id=%u command=%s reason=%s",
            cancelled.requestId, cancelled.command.c_str(), reason ? reason : "cancel");
    }
}

} // namespace ExternalCommandBridge
