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
#include <array>
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

constexpr std::uint64_t kDetailedHitchThresholdUs = 30000;
constexpr auto kHitchDetailRateLimit = std::chrono::milliseconds(250);
constexpr auto kHitchSummaryInterval = std::chrono::seconds(10);

struct BridgeTickState {
    std::uint64_t windowTicks{0};
    std::uint64_t totalTicks{0};
    std::chrono::steady_clock::time_point lastTick;
};

static constexpr std::array<const char*, 18> kBridgeNames = {
    "unknown",
    "action",
    "actor_snapshot",
    "dialogue_capture",
    "trade",
    "notification",
    "halt",
    "text_input",
    "spatial_scan",
    "world_context",
    "activity",
    "nearby_items",
    "nearby_poi",
    "nearby_furniture",
    "active_quest",
    "voice_upload",
    "rpg_event",
    "actor_snapshot_collect"
};

std::array<BridgeTickState, kBridgeNames.size()> g_bridgeTicks;
std::chrono::steady_clock::time_point g_lastMainLoopCallbackAt;
std::chrono::steady_clock::time_point g_lastHitchAt;
std::chrono::steady_clock::time_point g_lastHitchDetailAt;
std::chrono::steady_clock::time_point g_lastHitchSummaryAt;
std::uint64_t g_previousPluginWorkUs = 0;
std::uint64_t g_cadenceFrames = 0;
std::uint64_t g_cadenceGapSamples = 0;
std::uint64_t g_cadenceTotalGapUs = 0;
std::uint64_t g_cadenceMaxGapUs = 0;
std::uint64_t g_cadenceTotalPluginWorkUs = 0;
std::uint64_t g_cadenceMaxPluginWorkUs = 0;
std::uint64_t g_cadenceGaps25 = 0;
std::uint64_t g_cadenceGaps50 = 0;
std::uint64_t g_cadenceGaps100 = 0;
std::uint64_t g_cadenceGaps250 = 0;
std::uint64_t g_cadenceDetailsSuppressed = 0;

struct ProcessHealth {
    std::uint64_t workingSetBytes{0};
    std::uint64_t privateBytes{0};
    std::uint64_t kernelTime100ns{0};
    std::uint64_t userTime100ns{0};
    std::uint64_t readBytes{0};
    std::uint64_t writeBytes{0};
    std::uint64_t pageFaults{0};
    DWORD handles{0};
    bool valid{false};
};

ProcessHealth g_previousProcessHealth;
Logger::Diagnostics g_previousLoggerDiagnostics;
std::chrono::steady_clock::time_point g_lastProcessHealthSampleAt;

ProcessHealth CaptureProcessHealth() {
    ProcessHealth health;
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) {
        health.workingSetBytes = counters.WorkingSetSize;
        health.privateBytes = counters.PrivateUsage;
        health.pageFaults = counters.PageFaultCount;
        health.valid = true;
    }
    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        ULARGE_INTEGER kernelValue{};
        kernelValue.LowPart = kernel.dwLowDateTime;
        kernelValue.HighPart = kernel.dwHighDateTime;
        ULARGE_INTEGER userValue{};
        userValue.LowPart = user.dwLowDateTime;
        userValue.HighPart = user.dwHighDateTime;
        health.kernelTime100ns = kernelValue.QuadPart;
        health.userTime100ns = userValue.QuadPart;
    }
    IO_COUNTERS io{};
    if (GetProcessIoCounters(GetCurrentProcess(), &io)) {
        health.readBytes = io.ReadTransferCount;
        health.writeBytes = io.WriteTransferCount;
    }
    GetProcessHandleCount(GetCurrentProcess(), &health.handles);
    return health;
}

