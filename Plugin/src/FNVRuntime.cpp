#include "FNVRuntime.h"

#include "ActionManager.h"
#include "ActorPositionResolverFNV.h"
#include "Config.h"
#include "GameThreadDispatcher.h"
#include "HTTPManager.h"
#include "Logger.h"
#include "NativeComparisonTelemetry.h"
#include "RuntimeEventBus.h"
#include "RuntimeGeneration.h"
#include "RuntimeSnapshot.h"
#include "SpatialAwarenessFNV.h"
#include "SpatialPathProviderFNV.h"
#include "SpatialSnapshotManagerFNV.h"
#include "SpeakManager.h"
#include "TaskManager.h"
#include "VoiceRecorder.h"
#include "XNVSEAdapter.h"

#include <atomic>
#include <chrono>
#include <algorithm>
#include <sstream>
#include <Windows.h>
#include <Psapi.h>

namespace FNVRuntime {
namespace {

extern "C" void Dialectic_UpdateFrame(float deltaTime);

std::atomic<bool> g_available{false};
std::atomic<std::uint64_t> g_frameSequence{0};
std::atomic<bool> g_nativeFramePumpLogged{false};
std::uint32_t g_lastCellFormId = 0;
std::uint32_t g_lastWorldspaceFormId = 0;
bool g_hadMenuState = false;
bool g_lastInMenu = false;
bool g_lastPaused = false;
bool g_hadCombatState = false;
bool g_lastInCombat = false;
std::chrono::steady_clock::time_point g_lastActorCapture;
std::chrono::steady_clock::time_point g_lastEquipmentCapture;
std::chrono::steady_clock::time_point g_lastReferenceCapture;
std::chrono::steady_clock::time_point g_lastNavCaptureAttempt;
std::chrono::steady_clock::time_point g_lastQuestCapture;
std::chrono::steady_clock::time_point g_lastHealthLog;
std::chrono::steady_clock::time_point g_lastSpeechHealthLog;
SpeakManager::SpeechDiagnostics g_previousSpeechDiagnostics;
XNVSEAdapter::NativePresentationDiagnostics g_previousPresentationDiagnostics;

struct CaptureTiming {
    std::uint64_t calls{0};
    std::uint64_t totalUs{0};
    std::uint64_t maxUs{0};
};

CaptureTiming g_gameStateTiming;
CaptureTiming g_actorTiming;
CaptureTiming g_referenceTiming;
CaptureTiming g_navTiming;
CaptureTiming g_questTiming;
CaptureTiming g_totalCaptureTiming;
std::chrono::steady_clock::time_point g_lastCaptureTimingLog;
std::size_t g_actorHighWater = 0;
std::size_t g_referenceHighWater = 0;
std::size_t g_eventHighWater = 0;
std::size_t g_taskPendingHighWater = 0;
std::size_t g_dispatchPendingHighWater = 0;

struct ProcessHealth {
    std::uint64_t workingSetBytes{0};
    std::uint64_t privateBytes{0};
    DWORD handles{0};
    bool valid{false};
};

ProcessHealth CaptureProcessHealth() {
    ProcessHealth health;
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) {
        health.workingSetBytes = counters.WorkingSetSize;
        health.privateBytes = counters.PrivateUsage;
        health.valid = true;
    }
    GetProcessHandleCount(GetCurrentProcess(), &health.handles);
    return health;
}

void RecordCaptureTiming(CaptureTiming& timing,
                         std::chrono::steady_clock::time_point startedAt) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - startedAt).count();
    const std::uint64_t elapsedUs = elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 0;
    ++timing.calls;
    timing.totalUs += elapsedUs;
    if (elapsedUs > timing.maxUs) {
        timing.maxUs = elapsedUs;
    }
}

double AverageCaptureMs(const CaptureTiming& timing) {
    return timing.calls == 0
        ? 0.0
        : static_cast<double>(timing.totalUs) / static_cast<double>(timing.calls) / 1000.0;
}

double MaxCaptureMs(const CaptureTiming& timing) {
    return static_cast<double>(timing.maxUs) / 1000.0;
}

std::uint64_t CounterDelta(std::uint64_t current, std::uint64_t previous) {
    return current >= previous ? current - previous : current;
}

RuntimeEventBus::EventType MapEventType(XNVSEAdapter::LifecycleEvent event) {
    using Source = XNVSEAdapter::LifecycleEvent;
    using Target = RuntimeEventBus::EventType;
    switch (event) {
        case Source::PreLoadGame: return Target::PreLoadGame;
        case Source::LoadGame: return Target::LoadGame;
        case Source::PostLoadGame: return Target::PostLoadGame;
        case Source::NewGame: return Target::NewGame;
        case Source::SaveGame: return Target::SaveGame;
        case Source::ExitToMainMenu: return Target::ExitToMainMenu;
        case Source::ExitGame: return Target::ExitGame;
        case Source::ReloadConfig: return Target::ReloadConfig;
        case Source::RefSet3D:
        case Source::RefAttached:
        case Source::FormLoaded: return Target::ActorAttached;
        case Source::RefUnset3D:
        case Source::FormUnloaded: return Target::ActorDetached;
        case Source::CellStateChanged: return Target::CellStateChanged;
        case Source::CellRefsLoaded: return Target::CellReferencesLoaded;
        default: return Target::PostLoad;
    }
}

