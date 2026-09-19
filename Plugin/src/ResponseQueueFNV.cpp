#include "Interaction.h"
// ResponseQueueFNV.cpp - CHIM-style parsed response queue for Dialectic JSON lines

#include "MultiplayerSharing.h"
#include "ResponseQueueFNV.h"

#include "ActionManager.h"
#include "HTTPManager.h"
#include "Logger.h"
#include "IngameNotifier.h"
#include "RuntimeGeneration.h"
#include "SpeakManager.h"

#include <deque>
#include <chrono>
#include <mutex>
#include <utility>
#include <map>
#include <set>

namespace ResponseQueueFNV {
namespace {

enum class ItemType {
    Dialogue,
    Action
};

struct QueueItem {
    ItemType type = ItemType::Dialogue;
    DialogueLine dialogue;
    std::string actionJson;
    std::string source;
    bool directorScene = false;
    std::string directorSceneId;
    uint64_t responseGeneration = 0;
    uint64_t runtimeGeneration = 0;
    std::chrono::steady_clock::time_point enqueuedAt;
};

// Lines cancelled before reaching SpeakManager still need a terminal delivery report.
void AbortQueuedDirectorLine(const QueueItem& item) {
    if (item.type != ItemType::Dialogue || !item.dialogue.directorScene || item.dialogue.utteranceId.empty()) return;
    const auto& line = item.dialogue;
    CompleteDirectorSpeech(line.requestId, line.utteranceId, "aborted");
    HTTPManager::SendDialogueDeliveryAck(line.speaker, line.actorFormId, line.text,
        line.ttsCacheKey, line.utteranceId, "aborted", line.requestId);
}

struct DirectorProgress {
    std::set<std::string> pending;
    std::string outcome = "completed";
    std::size_t actions = 0;
};
std::mutex g_directorMutex;
std::map<std::string, DirectorProgress> g_directorScenes;

// Called with the scene lock held after speech or attached-action dispatch completes.
void FinishDirectorScene(const std::string& id) {
    const auto scene = g_directorScenes.find(id);
    if (scene != g_directorScenes.end() && scene->second.pending.empty() && scene->second.actions == 0) {
        IngameNotifier::Notify("Director scene stopped.");
        g_directorScenes.erase(scene);
    }
}

void CompleteDirectorAction(const std::string& id) {
    std::lock_guard<std::mutex> lock(g_directorMutex);
    const auto scene = g_directorScenes.find(id);
    if (scene == g_directorScenes.end()) return;
    if (scene->second.actions > 0) --scene->second.actions;
    FinishDirectorScene(id);
}

// Clearing response generations ends diagnostics even when an audio worker finishes later.
void CancelDirectorScenes() {
    std::lock_guard<std::mutex> lock(g_directorMutex);
    for (std::size_t i = 0; i < g_directorScenes.size(); ++i) {
        IngameNotifier::Notify("Director scene stopped.");
    }
    g_directorScenes.clear();
}

std::mutex g_mutex;
std::deque<QueueItem> g_items;
bool g_unfinished = false;
std::string g_unfinishedSource;
uint64_t g_activeGeneration = 0;
uint64_t g_activeRuntimeGeneration = 0;
uint64_t g_totalQueued = 0;
uint64_t g_totalDispatched = 0;
uint64_t g_totalDroppedStale = 0;

static std::string Preview(const std::string& value, std::size_t maxLen = 80) {
    if (value.size() <= maxLen) {
        return value;
    }
    return value.substr(0, maxLen);
}

} // namespace

void BeginDirectorScene(const std::string& id, const std::vector<DialogueLine>& lines, std::size_t actions) {
    std::lock_guard<std::mutex> lock(g_directorMutex);
    auto& scene = g_directorScenes[id];
    for (const auto& line : lines) scene.pending.insert(line.utteranceId);
    scene.actions = actions;
    IngameNotifier::Notify("Director scene started.");
}

void CompleteDirectorSpeech(const std::string& id, const std::string& utteranceId, const std::string& state) {
    if (state == "playing") return;
    std::lock_guard<std::mutex> lock(g_directorMutex);
    const auto scene = g_directorScenes.find(id);
    if (scene == g_directorScenes.end() || !scene->second.pending.erase(utteranceId)) return;
    if (state == "failed") scene->second.outcome = "failed";
    else if (state == "aborted" && scene->second.outcome != "failed") scene->second.outcome = "cancelled";
    FinishDirectorScene(id);
}

static bool IsCurrentGenerationLocked(uint64_t generation) {
    return Interaction::Allowed() && (generation == 0 || g_activeGeneration == 0 || generation == g_activeGeneration);
}

void SetActiveGeneration(uint64_t generation, const char* source) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_activeGeneration == generation) {
        return;
    }

    CancelDirectorScenes();
    g_activeGeneration = generation;
    g_activeRuntimeGeneration = RuntimeGeneration::Current();
    Logger::LogInfo("ResponseQueueFNV: active_generation=%llu runtime_generation=%llu source=%s pending=%zu",
        static_cast<unsigned long long>(g_activeGeneration),
        static_cast<unsigned long long>(g_activeRuntimeGeneration),
        source ? source : "",
        g_items.size());
}