std::string BuildRecentBridgeSummary(std::chrono::steady_clock::time_point now) {
    std::ostringstream summary;
    for (std::size_t index = 1; index < g_bridgeTicks.size(); ++index) {
        const auto& tick = g_bridgeTicks[index];
        if (tick.lastTick.time_since_epoch().count() == 0) continue;
        const auto ageMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - tick.lastTick).count();
        if (ageMs < 0 || ageMs > 750) continue;
        if (summary.tellp() > 0) summary << ",";
        summary << kBridgeNames[index] << ":" << ageMs << "ms";
    }
    return summary.tellp() > 0 ? summary.str() : "none";
}

std::string BuildBridgeWindowSummary() {
    std::ostringstream summary;
    for (std::size_t index = 1; index < g_bridgeTicks.size(); ++index) {
        const auto ticks = g_bridgeTicks[index].windowTicks;
        if (ticks == 0) continue;
        if (summary.tellp() > 0) summary << ",";
        summary << kBridgeNames[index] << ":" << ticks;
    }
    return summary.tellp() > 0 ? summary.str() : "none";
}

void ResetHitchTelemetry() {
    g_lastMainLoopCallbackAt = {};
    g_lastHitchAt = {};
    g_lastHitchDetailAt = {};
    g_lastHitchSummaryAt = {};
    g_previousPluginWorkUs = 0;
    g_cadenceFrames = 0;
    g_cadenceGapSamples = 0;
    g_cadenceTotalGapUs = 0;
    g_cadenceMaxGapUs = 0;
    g_cadenceTotalPluginWorkUs = 0;
    g_cadenceMaxPluginWorkUs = 0;
    g_cadenceGaps25 = 0;
    g_cadenceGaps50 = 0;
    g_cadenceGaps100 = 0;
    g_cadenceGaps250 = 0;
    g_cadenceDetailsSuppressed = 0;
    for (auto& tick : g_bridgeTicks) {
        tick.windowTicks = 0;
        tick.lastTick = {};
    }
}