bool HasRuntimeEventConsumer(XNVSEAdapter::LifecycleEvent event) {
    using Event = XNVSEAdapter::LifecycleEvent;
    switch (event) {
        case Event::PreLoadGame:
        case Event::LoadGame:
        case Event::NewGame:
        case Event::ExitToMainMenu:
        case Event::ExitGame:
        case Event::ReloadConfig:
            return true;
        default:
            return false;
    }
}

bool InvalidatesGeneration(XNVSEAdapter::LifecycleEvent event) {
    using Event = XNVSEAdapter::LifecycleEvent;
    return event == Event::PreLoadGame || event == Event::LoadGame ||
        event == Event::NewGame || event == Event::ExitToMainMenu || event == Event::ExitGame;
}

void ResetNativeState(const char* reason) {
    ActionManager::CancelNativeRuntimeActions(reason);
    XNVSEAdapter::RestoreNativeDialogueGuards();
    XNVSEAdapter::InvalidateNativePresentation();
    XNVSEAdapter::InvalidateNativeObjectCache();
    ActorPositionResolverFNV::InvalidateCache();
    SpatialAwarenessFNV::InvalidateCache();
    SpatialSnapshotManagerFNV::Invalidate();
    const std::uint64_t generation = RuntimeGeneration::Advance(reason);
    RuntimeSnapshot::Clear();
    RuntimeEventBus::Clear();
    GameThreadDispatcher::CancelAll(reason);
    TaskManager::CancelOlderThanGeneration(generation);
    g_lastCellFormId = 0;
    g_lastWorldspaceFormId = 0;
    g_hadMenuState = false;
    g_hadCombatState = false;
    g_lastActorCapture = {};
    g_lastEquipmentCapture = {};
    g_lastReferenceCapture = {};
    g_lastNavCaptureAttempt = {};
    g_lastQuestCapture = {};
    g_lastHealthLog = {};
    g_lastSpeechHealthLog = {};
    g_lastCaptureTimingLog = {};
    g_gameStateTiming = {};
    g_actorTiming = {};
    g_referenceTiming = {};
    g_navTiming = {};
    g_questTiming = {};
    g_totalCaptureTiming = {};
    g_actorHighWater = 0;
    g_referenceHighWater = 0;
    g_eventHighWater = 0;
    g_taskPendingHighWater = 0;
    g_dispatchPendingHighWater = 0;
    g_previousSpeechDiagnostics = SpeakManager::GetDiagnostics();
    g_previousPresentationDiagnostics = XNVSEAdapter::GetNativePresentationDiagnostics();
}