bool IsCurrentGeneration(uint64_t generation) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return IsCurrentGenerationLocked(generation);
}

void MarkUnfinished(bool unfinished, const char* source, uint64_t generation) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (unfinished && generation != 0) {
        g_activeGeneration = generation;
    } else if (!unfinished && generation != 0 && g_activeGeneration != 0 && generation != g_activeGeneration) {
        Logger::LogInfo("ResponseQueueFNV: ignored stale unfinished=false generation=%llu active=%llu source=%s pending=%zu",
            static_cast<unsigned long long>(generation),
            static_cast<unsigned long long>(g_activeGeneration),
            source ? source : "",
            g_items.size());
        return;
    } else if (generation != 0 && g_activeGeneration == 0) {
        g_activeGeneration = generation;
    }
    if (g_unfinished == unfinished &&
        (!unfinished || g_unfinishedSource == (source ? source : ""))) {
        return;
    }

    g_unfinished = unfinished;
    g_unfinishedSource = unfinished ? (source ? source : "") : "";
    Logger::LogInfo("ResponseQueueFNV: unfinished=%d source=%s pending=%zu",
        g_unfinished ? 1 : 0,
        source ? source : "",
        g_items.size());
}

bool IsUnfinished() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_unfinished;
}

void EnqueueDialogue(const DialogueLine& line, const char* source) {
    QueueItem item;
    item.type = ItemType::Dialogue;
    item.dialogue = line;
    item.source = source ? source : "ResponseQueueFNV";
    item.enqueuedAt = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lock(g_mutex);
    item.responseGeneration = line.responseGeneration;
    item.runtimeGeneration = line.runtimeGeneration != 0
        ? line.runtimeGeneration
        : (g_activeRuntimeGeneration != 0 ? g_activeRuntimeGeneration : RuntimeGeneration::Current());
    item.dialogue.runtimeGeneration = item.runtimeGeneration;
    if (!IsCurrentGenerationLocked(line.responseGeneration)) {
        ++g_totalDroppedStale;
        AbortQueuedDirectorLine(item);
        Logger::LogInfo("ResponseQueueFNV: dropped stale dialogue speaker='%s' generation=%llu active=%llu source=%s",
            line.speaker.c_str(),
            static_cast<unsigned long long>(line.responseGeneration),
            static_cast<unsigned long long>(g_activeGeneration),
            source ? source : "ResponseQueueFNV");
        return;
    }
    g_items.push_back(std::move(item));
    ++g_totalQueued;
    Logger::LogInfo("ResponseQueueFNV: queued dialogue speaker='%s' final=%d utterance='%s' text='%s' pending=%zu state=queued",
        line.speaker.c_str(),
        line.isFinalResponseLine ? 1 : 0,
        line.utteranceId.c_str(),
        Preview(line.text).c_str(),
        g_items.size());
}

void EnqueueAction(const std::string& lineObject, const char* source, uint64_t responseGeneration, bool directorScene, const std::string& directorSceneId) {
    QueueItem item;
    item.type = ItemType::Action;
    item.actionJson = lineObject;
    item.directorScene = directorScene;
    item.directorSceneId = directorSceneId;
    item.source = source ? source : "ResponseQueueFNV";
    item.enqueuedAt = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lock(g_mutex);
    item.responseGeneration = responseGeneration;
    item.runtimeGeneration = g_activeRuntimeGeneration != 0
        ? g_activeRuntimeGeneration
        : RuntimeGeneration::Current();
    if (!IsCurrentGenerationLocked(responseGeneration)) {
        ++g_totalDroppedStale;
        if (directorScene) CompleteDirectorAction(directorSceneId);
        Logger::LogInfo("ResponseQueueFNV: dropped stale action generation=%llu active=%llu source=%s",
            static_cast<unsigned long long>(responseGeneration),
            static_cast<unsigned long long>(g_activeGeneration),
            source ? source : "ResponseQueueFNV");
        return;
    }
    g_items.push_back(std::move(item));
    ++g_totalQueued;
    Logger::LogInfo("ResponseQueueFNV: queued action source=%s pending=%zu state=queued",
        source ? source : "ResponseQueueFNV",
        g_items.size());
}