void RecordFrameCadence(std::chrono::steady_clock::time_point callbackStartedAt,
                        std::uint64_t gapUs,
                        std::uint64_t pluginWorkUs) {
    const auto now = std::chrono::steady_clock::now();
    if (g_lastHitchSummaryAt.time_since_epoch().count() == 0) {
        g_lastHitchSummaryAt = callbackStartedAt;
    }

    ++g_cadenceFrames;
    g_cadenceTotalPluginWorkUs += pluginWorkUs;
    g_cadenceMaxPluginWorkUs = (std::max)(g_cadenceMaxPluginWorkUs, pluginWorkUs);
    if (gapUs > 0) {
        ++g_cadenceGapSamples;
        g_cadenceTotalGapUs += gapUs;
        g_cadenceMaxGapUs = (std::max)(g_cadenceMaxGapUs, gapUs);
        if (gapUs >= 25000) ++g_cadenceGaps25;
        if (gapUs >= 50000) ++g_cadenceGaps50;
        if (gapUs >= 100000) ++g_cadenceGaps100;
        if (gapUs >= 250000) ++g_cadenceGaps250;
    }

    if (gapUs >= kDetailedHitchThresholdUs && gapUs <= 5000000) {
        const auto sincePreviousHitchMs = g_lastHitchAt.time_since_epoch().count() == 0
            ? -1LL
            : std::chrono::duration_cast<std::chrono::milliseconds>(callbackStartedAt - g_lastHitchAt).count();
        g_lastHitchAt = callbackStartedAt;
        if (g_lastHitchDetailAt.time_since_epoch().count() == 0 ||
            callbackStartedAt - g_lastHitchDetailAt >= kHitchDetailRateLimit) {
            const auto state = RuntimeSnapshot::GetGameState();
            const auto queue = SpeakManager::GetQueueStatus();
            const auto dispatcher = GameThreadDispatcher::GetStatus();
            const auto taskSnapshot = TaskManager::GetSnapshot();
            const auto outsideCallbackUs = gapUs > g_previousPluginWorkUs
                ? gapUs - g_previousPluginWorkUs
                : 0;
            const std::string bridges = BuildRecentBridgeSummary(now);
            Logger::LogInfo(
                "[HITCH] gap_ms=%.3f since_previous_hitch_ms=%lld outside_callback_ms=%.3f "
                "previous_plugin_work_ms=%.3f current_plugin_work_ms=%.3f frame=%llu generation=%llu "
                "state(in_game=%d menu=%d paused=%d loading=%d dialogue=%d combat=%d cell=0x%08X world=0x%08X) "
                "queue(lines=%d downloads=%d prepared=%d playing=%d) tasks(pending=%zu active=%zu oldest_ms=%llu) "
                "dispatcher(pending=%zu) recent_bridges=%s",
                static_cast<double>(gapUs) / 1000.0,
                sincePreviousHitchMs,
                static_cast<double>(outsideCallbackUs) / 1000.0,
                static_cast<double>(g_previousPluginWorkUs) / 1000.0,
                static_cast<double>(pluginWorkUs) / 1000.0,
                static_cast<unsigned long long>(g_frameSequence.load()),
                static_cast<unsigned long long>(RuntimeGeneration::Current()),
                state.inGame ? 1 : 0,
                state.inMenu ? 1 : 0,
                state.paused ? 1 : 0,
                state.loadingMenuOpen ? 1 : 0,
                state.dialogueMenuOpen ? 1 : 0,
                state.inCombat ? 1 : 0,
                state.cellFormId,
                state.worldspaceFormId,
                queue.dialogueLinesQueued,
                queue.ttsDownloadsInProgress,
                queue.preparedAudioCount,
                queue.isPlaying ? 1 : 0,
                taskSnapshot.totals.pending,
                taskSnapshot.totals.active,
                static_cast<unsigned long long>(taskSnapshot.totals.oldestPendingMs),
                dispatcher.pending,
                bridges.c_str());
            g_lastHitchDetailAt = callbackStartedAt;
        } else {
            ++g_cadenceDetailsSuppressed;
        }
    }

    g_previousPluginWorkUs = pluginWorkUs;

    if (now - g_lastHitchSummaryAt >= kHitchSummaryInterval) {
        const auto windowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - g_lastHitchSummaryAt).count();
        const double averageGapMs = g_cadenceGapSamples == 0
            ? 0.0
            : static_cast<double>(g_cadenceTotalGapUs) / static_cast<double>(g_cadenceGapSamples) / 1000.0;
        const double averagePluginWorkMs = g_cadenceFrames == 0
            ? 0.0
            : static_cast<double>(g_cadenceTotalPluginWorkUs) / static_cast<double>(g_cadenceFrames) / 1000.0;
        const std::string bridges = BuildBridgeWindowSummary();
        Logger::LogInfo(
            "[HITCH_SUMMARY] window_ms=%lld frames=%llu avg_gap_ms=%.3f max_gap_ms=%.3f "
            "gaps_25=%llu gaps_50=%llu gaps_100=%llu gaps_250=%llu "
            "plugin_work(avg_ms=%.3f max_ms=%.3f) detail_suppressed=%llu bridge_ticks=%s",
            windowMs,
            static_cast<unsigned long long>(g_cadenceFrames),
            averageGapMs,
            static_cast<double>(g_cadenceMaxGapUs) / 1000.0,
            static_cast<unsigned long long>(g_cadenceGaps25),
            static_cast<unsigned long long>(g_cadenceGaps50),
            static_cast<unsigned long long>(g_cadenceGaps100),
            static_cast<unsigned long long>(g_cadenceGaps250),
            averagePluginWorkMs,
            static_cast<double>(g_cadenceMaxPluginWorkUs) / 1000.0,
            static_cast<unsigned long long>(g_cadenceDetailsSuppressed),
            bridges.c_str());
        if (g_bridgeTicks[1].windowTicks > 300) {
            Logger::LogWarning(
                "[BRIDGE_STORM] name=action ticks=%llu window_ms=%lld expected_max=300; duplicate ActionCommandTick chains are active",
                static_cast<unsigned long long>(g_bridgeTicks[1].windowTicks),
                windowMs);
        }
        if (g_bridgeTicks[17].windowTicks > 8) {
            Logger::LogWarning(
                "[BRIDGE_STORM] name=actor_snapshot_collect ticks=%llu window_ms=%lld expected_max=8; actor snapshots are being collected without one-shot request acknowledgement",
                static_cast<unsigned long long>(g_bridgeTicks[17].windowTicks),
                windowMs);
        }
        g_cadenceFrames = 0;
        g_cadenceGapSamples = 0;
        g_cadenceTotalGapUs = 0;
        g_cadenceMaxGapUs = 0;
        g_cadenceTotalPluginWorkUs = 0;
        g_cadenceMaxPluginWorkUs = 0;
        g_cadenceGaps25 = 0;
        g_cadenceGaps50 = 0;
        g_cadenceGaps100 = 0;
        g_cadenceGaps250 = 0;
        g_cadenceDetailsSuppressed = 0;
        for (auto& tick : g_bridgeTicks) tick.windowTicks = 0;
        g_lastHitchSummaryAt = now;
    }
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
    g_previousProcessHealth = CaptureProcessHealth();
    g_previousLoggerDiagnostics = Logger::GetDiagnostics();
    g_lastProcessHealthSampleAt = std::chrono::steady_clock::now();
    ResetHitchTelemetry();
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
        const auto loggerDiagnostics = Logger::GetDiagnostics();
        const auto processWindowMs = g_lastProcessHealthSampleAt.time_since_epoch().count() == 0
            ? 0LL
            : std::chrono::duration_cast<std::chrono::milliseconds>(now - g_lastProcessHealthSampleAt).count();
        const auto cpuTimeDelta100ns = CounterDelta(
            process.kernelTime100ns + process.userTime100ns,
            g_previousProcessHealth.kernelTime100ns + g_previousProcessHealth.userTime100ns);
        const double cpuTimeDeltaMs = static_cast<double>(cpuTimeDelta100ns) / 10000.0;
        const double cpuPercentOneCore = processWindowMs > 0
            ? cpuTimeDeltaMs * 100.0 / static_cast<double>(processWindowMs)
            : 0.0;
        const auto readBytesDelta = CounterDelta(process.readBytes, g_previousProcessHealth.readBytes);
        const auto writeBytesDelta = CounterDelta(process.writeBytes, g_previousProcessHealth.writeBytes);
        const auto pageFaultDelta = CounterDelta(process.pageFaults, g_previousProcessHealth.pageFaults);
        const auto loggerLinesDelta = CounterDelta(loggerDiagnostics.linesWritten, g_previousLoggerDiagnostics.linesWritten);
        const auto loggerBytesDelta = CounterDelta(loggerDiagnostics.bytesWritten, g_previousLoggerDiagnostics.bytesWritten);
        const auto loggerFlushDelta = CounterDelta(loggerDiagnostics.flushes, g_previousLoggerDiagnostics.flushes);
        const auto loggerSlowDelta = CounterDelta(loggerDiagnostics.slowWrites, g_previousLoggerDiagnostics.slowWrites);
        const auto loggerWriteUsDelta = CounterDelta(loggerDiagnostics.totalWriteUs, g_previousLoggerDiagnostics.totalWriteUs);
        const auto loggerLockUsDelta = CounterDelta(loggerDiagnostics.totalLockWaitUs, g_previousLoggerDiagnostics.totalLockWaitUs);
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
            "[PROCESS_HEALTH] window_ms=%lld valid=%d working_set_mb=%.1f private_mb=%.1f handles=%lu "
            "cpu_ms=%.1f cpu_pct_one_core=%.1f page_faults=%llu io(read_mb=%.3f write_mb=%.3f) "
            "logger(lines=%llu bytes=%llu flushes=%llu write_ms=%.3f lock_wait_ms=%.3f slow=%llu max_write_ms=%.3f max_lock_ms=%.3f) "
            "high_water(actors=%zu refs=%zu events=%zu tasks=%zu dispatcher=%zu)",
            processWindowMs,
            process.valid ? 1 : 0,
            static_cast<double>(process.workingSetBytes) / (1024.0 * 1024.0),
            static_cast<double>(process.privateBytes) / (1024.0 * 1024.0),
            static_cast<unsigned long>(process.handles),
            cpuTimeDeltaMs,
            cpuPercentOneCore,
            static_cast<unsigned long long>(pageFaultDelta),
            static_cast<double>(readBytesDelta) / (1024.0 * 1024.0),
            static_cast<double>(writeBytesDelta) / (1024.0 * 1024.0),
            static_cast<unsigned long long>(loggerLinesDelta),
            static_cast<unsigned long long>(loggerBytesDelta),
            static_cast<unsigned long long>(loggerFlushDelta),
            static_cast<double>(loggerWriteUsDelta) / 1000.0,
            static_cast<double>(loggerLockUsDelta) / 1000.0,
            static_cast<unsigned long long>(loggerSlowDelta),
            static_cast<double>(loggerDiagnostics.maxWriteUs) / 1000.0,
            static_cast<double>(loggerDiagnostics.maxLockWaitUs) / 1000.0,
            g_actorHighWater,
            g_referenceHighWater,
            g_eventHighWater,
            g_taskPendingHighWater,
            g_dispatchPendingHighWater);
        g_previousProcessHealth = process;
        g_previousLoggerDiagnostics = loggerDiagnostics;
        g_lastProcessHealthSampleAt = now;
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
        const auto callbackStartedAt = std::chrono::steady_clock::now();
        const std::uint64_t gapUs = g_lastMainLoopCallbackAt.time_since_epoch().count() == 0
            ? 0
            : static_cast<std::uint64_t>((std::max)(0LL,
                std::chrono::duration_cast<std::chrono::microseconds>(
                    callbackStartedAt - g_lastMainLoopCallbackAt).count()));
        g_lastMainLoopCallbackAt = callbackStartedAt;
        if (!g_nativeFramePumpLogged.exchange(true)) {
            Logger::LogInfo("[NATIVE_RUNTIME] xNVSE main-game-loop pump authoritative; legacy update thread disabled");
        }
        CaptureFrame();
        GameThreadDispatcher::Pump(RuntimeGeneration::Current());
        float deltaTime = gapUs > 0
            ? static_cast<float>(static_cast<double>(gapUs) / 1000000.0)
            : 1.0f / 60.0f;
        if (!std::isfinite(deltaTime) || deltaTime <= 0.0f || deltaTime > 1.0f) {
            deltaTime = 1.0f / 60.0f;
        }
        Dialectic_UpdateFrame(deltaTime);
        const auto pluginWorkElapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - callbackStartedAt).count();
        RecordFrameCadence(callbackStartedAt, gapUs,
            pluginWorkElapsed > 0 ? static_cast<std::uint64_t>(pluginWorkElapsed) : 0);
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
    g_previousProcessHealth = CaptureProcessHealth();
    g_previousLoggerDiagnostics = Logger::GetDiagnostics();
    g_lastProcessHealthSampleAt = std::chrono::steady_clock::now();
    ResetHitchTelemetry();
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

void RecordScriptBridgeTick(std::uint32_t bridgeId) {
    if (bridgeId == 0 || bridgeId >= g_bridgeTicks.size()) {
        return;
    }
    auto& tick = g_bridgeTicks[bridgeId];
    ++tick.windowTicks;
    ++tick.totalTicks;
    tick.lastTick = std::chrono::steady_clock::now();
}

void PumpLegacyFrameFallback() {
    if (IsAvailable()) {
        return;
    }
    GameThreadDispatcher::Pump(RuntimeGeneration::Current());
}

} // namespace FNVRuntime