void CaptureFrame() {
    const auto captureStartedAt = std::chrono::steady_clock::now();
    XNVSEAdapter::NativeGameState native;
    const auto gameStateStartedAt = std::chrono::steady_clock::now();
    if (!XNVSEAdapter::CaptureNativeGameState(native)) {
        RecordCaptureTiming(g_gameStateTiming, gameStateStartedAt);
        RecordCaptureTiming(g_totalCaptureTiming, captureStartedAt);
        return;
    }
    RecordCaptureTiming(g_gameStateTiming, gameStateStartedAt);

    if (g_lastCellFormId != 0 && native.cellFormId != 0 && native.cellFormId != g_lastCellFormId) {
        const std::uint32_t previous = g_lastCellFormId;
        Logger::LogInfo("[NATIVE_RUNTIME] cell changed old=0x%08X new=0x%08X",
            previous, native.cellFormId);
        ActionManager::CancelNativeRuntimeActions("cell_changed");
        XNVSEAdapter::RestoreNativeDialogueGuards();
        XNVSEAdapter::InvalidateNativeObjectCache();
        ActorPositionResolverFNV::InvalidateCache();
        SpatialAwarenessFNV::InvalidateCache();
        SpatialSnapshotManagerFNV::Invalidate();
        const std::uint64_t generation = RuntimeGeneration::Advance("cell_changed");
        GameThreadDispatcher::CancelAll("cell_changed");
        TaskManager::CancelOlderThanGeneration(generation);
        g_lastActorCapture = {};
        g_lastEquipmentCapture = {};
        g_lastReferenceCapture = {};
        g_lastNavCaptureAttempt = {};
        g_lastQuestCapture = {};
        RuntimeEventBus::Publish({RuntimeEventBus::EventType::CellChanged, generation, 0,
            native.cellFormId, previous, false, {}});
    }
    if (g_lastWorldspaceFormId != 0 && native.worldspaceFormId != g_lastWorldspaceFormId) {
        Logger::LogInfo("[NATIVE_RUNTIME] worldspace changed old=0x%08X new=0x%08X",
            g_lastWorldspaceFormId, native.worldspaceFormId);
    }

    if (!g_hadMenuState || g_lastInMenu != native.inMenu || g_lastPaused != native.paused) {
        g_hadMenuState = true;
        g_lastInMenu = native.inMenu;
        g_lastPaused = native.paused;
    }
    if (!g_hadCombatState || g_lastInCombat != native.inCombat) {
        g_hadCombatState = true;
        g_lastInCombat = native.inCombat;
    }

    g_lastCellFormId = native.cellFormId;
    g_lastWorldspaceFormId = native.worldspaceFormId;

    RuntimeSnapshot::GameState state;
    state.cellName = native.cellName;
    state.worldspaceName = native.worldspaceName;
    state.playerName = native.playerName;
    state.valid = native.valid;
    state.inGame = native.inGame;
    state.inMenu = native.inMenu;
    state.paused = native.paused;
    state.pipboyOpen = native.pipboyOpen;
    state.pauseMenuOpen = native.pauseMenuOpen;
    state.dialogueMenuOpen = native.dialogueMenuOpen;
    state.barterMenuOpen = native.barterMenuOpen;
    state.containerMenuOpen = native.containerMenuOpen;
    state.loadingMenuOpen = native.loadingMenuOpen;
    state.inCombat = native.inCombat;
    state.player3DLoaded = native.player3DLoaded;
    state.playerSneaking = native.playerSneaking;
    state.playerFormId = native.playerFormId;
    state.cellFormId = native.cellFormId;
    state.worldspaceFormId = native.worldspaceFormId;
    state.crosshairFormId = native.crosshairFormId;
    state.playerX = native.playerX;
    state.playerY = native.playerY;
    state.playerZ = native.playerZ;
    state.playerPitch = native.playerPitch;
    state.playerYaw = native.playerYaw;
    state.generation = RuntimeGeneration::Current();
    state.frameSequence = g_frameSequence.fetch_add(1) + 1;
    RuntimeSnapshot::UpdateGameState(std::move(state));

    const auto now = std::chrono::steady_clock::now();
    const auto navStatus = SpatialPathProviderFNV::GetNativeGraphStatus();
    const bool navGraphMissing = navStatus.cellFormId != native.cellFormId ||
        navStatus.generation != RuntimeGeneration::Current() || (!navStatus.ready && !navStatus.building);
    if (native.cellFormId != 0 && navGraphMissing &&
        (g_lastNavCaptureAttempt.time_since_epoch().count() == 0 ||
         now - g_lastNavCaptureAttempt >= std::chrono::seconds(2))) {
        XNVSEAdapter::NativeNavSceneState nativeNav;
        const auto navStartedAt = std::chrono::steady_clock::now();
        if (XNVSEAdapter::CaptureNativeNavScene(nativeNav) && nativeNav.valid) {
            RuntimeSnapshot::NavSceneState scene;
            scene.valid = nativeNav.valid;
            scene.interior = nativeNav.interior;
            scene.complete = nativeNav.complete;
            scene.cellFormId = nativeNav.cellFormId;
            scene.worldspaceFormId = nativeNav.worldspaceFormId;
            scene.generation = RuntimeGeneration::Current();
            scene.meshes.reserve(nativeNav.meshes.size());
            for (const auto& nativeMesh : nativeNav.meshes) {
                RuntimeSnapshot::NavMeshState mesh;
                mesh.formId = nativeMesh.formId;
                mesh.vertices.reserve(nativeMesh.vertices.size());
                for (const auto& vertex : nativeMesh.vertices) {
                    mesh.vertices.push_back({vertex.x, vertex.y, vertex.z});
                }
                mesh.triangles.reserve(nativeMesh.triangles.size());
                for (const auto& nativeTriangle : nativeMesh.triangles) {
                    RuntimeSnapshot::NavTriangle triangle;
                    for (std::size_t index = 0; index < 3; ++index) {
                        triangle.vertices[index] = nativeTriangle.vertices[index];
                        triangle.neighbors[index] = nativeTriangle.neighbors[index];
                    }
                    triangle.flags = nativeTriangle.flags;
                    triangle.doorFormId = nativeTriangle.doorFormId;
                    mesh.triangles.push_back(triangle);
                }
                scene.meshes.push_back(std::move(mesh));
            }
            SpatialPathProviderFNV::PublishNativeScene(scene);
            RuntimeSnapshot::UpdateNavScene(std::move(scene));
        }
        RecordCaptureTiming(g_navTiming, navStartedAt);
        g_lastNavCaptureAttempt = now;
    }

    if (g_lastActorCapture.time_since_epoch().count() == 0 ||
        now - g_lastActorCapture >= std::chrono::seconds(1)) {
        std::vector<XNVSEAdapter::NativeActorState> nativeActors;
        const auto actorsStartedAt = std::chrono::steady_clock::now();
        const bool refreshEquipment = g_lastEquipmentCapture.time_since_epoch().count() == 0 ||
            now - g_lastEquipmentCapture >= std::chrono::seconds(15);
        if (XNVSEAdapter::CaptureNativeActors(nativeActors, refreshEquipment)) {
            std::vector<RuntimeSnapshot::ActorState> actors;
            actors.reserve(nativeActors.size());
            for (const auto& source : nativeActors) {
                RuntimeSnapshot::ActorState actor;
                actor.name = source.name;
                actor.raceName = source.raceName;
                actor.formId = source.formId;
                actor.baseFormId = source.baseFormId;
                actor.cellFormId = source.cellFormId;
                actor.worldspaceFormId = source.worldspaceFormId;
            actor.raceFormId = source.raceFormId;
            actor.voiceFormId = source.voiceFormId;
            actor.combatTargetFormId = source.combatTargetFormId;
            actor.packageFormId = source.packageFormId;
                actor.equippedWeaponFormId = source.equippedWeaponFormId;
                actor.referenceType = source.referenceType;
                actor.baseType = source.baseType;
                actor.creature = source.creature;
                actor.deleted = source.deleted;
                actor.loaded3D = source.loaded3D;
                actor.interior = source.interior;
                actor.inCombat = source.inCombat;
                actor.hostileToPlayer = source.hostileToPlayer;
                actor.playerTeammate = source.playerTeammate;
                actor.female = source.female;
                actor.dead = source.dead;
                actor.weaponDrawn = source.weaponDrawn;
                actor.moving = source.moving;
                actor.running = source.running;
                actor.sneaking = source.sneaking;
                actor.facingStateKnown = source.facingStateKnown;
                actor.seated = source.seated;
                actor.animationBusy = source.animationBusy;
                actor.sitSleepState = source.sitSleepState;
                actor.animationAction = source.animationAction;
                actor.level = source.level;
                actor.health = source.health;
                actor.healthMax = source.healthMax;
                actor.actionPoints = source.actionPoints;
                actor.actionPointsMax = source.actionPointsMax;
                actor.distanceToPlayer = source.distanceToPlayer;
                actor.scale = source.scale;
                actor.x = source.x;
                actor.y = source.y;
                actor.z = source.z;
                actor.yaw = source.yaw;
                actor.equipment.reserve(source.equipment.size());
                for (const auto& sourceItem : source.equipment) {
                    RuntimeSnapshot::EquipmentItem item;
                    item.name = sourceItem.name;
                    item.baseFormId = sourceItem.baseFormId;
                    item.type = sourceItem.type;
                    item.condition = sourceItem.condition;
                    actor.equipment.push_back(std::move(item));
                }
                actors.push_back(actor);
            }
            g_actorHighWater = (std::max)(g_actorHighWater, actors.size());
            RuntimeSnapshot::UpdateActors(std::move(actors), RuntimeGeneration::Current());
            if (refreshEquipment) {
                g_lastEquipmentCapture = now;
            }
        }
        RecordCaptureTiming(g_actorTiming, actorsStartedAt);
        g_lastActorCapture = now;
    }

    if (g_lastReferenceCapture.time_since_epoch().count() == 0 ||
        now - g_lastReferenceCapture >= std::chrono::seconds(3)) {
        std::vector<XNVSEAdapter::NativeReferenceState> nativeReferences;
        const auto referencesStartedAt = std::chrono::steady_clock::now();
        if (XNVSEAdapter::CaptureNativeReferences(nativeReferences)) {
            std::vector<RuntimeSnapshot::ReferenceState> references;
            references.reserve(nativeReferences.size());
            for (const auto& source : nativeReferences) {
                RuntimeSnapshot::ReferenceState reference;
                reference.name = source.name;
                reference.destinationName = source.destinationName;
                reference.formId = source.formId;
                reference.baseFormId = source.baseFormId;
                reference.cellFormId = source.cellFormId;
                reference.destinationCellFormId = source.destinationCellFormId;
                reference.baseType = source.baseType;
                reference.deleted = source.deleted;
                reference.taken = source.taken;
                reference.loaded3D = source.loaded3D;
                reference.crosshair = source.crosshair;
                reference.teleportDoor = source.teleportDoor;
                reference.openStateKnown = source.openStateKnown;
                reference.locked = source.locked;
                reference.openState = source.openState;
                reference.distanceToPlayer = source.distanceToPlayer;
                reference.x = source.x;
                reference.y = source.y;
                reference.z = source.z;
                reference.yaw = source.yaw;
                references.push_back(std::move(reference));
            }
            g_referenceHighWater = (std::max)(g_referenceHighWater, references.size());
            RuntimeSnapshot::UpdateReferences(std::move(references), RuntimeGeneration::Current());
        }
        RecordCaptureTiming(g_referenceTiming, referencesStartedAt);
        g_lastReferenceCapture = now;
    }

    if (g_lastQuestCapture.time_since_epoch().count() == 0 ||
        now - g_lastQuestCapture >= std::chrono::seconds(1)) {
        XNVSEAdapter::NativeQuestState nativeQuest;
        const auto questStartedAt = std::chrono::steady_clock::now();
        if (XNVSEAdapter::CaptureNativeQuest(nativeQuest)) {
            RuntimeSnapshot::QuestState quest;
            quest.valid = nativeQuest.valid;
            quest.formId = nativeQuest.formId;
            quest.name = nativeQuest.name;
            quest.editorId = nativeQuest.editorId;
            quest.generation = RuntimeGeneration::Current();
            quest.objectives.reserve(nativeQuest.objectives.size());
            for (const auto& source : nativeQuest.objectives) {
                RuntimeSnapshot::QuestObjective objective;
                objective.objectiveId = source.objectiveId;
                objective.text = source.text;
                quest.objectives.push_back(std::move(objective));
            }
            RuntimeSnapshot::UpdateQuest(std::move(quest));
        }
        RecordCaptureTiming(g_questTiming, questStartedAt);
        g_lastQuestCapture = now;
    }

    RecordCaptureTiming(g_totalCaptureTiming, captureStartedAt);
    g_eventHighWater = (std::max)(g_eventHighWater, RuntimeEventBus::PendingCount());
    if (g_lastCaptureTimingLog.time_since_epoch().count() == 0 ||
        now - g_lastCaptureTimingLog >= std::chrono::seconds(30)) {
        Logger::LogInfo(
            "[NATIVE_CAPTURE_PERF] frame(calls=%llu avg_ms=%.3f max_ms=%.3f) "
            "state(calls=%llu avg_ms=%.3f max_ms=%.3f) actors(calls=%llu avg_ms=%.3f max_ms=%.3f) "
            "refs(calls=%llu avg_ms=%.3f max_ms=%.3f) nav(calls=%llu avg_ms=%.3f max_ms=%.3f) "
            "quest(calls=%llu avg_ms=%.3f max_ms=%.3f)",
            static_cast<unsigned long long>(g_totalCaptureTiming.calls),
            AverageCaptureMs(g_totalCaptureTiming), MaxCaptureMs(g_totalCaptureTiming),
            static_cast<unsigned long long>(g_gameStateTiming.calls),
            AverageCaptureMs(g_gameStateTiming), MaxCaptureMs(g_gameStateTiming),
            static_cast<unsigned long long>(g_actorTiming.calls),
            AverageCaptureMs(g_actorTiming), MaxCaptureMs(g_actorTiming),
            static_cast<unsigned long long>(g_referenceTiming.calls),
            AverageCaptureMs(g_referenceTiming), MaxCaptureMs(g_referenceTiming),
            static_cast<unsigned long long>(g_navTiming.calls),
            AverageCaptureMs(g_navTiming), MaxCaptureMs(g_navTiming),
            static_cast<unsigned long long>(g_questTiming.calls),
            AverageCaptureMs(g_questTiming), MaxCaptureMs(g_questTiming));
        g_gameStateTiming = {};
        g_actorTiming = {};
        g_referenceTiming = {};
        g_navTiming = {};
        g_questTiming = {};
        g_totalCaptureTiming = {};
        g_lastCaptureTimingLog = now;
    }

    if (g_lastSpeechHealthLog.time_since_epoch().count() == 0 ||
        now - g_lastSpeechHealthLog >= std::chrono::seconds(15)) {
        const auto speech = SpeakManager::GetDiagnostics();
        const auto presentation = XNVSEAdapter::GetNativePresentationDiagnostics();
        const auto queue = SpeakManager::GetQueueStatus();
        const auto http = HTTPManager::GetQueueStatus();
        const auto dispatcher = GameThreadDispatcher::GetStatus();
        const auto windowMs = g_lastSpeechHealthLog.time_since_epoch().count() == 0
            ? 0LL
            : std::chrono::duration_cast<std::chrono::milliseconds>(now - g_lastSpeechHealthLog).count();

        const auto faceGenFailureDelta = CounterDelta(
            presentation.faceGenFailed, g_previousPresentationDiagnostics.faceGenFailed);
        const auto scriptMfgFallbackDelta = CounterDelta(
            speech.scriptMfgFallbacks, g_previousSpeechDiagnostics.scriptMfgFallbacks);

        Logger::LogInfo(
            "[SPEECH_HEALTH] window_ms=%lld generation=%llu speaker='%s' "
            "queue(lines=%d downloads=%d prepared=%d tasks=%d/%d processing=%d playing=%d) "
            "http(stream=%d active_streams=%d tasks=%zu/%zu responses=%zu dispatched=%llu dropped=%llu) "
            "dispatcher(pending=%zu queued=%llu executed=%llu cancelled=%llu stale=%llu rejected=%llu)",
            windowMs,
            static_cast<unsigned long long>(RuntimeGeneration::Current()),
            queue.currentSpeaker.c_str(),
            queue.dialogueLinesQueued,
            queue.ttsDownloadsInProgress,
            queue.preparedAudioCount,
            queue.ttsTasksPending,
            queue.ttsTasksActive,
            queue.isProcessing ? 1 : 0,
            queue.isPlaying ? 1 : 0,
            http.streamInProgress ? 1 : 0,
            http.activeStreamTasks,
            http.pendingHttpTasks,
            http.activeHttpTasks,
            http.httpResponsesQueued,
            static_cast<unsigned long long>(http.totalResponsesDispatched),
            static_cast<unsigned long long>(http.totalResponsesDroppedStale),
            dispatcher.pending,
            static_cast<unsigned long long>(dispatcher.queued),
            static_cast<unsigned long long>(dispatcher.executed),
            static_cast<unsigned long long>(dispatcher.cancelled),
            static_cast<unsigned long long>(dispatcher.stale),
            static_cast<unsigned long long>(dispatcher.rejected));

        Logger::LogInfo(
            "[SPEECH_HEALTH] delta(lines=%llu player_audio=%llu player_text=%llu prepare=%llu ready=%llu audio_fail=%llu "
            "play_start=%llu play_end=%llu play_fail=%llu cancellations=%llu) "
            "lipsync(face_attempt=%llu face_ok=%llu face_fail=%llu mfg_request=%llu mfg_native=%llu mfg_script=%llu) "
            "subtitle(native_attempt=%llu native_ok=%llu native_fail=%llu script=%llu) "
            "guard(attempt=%llu native_ok=%llu native_fail=%llu restores=%llu manager_native=%llu manager_script=%llu) "
            "facing(attempt=%llu ok=%llu fail=%llu)",
            static_cast<unsigned long long>(CounterDelta(speech.dialogueLinesReceived, g_previousSpeechDiagnostics.dialogueLinesReceived)),
            static_cast<unsigned long long>(CounterDelta(speech.playerAudioLinesReceived, g_previousSpeechDiagnostics.playerAudioLinesReceived)),
            static_cast<unsigned long long>(CounterDelta(speech.playerTextOnlyLinesReceived, g_previousSpeechDiagnostics.playerTextOnlyLinesReceived)),
            static_cast<unsigned long long>(CounterDelta(speech.audioPrepareStarted, g_previousSpeechDiagnostics.audioPrepareStarted)),
            static_cast<unsigned long long>(CounterDelta(speech.audioReady, g_previousSpeechDiagnostics.audioReady)),
            static_cast<unsigned long long>(CounterDelta(speech.audioFailed, g_previousSpeechDiagnostics.audioFailed)),
            static_cast<unsigned long long>(CounterDelta(speech.playbackStarted, g_previousSpeechDiagnostics.playbackStarted)),
            static_cast<unsigned long long>(CounterDelta(speech.playbackCompleted, g_previousSpeechDiagnostics.playbackCompleted)),
            static_cast<unsigned long long>(CounterDelta(speech.playbackFailed, g_previousSpeechDiagnostics.playbackFailed)),
            static_cast<unsigned long long>(CounterDelta(speech.dialogueTurnCancellations, g_previousSpeechDiagnostics.dialogueTurnCancellations)),
            static_cast<unsigned long long>(CounterDelta(presentation.faceGenAttempts, g_previousPresentationDiagnostics.faceGenAttempts)),
            static_cast<unsigned long long>(CounterDelta(presentation.faceGenApplied, g_previousPresentationDiagnostics.faceGenApplied)),
            static_cast<unsigned long long>(faceGenFailureDelta),
            static_cast<unsigned long long>(CounterDelta(speech.lipSyncCommandsRequested, g_previousSpeechDiagnostics.lipSyncCommandsRequested)),
            static_cast<unsigned long long>(CounterDelta(speech.nativeMfgApplied, g_previousSpeechDiagnostics.nativeMfgApplied)),
            static_cast<unsigned long long>(scriptMfgFallbackDelta),
            static_cast<unsigned long long>(CounterDelta(presentation.subtitleAttempts, g_previousPresentationDiagnostics.subtitleAttempts)),
            static_cast<unsigned long long>(CounterDelta(presentation.subtitleApplied, g_previousPresentationDiagnostics.subtitleApplied)),
            static_cast<unsigned long long>(CounterDelta(presentation.subtitleFailed, g_previousPresentationDiagnostics.subtitleFailed)),
            static_cast<unsigned long long>(CounterDelta(speech.subtitleScriptFallbacks, g_previousSpeechDiagnostics.subtitleScriptFallbacks)),
            static_cast<unsigned long long>(CounterDelta(presentation.dialogueGuardAttempts, g_previousPresentationDiagnostics.dialogueGuardAttempts)),
            static_cast<unsigned long long>(CounterDelta(presentation.dialogueGuardApplied, g_previousPresentationDiagnostics.dialogueGuardApplied)),
            static_cast<unsigned long long>(CounterDelta(presentation.dialogueGuardFailed, g_previousPresentationDiagnostics.dialogueGuardFailed)),
            static_cast<unsigned long long>(CounterDelta(presentation.dialogueGuardRestores, g_previousPresentationDiagnostics.dialogueGuardRestores)),
            static_cast<unsigned long long>(CounterDelta(speech.nativeDialogueGuards, g_previousSpeechDiagnostics.nativeDialogueGuards)),
            static_cast<unsigned long long>(CounterDelta(speech.scriptDialogueGuards, g_previousSpeechDiagnostics.scriptDialogueGuards)),
            static_cast<unsigned long long>(CounterDelta(presentation.facingAttempts, g_previousPresentationDiagnostics.facingAttempts)),
            static_cast<unsigned long long>(CounterDelta(presentation.facingApplied, g_previousPresentationDiagnostics.facingApplied)),
            static_cast<unsigned long long>(CounterDelta(presentation.facingFailed, g_previousPresentationDiagnostics.facingFailed)));

        if (faceGenFailureDelta >= 20 || scriptMfgFallbackDelta >= 20) {
            Logger::LogWarning(
                "[SPEECH_HEALTH] high lipsync fallback rate speaker='%s' face_fail=%llu script_mfg=%llu window_ms=%lld",
                queue.currentSpeaker.c_str(),
                static_cast<unsigned long long>(faceGenFailureDelta),
                static_cast<unsigned long long>(scriptMfgFallbackDelta),
                windowMs);
        }

        g_previousSpeechDiagnostics = speech;
        g_previousPresentationDiagnostics = presentation;
        g_lastSpeechHealthLog = now;
    }

    if (g_lastHealthLog.time_since_epoch().count() == 0 ||
        now - g_lastHealthLog >= std::chrono::seconds(30)) {
        const auto dispatcher = GameThreadDispatcher::GetStatus();
        const auto taskSnapshot = TaskManager::GetSnapshot();
        const auto& tasks = taskSnapshot.totals;
        const auto nav = SpatialPathProviderFNV::GetNativeGraphStatus();
        const auto spatial = SpatialSnapshotManagerFNV::GetStatus();
        const auto process = CaptureProcessHealth();
        g_taskPendingHighWater = (std::max)(g_taskPendingHighWater, tasks.pending);
        g_dispatchPendingHighWater = (std::max)(g_dispatchPendingHighWater, dispatcher.pending);
        Logger::LogInfo(
            "[NATIVE_RUNTIME] generation=%llu frame=%llu actors=%zu refs=%zu events=%zu "
            "nav(cell=0x%08X ready=%d building=%d nodes=%zu edges=%zu) "
            "spatial(candidates=%zu next=%zu evals=%llu) "
            "dispatcher(pending=%zu queued=%llu executed=%llu stale=%llu rejected=%llu) "
            "tasks(workers=%zu pending=%zu active=%zu uptime_ms=%llu oldest_ms=%llu queued=%llu completed=%llu cancelled=%llu timeout=%llu errors=%llu rejected=%llu)",
            static_cast<unsigned long long>(RuntimeGeneration::Current()),
            static_cast<unsigned long long>(g_frameSequence.load()),
            RuntimeSnapshot::GetActors().size(),
            RuntimeSnapshot::GetReferences().size(),
            RuntimeEventBus::PendingCount(),
            nav.cellFormId, nav.ready ? 1 : 0, nav.building ? 1 : 0, nav.nodes, nav.edges,
            spatial.candidates, spatial.nextCandidate,
            static_cast<unsigned long long>(spatial.evaluations),
            dispatcher.pending,
            static_cast<unsigned long long>(dispatcher.queued),
            static_cast<unsigned long long>(dispatcher.executed),
            static_cast<unsigned long long>(dispatcher.stale),
            static_cast<unsigned long long>(dispatcher.rejected),
            tasks.workers, tasks.pending, tasks.active,
            static_cast<unsigned long long>(tasks.uptimeMs),
            static_cast<unsigned long long>(tasks.oldestPendingMs),
            static_cast<unsigned long long>(tasks.queued),
            static_cast<unsigned long long>(tasks.completed),
            static_cast<unsigned long long>(tasks.cancelled),
            static_cast<unsigned long long>(tasks.timedOut),
            static_cast<unsigned long long>(tasks.errors),
            static_cast<unsigned long long>(tasks.rejected));
        Logger::LogInfo(
            "[PROCESS_HEALTH] valid=%d working_set_mb=%.1f private_mb=%.1f handles=%lu "
            "high_water(actors=%zu refs=%zu events=%zu tasks=%zu dispatcher=%zu)",
            process.valid ? 1 : 0,
            static_cast<double>(process.workingSetBytes) / (1024.0 * 1024.0),
            static_cast<double>(process.privateBytes) / (1024.0 * 1024.0),
            static_cast<unsigned long>(process.handles),
            g_actorHighWater,
            g_referenceHighWater,
            g_eventHighWater,
            g_taskPendingHighWater,
            g_dispatchPendingHighWater);
        if (!taskSnapshot.types.empty()) {
            std::ostringstream typeSummary;
            for (std::size_t index = 0; index < taskSnapshot.types.size(); ++index) {
                const auto& type = taskSnapshot.types[index];
                if (index > 0) typeSummary << " ";
                typeSummary << type.type << "[" << TaskManager::LaneName(type.lane)
                    << ":p=" << type.pending << ",a=" << type.active
                    << ",q=" << type.queued << ",c=" << type.completed
                    << ",x=" << type.cancelled << ",t=" << type.timedOut
                    << ",e=" << type.errors << ",avg=" << type.averageDurationMs
                    << ",max=" << type.maximumDurationMs << ",age=" << type.oldestPendingMs << "]";
            }
            Logger::LogInfo("[TASK_HEALTH] types %s", typeSummary.str().c_str());
        }
        std::ostringstream activeWorkers;
        for (const auto& worker : taskSnapshot.workers) {
            if (!worker.active) continue;
            if (activeWorkers.tellp() > 0) activeWorkers << " ";
            activeWorkers << "#" << worker.index << "=" << worker.type
                << "(" << worker.key << "," << worker.runningMs << "ms)";
        }
        if (activeWorkers.tellp() > 0) {
            Logger::LogInfo("[TASK_HEALTH] active %s", activeWorkers.str().c_str());
        }
        const VoiceRecorder::ServiceStatus voice = VoiceRecorder::GetServiceStatus();
        Logger::LogInfo(
            "[TASK_HEALTH] voice recording=%d open_mic=%d muted=%d record_worker=%d monitor_worker=%d "
            "heartbeat_age_ms=%llu captures=%llu/%llu monitors=%llu/%llu",
            voice.recording ? 1 : 0,
            voice.openMicMonitoring ? 1 : 0,
            voice.openMicMuted ? 1 : 0,
            voice.recordWorkerActive ? 1 : 0,
            voice.openMicWorkerActive ? 1 : 0,
            static_cast<unsigned long long>(voice.heartbeatAgeMs),
            static_cast<unsigned long long>(voice.captureWorkersCompleted),
            static_cast<unsigned long long>(voice.captureWorkersStarted),
            static_cast<unsigned long long>(voice.monitorWorkersCompleted),
            static_cast<unsigned long long>(voice.monitorWorkersStarted));
        const auto comparisons = NativeComparisonTelemetry::Snapshot();
        if (!comparisons.empty()) {
            std::ostringstream summary;
            for (std::size_t index = 0; index < comparisons.size(); ++index) {
                if (index > 0) summary << " ";
                summary << comparisons[index].domain
                        << "(m=" << comparisons[index].matches
                        << ",x=" << comparisons[index].mismatches
                        << ",u=" << comparisons[index].unavailable << ")";
            }
            Logger::LogInfo("[NATIVE_COMPARE] summary %s", summary.str().c_str());
        }
        g_lastHealthLog = now;
    }
}