bool DispatchPending(std::size_t maxItems) {
    if (MultiplayerSharing::IsListener()) return false;
    bool dispatchedAny = false;
    std::size_t dispatchedThisCall = 0;

    while (maxItems == 0 || dispatchedThisCall < maxItems) {
        bool sceneAction = false;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_items.empty()) break;
            sceneAction = g_items.front().directorScene;
        }
        bool speechPending = false;
        if (sceneAction) {
            const auto speech = SpeakManager::GetQueueStatus();
            speechPending = speech.isProcessing || speech.isPlaying || speech.currentPlaybackLineActive
                || speech.dialogueLinesQueued > 0 || speech.ttsDownloadsInProgress > 0
                || speech.preparedAudioCount > 0 || speech.ttsTasksPending > 0 || speech.ttsTasksActive > 0;
        }
        QueueItem item;
        bool droppedStale = false;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_items.empty()) {
                break;
            }
            // Attached actions wait for preceding speech, never for action completion. Check stale
            // generations first so a cancelled scene never blocks a new response.
            const auto& next = g_items.front();
            if (next.directorScene && speechPending && IsCurrentGenerationLocked(next.responseGeneration)
                && RuntimeGeneration::IsCurrent(next.runtimeGeneration)) {
                break;
            }
            item = std::move(g_items.front());
            g_items.pop_front();
            if (!IsCurrentGenerationLocked(item.responseGeneration) ||
                !RuntimeGeneration::IsCurrent(item.runtimeGeneration)) {
                ++g_totalDroppedStale;
                Logger::LogInfo(
                    "ResponseQueueFNV: dropped stale queued item response_generation=%llu active=%llu runtime_generation=%llu current_runtime=%llu source=%s",
                    static_cast<unsigned long long>(item.responseGeneration),
                    static_cast<unsigned long long>(g_activeGeneration),
                    static_cast<unsigned long long>(item.runtimeGeneration),
                    static_cast<unsigned long long>(RuntimeGeneration::Current()),
                    item.source.c_str());
                droppedStale = true;
            } else {
                ++g_totalDispatched;
            }
        }

        if (droppedStale) {
            AbortQueuedDirectorLine(item);
            if (item.directorScene) CompleteDirectorAction(item.directorSceneId);
            continue;
        }

        dispatchedAny = true;
        ++dispatchedThisCall;
        const auto dispatchLatencyMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - item.enqueuedAt).count();

        if (item.type == ItemType::Dialogue) {
            const auto& line = item.dialogue;
            Logger::LogInfo("ResponseQueueFNV: dispatch dialogue speaker='%s' final=%d utterance='%s' source=%s latency_ms=%lld state=dispatching",
                line.speaker.c_str(),
                line.isFinalResponseLine ? 1 : 0,
                line.utteranceId.c_str(),
                item.source.c_str(),
                static_cast<long long>(dispatchLatencyMs));
            SpeakManager::QueueDialogue(line.text,
                                        line.speaker,
                                        line.actorFormId,
                                        line.isFinalResponseLine,
                                        line.listenerHint,
                                        line.rechatTargetHint,
                                        line.rechatDepth,
                                        line.ttsCacheKey,
                                        line.utteranceId,
                                        line.requestId,
                                        line.runtimeGeneration,
                                        line.listenerFormId,
                                        line.rechatTargetFormId,
                                        line.displayName,
                                        line.directorScene);
            continue;
        }

        bool skipDirectorAction = false;
        if (item.directorScene) {
            std::lock_guard<std::mutex> lock(g_directorMutex);
            const auto scene = g_directorScenes.find(item.directorSceneId);
            skipDirectorAction = scene == g_directorScenes.end() || scene->second.outcome != "completed";
        }
        if (skipDirectorAction) {
            CompleteDirectorAction(item.directorSceneId);
            continue;
        }
        Logger::LogInfo("ResponseQueueFNV: dispatch action source=%s latency_ms=%lld state=dispatching",
            item.source.c_str(),
            static_cast<long long>(dispatchLatencyMs));
        ActionManager::HandleRoleCommandJson(
            item.actionJson, item.source.c_str(), item.runtimeGeneration);
        if (item.directorScene) CompleteDirectorAction(item.directorSceneId);
    }

    return dispatchedAny;
}

void Clear(const char* reason) {
    CancelDirectorScenes();
    std::deque<QueueItem> cleared;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        cleared.swap(g_items);
        g_unfinished = false;
        g_unfinishedSource.clear();
    }
    for (const auto& item : cleared) AbortQueuedDirectorLine(item);
    Logger::LogInfo("ResponseQueueFNV: cleared pending=%zu reason=%s", cleared.size(), reason ? reason : "clear");
}

bool HasPending() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return !g_items.empty();
}

QueueStatus GetStatus() {
    QueueStatus status;
    std::lock_guard<std::mutex> lock(g_mutex);
    status.unfinished = g_unfinished;
    status.activeGeneration = g_activeGeneration;
    status.activeRuntimeGeneration = g_activeRuntimeGeneration;
    status.pendingItems = g_items.size();
    status.totalQueued = g_totalQueued;
    status.totalDispatched = g_totalDispatched;
    status.totalDroppedStale = g_totalDroppedStale;
    status.unfinishedSource = g_unfinishedSource;

    for (const auto& item : g_items) {
        if (item.type == ItemType::Dialogue) {
            ++status.pendingDialogueLines;
        } else {
            ++status.pendingActionLines;
        }
    }
    return status;
}

} // namespace ResponseQueueFNV
