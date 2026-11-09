#include "ExternalCommandBridge.h"

#include "ActionManager.h"
#include "ActivationManager.h"
#include "AgentManager.h"
#include "ExternalCommandRegistry.h"
#include "GameLoop.h"
#include "GameThreadDispatcher.h"
#include "HTTPManager.h"
#include "Interaction.h"
#include "Logger.h"
#include "MultiplayerSharing.h"
#include "NPCDetector.h"
#include "PlayerInventoryManagerFNV.h"
#include "RuntimeGeneration.h"
#include "RuntimeSnapshot.h"
#include "WorldContextFNV.h"
#include "XNVSEAdapter.h"

#include <chrono>
#include <string>
#include <utility>

namespace ExternalCommandBridge {
namespace {

using ExternalCommandRegistry::PendingRequest;

constexpr const char* kDispatchType = "external_command";

ExternalCommandRegistry::Registry g_registry;
ExternalCommandRegistry::ActorControlTable g_actorControl;
ExternalCommandRegistry::RefreshThrottle g_actorRefresh;
ExternalCommandRegistry::RefreshThrottle g_playerRefresh;
constexpr int kRefreshInventory = 1;
constexpr int kRefreshWorld = 2;
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
    if (g_registry.Cancel(requestId, request, ExternalCommandRegistry::Outcome::Failed)) {
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
    const bool dispatched = XNVSEAdapter::DispatchExternalCommandEvent(request.actorFormId, request.handle,
        request.bridge, request.command, request.parameter, request.requestId, scriptHandlers);
    Logger::LogInfo("[EXTERNAL_COMMAND] dispatched id=%u bridge=%s handle=%u command=%s actor=0x%08X "
        "handlers=%zu ok=%d", request.requestId, request.bridge.c_str(), request.handle,
        request.command.c_str(), request.actorFormId, scriptHandlers, dispatched ? 1 : 0);
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

int RegisterOwnedBridge(const std::string& name, const std::string& owner) {
    std::uint32_t handle = 0;
    const auto result = g_registry.RegisterOwnedBridge(Trim(name), Trim(owner), handle);
    switch (result) {
        case ExternalCommandRegistry::RegisterResult::Registered:
        case ExternalCommandRegistry::RegisterResult::AlreadyOwned:
            Logger::LogInfo("[EXTERNAL_COMMAND] register owned bridge=%s owner=%s handle=%u result=%s",
                name.c_str(), owner.c_str(), handle, ExternalCommandRegistry::ToString(result));
            return static_cast<int>(handle);
        default:
            break;
    }
    Logger::LogWarning("[EXTERNAL_COMMAND] register owned bridge=%s owner=%s result=%s",
        name.c_str(), owner.c_str(), ExternalCommandRegistry::ToString(result));
    switch (result) {
        case ExternalCommandRegistry::RegisterResult::InvalidName: return -1;
        case ExternalCommandRegistry::RegisterResult::InvalidOwner: return -2;
        case ExternalCommandRegistry::RegisterResult::OwnedByOther: return -3;
        default: return -4;
    }
}

namespace {

int FinishCompletion(ExternalCommandRegistry::CompleteResult completion,
                     const PendingRequest& request,
                     int requestId,
                     const std::string& bridge,
                     std::uint32_t actorFormId,
                     bool succeeded,
                     const std::string& result) {
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

void CancelStale() {
    // Save/load cancellation is local: requests from an older runtime generation never reach the server.
    for (const PendingRequest& stale : g_registry.TakeStale(RuntimeGeneration::Current())) {
        Logger::LogInfo("[EXTERNAL_COMMAND] cancelled id=%u command=%s reason=runtime_generation_changed",
            stale.requestId, stale.command.c_str());
    }
}

} // namespace

int CompleteRequest(std::uint32_t actorFormId,
                    const std::string& bridge,
                    int requestId,
                    bool succeeded,
                    const std::string& result) {
    if (requestId <= 0) return 0;
    CancelStale();
    PendingRequest request;
    const auto completion = g_registry.Complete(Trim(bridge), static_cast<std::uint32_t>(requestId),
        actorFormId, request, succeeded);
    return FinishCompletion(completion, request, requestId, bridge, actorFormId, succeeded, result);
}

int CompleteOwnedRequest(std::uint32_t actorFormId,
                         int handle,
                         int requestId,
                         bool succeeded,
                         const std::string& result) {
    if (requestId <= 0 || handle <= 0) return 0;
    CancelStale();
    PendingRequest request;
    const auto completion = g_registry.CompleteOwned(static_cast<std::uint32_t>(handle),
        static_cast<std::uint32_t>(requestId), actorFormId, succeeded, request);
    return FinishCompletion(completion, request, requestId, "handle " + std::to_string(handle), actorFormId,
        succeeded, result);
}

int IsRequestPending(int requestId) {
    return requestId > 0 && g_registry.IsPending(static_cast<std::uint32_t>(requestId)) ? 1 : 0;
}

int GetRequestStatus(int handle, int requestId) {
    if (handle < 0 || requestId <= 0) return 0;
    CancelStale();
    return static_cast<int>(g_registry.Status(static_cast<std::uint32_t>(handle),
        static_cast<std::uint32_t>(requestId)));
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

int SetActorFlag(std::uint32_t actorFormId, int handle, std::uint32_t flag, int active) {
    const char* flagName = flag == ExternalCommandRegistry::kTalkLock ? "talk_lock" : "animation_busy";
    int code = 1;
    if (active != 0 && active != 1) {
        code = -4;
    } else if (handle <= 0 || !g_registry.IsOwnedHandle(static_cast<std::uint32_t>(handle))) {
        code = -1;
    } else {
        switch (g_actorControl.Set(static_cast<std::uint32_t>(handle), actorFormId,
            static_cast<ExternalCommandRegistry::ActorFlag>(flag), active != 0)) {
            case ExternalCommandRegistry::ControlResult::Applied: code = 1; break;
            case ExternalCommandRegistry::ControlResult::StillHeldByOther: code = 2; break;
            case ExternalCommandRegistry::ControlResult::InvalidHandle: code = -1; break;
            case ExternalCommandRegistry::ControlResult::InvalidActor: code = -2; break;
            case ExternalCommandRegistry::ControlResult::Full: code = -3; break;
        }
    }
    if (code > 0) {
        Logger::LogInfo("[ADDON_CONTROL] %s=%d actor=0x%08X handle=%d result=%d",
            flagName, active, actorFormId, handle, code);
    } else {
        Logger::LogWarning("[ADDON_CONTROL] %s=%d rejected actor=0x%08X handle=%d result=%d",
            flagName, active, actorFormId, handle, code);
    }
    return code;
}

int GetActorFlags(std::uint32_t actorFormId, int handle) {
    return handle < 0 ? 0 : static_cast<int>(g_actorControl.Flags(actorFormId, static_cast<std::uint32_t>(handle)));
}

bool IsActorTalkBlocked(std::uint32_t actorFormId) {
    return g_actorControl.Flags(actorFormId) != 0;
}

bool IsActorAnimationBusy(std::uint32_t actorFormId) {
    return (g_actorControl.Flags(actorFormId) & ExternalCommandRegistry::kAnimationBusy) != 0;
}

const char* ActorBlockReason(std::uint32_t actorFormId) {
    const std::uint32_t flags = g_actorControl.Flags(actorFormId);
    if ((flags & ExternalCommandRegistry::kAnimationBusy) != 0) return "animation_busy";
    return (flags & ExternalCommandRegistry::kTalkLock) != 0 ? "talk_locked" : nullptr;
}

bool IsOwnedHandle(int handle) {
    return handle > 0 && g_registry.IsOwnedHandle(static_cast<std::uint32_t>(handle));
}

namespace {

// Loaded, living, non-excluded actors in the player's scene, from the existing native snapshot.
std::vector<ExternalCommandRegistry::AgentCandidate> SceneActorCandidates() {
    std::vector<ExternalCommandRegistry::AgentCandidate> candidates;
    RuntimeSnapshot::GameState gameState;
    if (MultiplayerSharing::IsListener() ||
        !RuntimeSnapshot::TryGetFreshGameState(gameState, std::chrono::milliseconds(500)) ||
        !gameState.inGame || gameState.loadingMenuOpen) {
        return candidates;
    }
    for (const RuntimeSnapshot::ActorState& actor : RuntimeSnapshot::GetActors()) {
        if (actor.formId == gameState.playerFormId || actor.name.empty() ||
            !RuntimeSnapshot::IsActorInScene(actor, gameState) ||
            NPCDetector::IsExcluded(actor.formId, actor.name)) {
            continue;
        }
        candidates.push_back({ actor.formId, actor.name, actor.distanceToPlayer,
            AgentManager::IsAIAgent(actor.formId) });
    }
    return candidates;
}

void LogAgentOperation(const char* operation, std::uint32_t actorFormId, int handle, int code) {
    if (code > 0) {
        Logger::LogInfo("[ADDON_AGENT] %s actor=0x%08X handle=%d result=%d", operation, actorFormId, handle, code);
    } else {
        Logger::LogWarning("[ADDON_AGENT] %s rejected actor=0x%08X handle=%d result=%d",
            operation, actorFormId, handle, code);
    }
}

} // namespace

std::vector<std::uint32_t> QueryActors(int filter, int limit, float maxDistance) {
    if (!ExternalCommandRegistry::IsValidAgentFilter(filter) || limit <= 0) return {};
    return ExternalCommandRegistry::SelectAgentCandidates(SceneActorCandidates(),
        static_cast<ExternalCommandRegistry::AgentFilter>(filter), limit, maxDistance);
}

std::uint32_t FindAgentByName(const std::string& name) {
    const std::string trimmed = Trim(name);
    if (trimmed.empty() || trimmed.size() > ExternalCommandRegistry::kMaxTextLength) return 0;
    bool ambiguous = false;
    const std::uint32_t formId = ExternalCommandRegistry::FindUniqueAgentByName(
        SceneActorCandidates(), trimmed, ambiguous);
    if (ambiguous) {
        Logger::LogWarning("[ADDON_AGENT] name lookup rejected name=%s reason=ambiguous", trimmed.c_str());
    }
    return formId;
}

int GetAgentState(std::uint32_t actorFormId) {
    if (actorFormId == 0 || !AgentManager::IsAIAgent(actorFormId)) return 0;
    return 1 | (AgentManager::IsManuallyActivated(actorFormId) ? 2 : 0) |
        (AgentManager::IsAutoManaged(actorFormId) ? 4 : 0);
}

int RegisterAgent(std::uint32_t actorFormId, int handle) {
    int code = 1;
    if (!IsOwnedHandle(handle)) {
        code = -1;
    } else if (MultiplayerSharing::IsListener()) {
        code = -4;
    } else if (!ActorInCurrentScene(actorFormId)) {
        code = -2;
    } else if (AgentManager::IsManuallyActivated(actorFormId)) {
        code = 2;
    } else if (!ActivationManager::ActivateActor(actorFormId, ActivationManager::ActivationSource::Manual)) {
        // The activation manager logs the policy reason (exclusion, eligibility, scene package).
        code = -3;
    }
    LogAgentOperation("register", actorFormId, handle, code);
    return code;
}

int UnregisterAgent(std::uint32_t actorFormId, int handle) {
    int code = 1;
    if (!IsOwnedHandle(handle)) {
        code = -1;
    } else if (actorFormId == 0) {
        code = -2;
    } else if (MultiplayerSharing::IsListener()) {
        code = -4;
    } else if (!ActivationManager::DeactivateActor(actorFormId)) {
        code = 0;
    }
    LogAgentOperation("unregister", actorFormId, handle, code);
    return code;
}

int RefreshActorContext(std::uint32_t actorFormId, int handle) {
    int code = 1;
    std::string actorName;
    if (!IsOwnedHandle(handle)) {
        code = -1;
    } else if (MultiplayerSharing::IsListener()) {
        code = -4;
    } else if (!ActorInCurrentScene(actorFormId, &actorName)) {
        code = -2;
    } else if (!AgentManager::IsAIAgent(actorFormId)) {
        code = -3;
    } else if (!g_actorRefresh.TryBegin(actorFormId, std::chrono::steady_clock::now())) {
        code = 2;
    } else if (!AgentManager::RefreshActorMetadataForPrompt(actorFormId, actorName)) {
        code = -2;
    }
    LogAgentOperation("refresh_actor_context", actorFormId, handle, code);
    return code;
}

int RefreshPlayerContext(int handle, int flags) {
    int code = 2;
    if (!IsOwnedHandle(handle)) {
        code = -1;
    } else if (flags <= 0 || (flags & ~(kRefreshInventory | kRefreshWorld)) != 0) {
        code = -2;
    } else if (MultiplayerSharing::IsListener()) {
        code = -4;
    } else {
        const auto now = std::chrono::steady_clock::now();
        if ((flags & kRefreshInventory) != 0 && g_playerRefresh.TryBegin(kRefreshInventory, now)) {
            PlayerInventoryManagerFNV::MarkDirty("addon_context_refresh", 200);
            code = 1;
        }
        if ((flags & kRefreshWorld) != 0 && g_playerRefresh.TryBegin(kRefreshWorld, now)) {
            // Uploads only when the location, weather, time or radio signature changed.
            WorldContextFNV::SendNow(false);
            code = 1;
        }
    }
    LogAgentOperation("refresh_player_context", 0, handle, code);
    return code;
}

int SendAddonMessage(std::uint32_t actorFormId, int handle, int mode, const std::string& text) {
    int code = -1;
    if (IsOwnedHandle(handle)) {
        code = MultiplayerSharing::IsListener() ? -4 : GameLoop::RequestAddonMessage(actorFormId, mode, text);
    }
    Logger::LogInfo("[ADDON_MESSAGE] message actor=0x%08X handle=%d mode=%d chars=%zu result=%d",
        actorFormId, handle, mode, text.size(), code);
    return code;
}

int RequestAddonReaction(std::uint32_t actorFormId, int handle, int eligibility, const std::string& text) {
    int code = -1;
    if (!IsOwnedHandle(handle)) {
        code = -1;
    } else if (eligibility != 0 && eligibility != 1) {
        code = -2;
    } else if (MultiplayerSharing::IsListener()) {
        code = -4;
    } else {
        code = GameLoop::RequestAddonReaction(actorFormId, eligibility == 1, text);
    }
    Logger::LogInfo("[ADDON_MESSAGE] reaction actor=0x%08X handle=%d eligibility=%d chars=%zu result=%d",
        actorFormId, handle, eligibility, text.size(), code);
    return code;
}

int SendAddonContext(std::uint32_t actorFormId, int handle, const std::string& type,
                     const std::string& name, const std::string& text) {
    const std::string bridge = handle > 0 ? g_registry.OwnedBridgeName(static_cast<std::uint32_t>(handle)) : "";
    const std::string cleanType = Trim(type);
    const std::string cleanName = Trim(name);
    std::string actorName;
    int code = 1;
    if (bridge.empty()) {
        code = -1;
    } else if (!ExternalCommandRegistry::IsValidEventName(cleanType) ||
               !ExternalCommandRegistry::IsValidEventName(cleanName) ||
               text.empty() || text.size() > ExternalCommandRegistry::kMaxTextLength) {
        code = -2;
    } else if (MultiplayerSharing::IsListener()) {
        code = -4;
    } else if (!Interaction::Allowed() || (actorFormId != 0 && !ActorInCurrentScene(actorFormId, &actorName)) ||
               !g_registry.AllowPluginEvent(bridge, std::chrono::steady_clock::now())) {
        code = -3;
    } else {
        HTTPManager::SendEvent("pluginevent", ExternalCommandRegistry::BuildAddonContextPayload(
            bridge, cleanType, cleanName, text, actorName, actorFormId));
    }
    Logger::LogInfo("[ADDON_MESSAGE] context bridge=%s type=%s name=%s actor=0x%08X chars=%zu result=%d",
        bridge.c_str(), cleanType.c_str(), cleanName.c_str(), actorFormId, text.size(), code);
    return code;
}

int SetInteractionEnabled(int handle, int enabled) {
    if (handle <= 0 || !g_registry.IsOwnedHandle(static_cast<std::uint32_t>(handle))) {
        Logger::LogWarning("[ADDON_CONTROL] interaction=%d rejected handle=%d reason=unknown_handle", enabled, handle);
        return -1;
    }
    if (enabled != 0 && enabled != 1) {
        Logger::LogWarning("[ADDON_CONTROL] interaction=%d rejected handle=%d reason=invalid_value", enabled, handle);
        return -2;
    }
    const int result = Interaction::Request(enabled == 1);
    Logger::LogInfo("[ADDON_CONTROL] interaction=%d handle=%d result=%s state=%d",
        enabled, handle, result == 1 ? "unchanged" : "accepted", Interaction::Status());
    return result;
}

void ClearActorFlags(const char* reason) {
    const std::size_t cleared = g_actorControl.Clear();
    g_actorRefresh.Clear();
    g_playerRefresh.Clear();
    if (cleared != 0) {
        Logger::LogInfo("[ADDON_CONTROL] cleared flags actors=%zu reason=%s", cleared, reason ? reason : "unknown");
    }
}

void Update() {
    const auto now = std::chrono::steady_clock::now();
    if (g_lastUpdate.time_since_epoch().count() != 0 && now - g_lastUpdate < std::chrono::milliseconds(250)) {
        return;
    }
    g_lastUpdate = now;
    CancelStale();
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
