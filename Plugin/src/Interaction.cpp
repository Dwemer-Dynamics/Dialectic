#include "Interaction.h"
#include "Config.h"
#include "HTTPManager.h"
#include "GameThreadDispatcher.h"
#include "IngameNotifier.h"
#include "MultiplayerSharing.h"
#include "ResponseQueueFNV.h"
#include "SpeakManager.h"
#include "TaskManager.h"
#include "VoiceRecorder.h"
#include "XNVSEAdapter.h"
#include "RuntimeGeneration.h"
#include <atomic>
#include <chrono>
#include <regex>

namespace Interaction {
namespace {
std::atomic<int> status{2}, desired{-1};
std::atomic<bool> busy{false};
std::atomic<uint64_t> epoch{1}, generation{0};
bool listener = false;
auto nextAttempt = std::chrono::steady_clock::time_point{};

// State replies are tiny; accept only the typed fields from the toggle endpoint.
bool Decode(const std::string& body, bool& enabled, uint64_t& version) {
    if (body.size() > 1024) return false;
    static const std::regex statePattern(R"("enabled"\s*:\s*(true|false))");
    static const std::regex versionPattern(R"("generation"\s*:\s*([0-9]+))");
    std::smatch stateMatch, versionMatch;
    if (!std::regex_search(body, stateMatch, statePattern) || !std::regex_search(body, versionMatch, versionPattern)) return false;
    try { version = std::stoull(versionMatch[1].str()); } catch (...) { return false; }
    enabled = stateMatch[1] == "true";
    return true;
}

void DiscardPending() {
    ++epoch;
    HTTPManager::DiscardInteractionResponses();
    ResponseQueueFNV::Clear("interaction_off");
    GameThreadDispatcher::CancelByType("action", "interaction_off");
    GameThreadDispatcher::CancelByType("narrator_action", "interaction_off");
    SpeakManager::DiscardPendingInteraction();
    VoiceRecorder::StopRecording();
    VoiceRecorder::StopOpenMicMonitoring();
}
}
int Status() { return status.load(); }
bool Allowed() { return Status() == 1; }
uint64_t Epoch() { return epoch.load(); }
uint64_t Generation() { return generation.load(); }
bool IsCurrent(uint64_t value) { return Allowed() && value == Epoch(); }
bool ManualInputAllowed() {
    if (Allowed()) return true;
    IngameNotifier::Notify("Dialectic is off.");
    return false;
}
bool IsTrigger(const std::string& type) {
    return type.starts_with("diary") || type.starts_with("player_menu_tts_")
        || type == "inputtext" || type == "inputtext_s" || type == "ginputtext"
        || type == "ginputtext_s" || type == "narrator_inputtext" || type == "bored"
        || type == "rechat" || type == "combatbark" || type == "instruction"
        || type == "suggestion" || type == "narration" || type == "narrator_welcome"
        || type == "just_say" || type == "cheatmode" || type == "vision" || type == "npc_tts_play";
}
void Toggle() {
    if (busy || Status() == 2) {
        XNVSEAdapter::UpdateInteractionMenuState(Status());
        return;
    }
    if (Status() != 3) desired = Allowed() ? 0 : 1;
    status = 2;
    DiscardPending();
    if (MultiplayerSharing::IsListener()) {
        const bool saved = Config::WriteCustomINIValue("Interaction", "ListenerEnabled", desired == 1 ? "1" : "0");
        status = saved ? desired.load() : 3;
    }
    nextAttempt = {};
    Update();
}
void Update() {
    static int lastStatus = 2;
    static int publishedStatus = -1;
    static uint64_t publishedRuntime = 0;
    const int current = Status();
    if ((publishedStatus != current || publishedRuntime != RuntimeGeneration::Current())
        && XNVSEAdapter::UpdateInteractionMenuState(current)) {
        publishedStatus = current;
        publishedRuntime = RuntimeGeneration::Current();
    }
    if (current != lastStatus) {
        lastStatus = current;
        if (current != 2) IngameNotifier::Notify(current == 1 ? "Dialectic is on." : current == 0 ? "Dialectic is off."
            : "Could not sync. Dialectic is off. Try again.");
    }
    const bool nowListener = MultiplayerSharing::IsListener();
    if (listener != nowListener) {
        listener = nowListener;
        status = 2;
        DiscardPending();
        desired = -1;
        nextAttempt = {};
    }
    if (nowListener) {
        if (Status() == 2) status = Config::ReadINIInt("Interaction", "ListenerEnabled", 1) ? 1 : 0;
        return;
    }
    if ((Status() != 2 && Status() != 3) || busy || std::chrono::steady_clock::now() < nextAttempt) return;
    busy = true;
    nextAttempt = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    const auto requestEpoch = Epoch();
    const int target = desired.load();
    TaskManager::Options options;
    options.type = "interaction_sync";
    options.lane = TaskManager::Lane::Background;
    options.timeout = std::chrono::seconds(15);
    auto task = TaskManager::Submit(options, [requestEpoch, target](const TaskManager::CancellationToken&) {
        bool enabled = false;
        uint64_t version = 0;
        bool valid = Decode(HTTPManager::SendJson("interaction.php", "{}"), enabled, version);
        if (valid && target >= 0 && enabled != (target == 1)) {
            valid = Decode(HTTPManager::SendJson("interaction.php", std::string("{\"enabled\":")
                + (target ? "true" : "false") + ",\"generation\":" + std::to_string(version) + "}"), enabled, version)
                && enabled == (target == 1);
        }
        if (Epoch() == requestEpoch) {
            if (valid) { generation = version; desired = enabled ? 1 : 0; status = enabled ? 1 : 0; }
            else status = 3;
        }
    }, [requestEpoch](bool, const char*) {
        if (Epoch() == requestEpoch && Status() == 2) status = 3;
        busy = false;
    });
    if (!task) { busy = false; status = 3; }
}
std::wstring Headers() {
    return L"\r\nX-Dialectic-Generation: " + std::to_wstring(Generation())
        + L"\r\nX-Dialectic-Passive: " + (Allowed() ? L"0" : L"1");
}
}
