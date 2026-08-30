#include "ExternalEventAPI.h"

#include "ActionManager.h"
#include "GameLoop.h"
#include "GameThreadDispatcher.h"
#include "Logger.h"
#include "RuntimeGeneration.h"
#include "XNVSEAdapter.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <utility>

namespace ExternalEventAPI {
namespace {

std::atomic<bool> g_initialized{false};

const char* EventName(XNVSEAdapter::PublicDialecticEvent event) {
    switch (event) {
        case XNVSEAdapter::PublicDialecticEvent::SpeakExact: return "DialecticSpeakExact";
        case XNVSEAdapter::PublicDialecticEvent::Comment: return "DialecticComment";
        case XNVSEAdapter::PublicDialecticEvent::React: return "DialecticReact";
        case XNVSEAdapter::PublicDialecticEvent::Ask: return "DialecticAsk";
        case XNVSEAdapter::PublicDialecticEvent::OpenPrompt: return "DialecticOpenPrompt";
        case XNVSEAdapter::PublicDialecticEvent::Recruit: return "DialecticRecruit";
        case XNVSEAdapter::PublicDialecticEvent::Dismiss: return "DialecticDismiss";
        case XNVSEAdapter::PublicDialecticEvent::Wait: return "DialecticWait";
        case XNVSEAdapter::PublicDialecticEvent::Resume: return "DialecticResume";
    }
    return "DialecticUnknown";
}

bool ExecuteEvent(XNVSEAdapter::PublicDialecticEvent event,
                  std::uint32_t actorFormId,
                  const std::string& text) {
    switch (event) {
        case XNVSEAdapter::PublicDialecticEvent::SpeakExact:
            return GameLoop::RequestExternalExactSpeech(actorFormId, text);
        case XNVSEAdapter::PublicDialecticEvent::Comment:
            return GameLoop::RequestExternalComment(actorFormId);
        case XNVSEAdapter::PublicDialecticEvent::React:
            return GameLoop::RequestExternalReaction(actorFormId, text);
        case XNVSEAdapter::PublicDialecticEvent::Ask:
            return GameLoop::RequestExternalQuestion(actorFormId, text);
        case XNVSEAdapter::PublicDialecticEvent::OpenPrompt:
            return GameLoop::RequestTextInputMenuOpenForActor(actorFormId, "");
        case XNVSEAdapter::PublicDialecticEvent::Recruit:
            return ActionManager::RequestExternalFollowerAction(
                ActionManager::ExternalFollowerAction::Recruit, actorFormId, "");
        case XNVSEAdapter::PublicDialecticEvent::Dismiss:
            return ActionManager::RequestExternalFollowerAction(
                ActionManager::ExternalFollowerAction::Dismiss, actorFormId, "");
        case XNVSEAdapter::PublicDialecticEvent::Wait:
            return ActionManager::RequestExternalFollowerAction(
                ActionManager::ExternalFollowerAction::Wait, actorFormId, "");
        case XNVSEAdapter::PublicDialecticEvent::Resume:
            return ActionManager::RequestExternalFollowerAction(
                ActionManager::ExternalFollowerAction::Resume, actorFormId, "");
    }
    return false;
}

// Native event callbacks only copy arguments and defer all game/server work to the game thread.
void QueueEvent(XNVSEAdapter::PublicDialecticEvent event,
                std::uint32_t actorFormId,
                std::string text) {
    const char* eventName = EventName(event);
    if (!g_initialized.load(std::memory_order_acquire)) {
        Logger::LogInfo("[xNVSE event API] dropped %s because API is shutting down", eventName);
        return;
    }
    if (actorFormId == 0 || actorFormId == 0x00000014 || text.size() > 1000) {
        Logger::LogWarning("[xNVSE event API] rejected %s actor=0x%08X chars=%zu",
            eventName, actorFormId, text.size());
        return;
    }

    const std::uint64_t generation = RuntimeGeneration::Current();
    const std::string key = std::string(eventName) + ":" + std::to_string(actorFormId);
    const bool queued = GameThreadDispatcher::Enqueue("public_event", key, generation,
        [event, actorFormId, text = std::move(text), eventName = std::string(eventName)]() {
            const bool accepted = ExecuteEvent(event, actorFormId, text);
            Logger::LogInfo("[xNVSE event API] %s actor=0x%08X result=%s",
                eventName.c_str(), actorFormId, accepted ? "accepted" : "rejected");
        },
        [eventName = std::string(eventName), actorFormId](const char* reason) {
            Logger::LogWarning("[xNVSE event API] dropped %s actor=0x%08X reason=%s",
                eventName.c_str(), actorFormId, reason ? reason : "unknown");
        });
    if (!queued) {
        Logger::LogWarning("[xNVSE event API] queue full for %s actor=0x%08X",
            eventName, actorFormId);
    }
}

} // namespace

bool Initialize() {
    bool expected = false;
    if (!g_initialized.compare_exchange_strong(expected, true)) {
        return true;
    }

    const bool registered = XNVSEAdapter::RegisterPublicDialecticEvents(QueueEvent);
    if (!registered) {
        Logger::LogWarning("[xNVSE event API] one or more events unavailable; inspect collision/interface logs");
    }
    return registered;
}

void Shutdown() {
    if (!g_initialized.exchange(false)) {
        return;
    }
    XNVSEAdapter::UnregisterPublicDialecticEvents();
    GameThreadDispatcher::CancelByType("public_event", "public_event_api_shutdown");
}

} // namespace ExternalEventAPI