void OnMessage(const XNVSEAdapter::Message& message) {
    using Event = XNVSEAdapter::LifecycleEvent;
    if (message.event == Event::MainGameLoop) {
        if (!g_nativeFramePumpLogged.exchange(true)) {
            Logger::LogInfo("[NATIVE_RUNTIME] xNVSE main-game-loop pump authoritative; legacy update thread disabled");
        }
        CaptureFrame();
        GameThreadDispatcher::Pump(RuntimeGeneration::Current());
        static auto lastFrame = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        float deltaTime = std::chrono::duration<float>(now - lastFrame).count();
        lastFrame = now;
        if (!std::isfinite(deltaTime) || deltaTime <= 0.0f || deltaTime > 1.0f) {
            deltaTime = 1.0f / 60.0f;
        }
        Dialectic_UpdateFrame(deltaTime);
        return;
    }

    if (InvalidatesGeneration(message.event)) {
        ResetNativeState(message.event == Event::PreLoadGame ? "pre_load_game" :
            message.event == Event::LoadGame ? "load_game" :
            message.event == Event::NewGame ? "new_game" :
            message.event == Event::ExitToMainMenu ? "exit_to_main_menu" : "exit_game");
    }

    if (message.event == Event::ReloadConfig) {
        Config::Load();
    }

    // Reference attach/detach notifications can fire thousands of times while
    // actors rebuild 3D. Do not queue lifecycle events that GameLoop ignores.
    if (!HasRuntimeEventConsumer(message.event)) {
        return;
    }

    RuntimeEventBus::Event event;
    event.type = MapEventType(message.event);
    event.generation = RuntimeGeneration::Current();
    event.formId = message.formId;
    event.text = message.text;
    RuntimeEventBus::Publish(std::move(event));
}

} // namespace

bool Initialize(const void* nvseInterface, std::uint32_t pluginHandle) {
    GameThreadDispatcher::Initialize();
    TaskManager::Initialize(8, 512);
    const bool initialized = XNVSEAdapter::Initialize(nvseInterface, pluginHandle, OnMessage);
    g_available.store(initialized, std::memory_order_release);
    Logger::LogInfo("FNVRuntime: native lifecycle %s", initialized ? "enabled" : "unavailable");
    return initialized;
}

void Shutdown() {
    g_available.store(false, std::memory_order_release);
    XNVSEAdapter::Shutdown();
    TaskManager::Shutdown();
    GameThreadDispatcher::Shutdown();
    RuntimeEventBus::Clear();
    RuntimeSnapshot::Clear();
}

bool IsAvailable() {
    return g_available.load(std::memory_order_acquire);
}

void PumpLegacyFrameFallback() {
    if (IsAvailable()) {
        return;
    }
    GameThreadDispatcher::Pump(RuntimeGeneration::Current());
}

} // namespace FNVRuntime
