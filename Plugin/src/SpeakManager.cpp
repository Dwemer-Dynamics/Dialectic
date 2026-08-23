#include "SpeakManager.h"
#include "ActionManager.h"
#include "AudioManager.h"
#include "AgentManager.h"
#include "ActorEligibilityFNV.h"
#include "ActivityStatusFNV.h"
#include "ActorPositionResolverFNV.h"
#include "GameLoop.h"
#include "GameThreadDispatcher.h"
#include "HeadVoiceVolumeUtils.h"
#include "HTTPManager.h"
#include "Config.h"
#include "Misc.h"
#include "SpatialAwarenessFNV.h"
#include "TargetManager.h"
#include "WorldContextFNV.h"
#include "RuntimeGeneration.h"
#include "RuntimeSnapshot.h"
#include "TaskManager.h"
#include "XNVSEAdapter.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>

#include <thread>
#include <chrono>
#include <queue>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <string>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>
#include <atomic>
#include <functional>
#include <set>
#include <utility>

#pragma comment(lib, "winhttp.lib")

// Forward declare Log from main.cpp
void Log(const char* fmt, ...);

namespace SpeakManager {

    // Script line structure for dialogue queue
    struct ScriptLine {
        std::string text;
        std::string actor;
        std::string displayName;
        std::string action;
        uint32_t actorFormId = 0;
        bool isFinalResponseLine = false;
        std::string listenerHint;
        std::string rechatTargetHint;
        uint32_t listenerFormId = 0;
        uint32_t rechatTargetFormId = 0;
        int rechatDepth = 0;
        std::string ttsCacheKey;
        std::string utteranceId;
        std::string requestId;
        std::string sceneKey;
        uint64_t runtimeGeneration = 0;
        bool rechatLaunched = false;
        bool textOnlyFallback = false;
        uint64_t sequence = 0;
    };

    // Pending audio download result
    struct PendingAudio {
        std::vector<uint8_t> audioData;
        std::string speaker;
        uint32_t actorFormId = 0;
        ScriptLine line;
        bool ready;
        bool textOnlyFallback = false;
    };

    static std::queue<ScriptLine> g_scriptQueue;
    static std::mutex g_queueMutex;
    static bool g_isProcessing = false;
    static bool g_aborted = false;
    static std::vector<PendingAudio> g_pendingAudioQueue;
    static int g_downloadsInProgress = 0;
    static const int MAX_PREFETCHED_AUDIO = 2;
    static std::atomic<uint64_t> g_audioGeneration{0};
    static uint64_t g_nextPriorityLineSequence = 1;
    static uint64_t g_nextLineSequence = 1000000;
    static std::set<uint64_t> g_pendingLineSequences;
    static std::mutex g_pendingMutex;
    static std::mutex g_playerTtsGateMutex;
    static bool g_playerInputTtsGateActive = false;
    static std::chrono::steady_clock::time_point g_playerInputTtsGateUntil = {};
    static constexpr auto kPlayerInputTtsGateTimeout = std::chrono::seconds(5);
    static constexpr int kNpcTtsMaxDownloadAttempts = 50;
    static constexpr auto kNpcTtsPreparationTimeout = std::chrono::seconds(20);
    static uint32_t g_currentSpeakerFormId = 0;
    static std::string g_currentSpeaker;
    static ScriptLine g_currentPlaybackLine;
    static bool g_currentPlaybackLineActive = false;
    static bool g_currentPlaybackRechatLaunched = false;
    static bool g_currentPlaybackIsHeadVoice = false;
    static bool g_faceTargetBridgeActive = false;
static uint32_t g_faceTargetSpeakerFormId = 0;
static uint32_t g_faceTargetTargetFormId = 0;
    static std::string g_lastDialogueGuardValue;
    static std::chrono::steady_clock::time_point g_lastDialogueGuardStatusRead = {};
    static std::chrono::steady_clock::time_point g_dialogueGuardHoldUntil = {};
    static constexpr auto kVanillaDialogueGuardTail = std::chrono::seconds(5);
    static std::string g_lastSpatialLogKey;
    static std::chrono::steady_clock::time_point g_lastSpatialLogTime = {};
    static std::chrono::steady_clock::time_point g_lastSpatialPlaybackUpdateTime = {};
    static uint32_t g_lastSpatialPlaybackSpeakerFormId = 0;
    static constexpr auto kSpatialPlaybackUpdateInterval = std::chrono::milliseconds(150);
    static std::chrono::steady_clock::time_point g_lastQueueStatusLogTime = {};
    static std::chrono::steady_clock::time_point g_lastQueueStatusWriteTime = {};
    static bool g_playbackPausedForMenu = false;
    static std::string g_lastPlayerSceneKey;
    static std::atomic<uint64_t> g_dialogueLinesReceived{0};
    static std::atomic<uint64_t> g_playerAudioLinesReceived{0};
    static std::atomic<uint64_t> g_playerTextOnlyLinesReceived{0};
    static std::atomic<uint64_t> g_audioPrepareStarted{0};
    static std::atomic<uint64_t> g_audioReady{0};
    static std::atomic<uint64_t> g_audioFailed{0};
    static std::atomic<uint64_t> g_playbackStarted{0};
    static std::atomic<uint64_t> g_playbackCompleted{0};
    static std::atomic<uint64_t> g_playbackFailed{0};
    static std::atomic<uint64_t> g_lipSyncCommandsRequested{0};
    static std::atomic<uint64_t> g_nativeMfgApplied{0};
    static std::atomic<uint64_t> g_scriptMfgFallbacks{0};
    static std::atomic<uint64_t> g_subtitleScriptFallbacks{0};
    static std::atomic<uint64_t> g_nativeDialogueGuards{0};
    static std::atomic<uint64_t> g_scriptDialogueGuards{0};
    static std::atomic<uint64_t> g_dialogueTurnCancellations{0};
    static void SendDeliveryState(const ScriptLine& line, const std::string& state) {
        if (line.actor.empty() || line.text.empty()) {
            return;
        }
        Log("SpeakManager: utterance state=%s speaker='%s' utterance='%s' cache='%s'",
            state.c_str(),
            line.actor.c_str(),
            line.utteranceId.c_str(),
            line.ttsCacheKey.c_str());
        HTTPManager::SendDialogueDeliveryAck(
            line.actor,
            line.actorFormId,
            line.text,
            line.ttsCacheKey,
            line.utteranceId,
            state,
            line.requestId);
    }

    static std::string PreviewText(const std::string& text) {
        std::string preview = text;
        const char* whitespace = " \t\r\n";
        const size_t start = preview.find_first_not_of(whitespace);
        if (start == std::string::npos) {
            return "";
        }
        const size_t end = preview.find_last_not_of(whitespace);
        preview = preview.substr(start, end - start + 1);
        if (preview.size() > 80) {
            preview = preview.substr(0, 77) + "...";
        }
        return preview;
    }
    struct PendingRechatRetry {
        bool active = false;
        std::string speaker;
        std::string listenerHint;
        std::string explicitTarget;
        std::string originLine;
        int rechatDepth = 0;
        uint32_t speakerFormId = 0;
        uint32_t listenerFormId = 0;
        uint32_t targetFormId = 0;
    };

    static std::mutex g_rechatMutex;
    static bool g_rechatInFlight = false;
    static std::string g_rechatInFlightSpeaker;
    static PendingRechatRetry g_pendingRechatRetry;
    static std::string g_lastRechatter;
    static bool g_rechatChainClosed = false;
    static bool g_rechatChainAutonomous = false;
    static std::string g_rechatChainId;
    static std::chrono::steady_clock::time_point g_rechatCooldownUntil = {};
    static ScriptLine g_pendingRechatLaunchLine;
    static bool g_pendingRechatLaunchActive = false;
    static std::chrono::steady_clock::time_point g_pendingRechatLaunchUntil = {};
    static std::chrono::steady_clock::time_point g_pendingRechatNextCheck = {};
    static constexpr int kRechatActorFreshnessMs = 10000;
    static constexpr int kDialogueActorFreshnessMs = 8000;
    static constexpr int kDialogueActorLatestScanMs = 8000;

    static const char* kSubtitleBridgePath = "Data\\NVSE\\Plugins\\dialectic_subtitle.txt";
    static const char* kLipSyncStatusPath = "Data\\NVSE\\Plugins\\dialectic_lipsync_status.txt";
    static const char* kDialogueGuardRefPath = "Data\\NVSE\\Plugins\\dialectic_dialogue_guard_ref.txt";
    static const char* kDialogueGuardModPath = "Data\\NVSE\\Plugins\\dialectic_dialogue_guard_mod.txt";
    static const char* kDialogueGuardLocalPath = "Data\\NVSE\\Plugins\\dialectic_dialogue_guard_local.txt";
    static const char* kDialogueGuardStatusPath = "Data\\NVSE\\Plugins\\dialectic_dialogue_guard_status.txt";
    static const char* kQueueStatusPath = "Data\\NVSE\\Plugins\\dialectic_queue_status.tmp";
    static const char* kRechatStatusPath = "Data\\NVSE\\Plugins\\dialectic_rechat_status.tmp";

    struct PassiveSubtitleSegment {
        double startSeconds = 0.0;
        double endSeconds = 0.0;
        std::string text;
    };

    static bool g_subtitleActive = false;
    // Treat startup as potentially dirty so a stale subtitle from a previous
    // process is cleared the first time native presentation succeeds.
    static bool g_subtitleBridgeFallbackActive = true;
    static std::vector<PassiveSubtitleSegment> g_subtitleSegments;
    static size_t g_subtitleSegmentIndex = 0;
    static std::string g_subtitleUtteranceId;
    static std::string g_subtitleLastText;
    static bool g_playerTextOnlySubtitleActive = false;
    static std::chrono::steady_clock::time_point g_playerTextOnlySubtitleUntil = {};
    static constexpr auto kPlayerTextOnlySubtitleDuration = std::chrono::seconds(2);
    static bool g_npcTextOnlyFallbackActive = false;
    static std::chrono::steady_clock::time_point g_npcTextOnlyFallbackUntil = {};
    static std::mutex g_recentSubtitleMutex;
    static std::vector<std::pair<std::string, std::chrono::steady_clock::time_point>> g_recentAiSubtitleKeys;
    static constexpr auto kRecentAiSubtitleWindow = std::chrono::seconds(20);

    static float g_preClipMs = 100.0f;
    static float g_postClipMs = 100.0f;
    static int g_animationResolution = 500;
    static float g_animationIntensity = 1.0f;
    static std::string g_lastDialogueGuardStatus;

    static std::string Trim(const std::string& value) {
        const char* whitespace = " \t\r\n";
        const size_t start = value.find_first_not_of(whitespace);
        if (start == std::string::npos) {
            return "";
        }
        const size_t end = value.find_last_not_of(whitespace);
        return value.substr(start, end - start + 1);
    }

    static std::string NormalizeName(const std::string& value) {
        std::string normalized = Trim(value);
        std::transform(normalized.begin(), normalized.end(), normalized.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return normalized;
    }

    static bool EqualsIgnoreCase(const std::string& left, const std::string& right) {
        return NormalizeName(left) == NormalizeName(right);
    }

    static float DistanceBetween(const ActorPositionResolverFNV::Vector3& left,
                                 const ActorPositionResolverFNV::Vector3& right) {
        const float dx = left.x - right.x;
        const float dy = left.y - right.y;
        const float dz = left.z - right.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    static std::string HexFormId(uint32_t formId) {
        if (formId == 0) {
            return "";
        }

        std::ostringstream stream;
        stream << "0x" << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << formId;
        return stream.str();
    }

    static std::string RawHexFormId(uint32_t formId) {
        if (formId == 0) {
            return "";
        }

        std::ostringstream stream;
        stream << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << formId;
        return stream.str();
    }

    static std::string CurrentPlayerSceneKey() {
        const auto world = WorldContextFNV::GetCurrent();
        if (world.resolved) {
            std::ostringstream stream;
            if (world.interiorKnown && world.isInterior && !world.cellFormId.empty()) {
                stream << "cell:" << world.cellFormId << ":interior:1";
                return stream.str();
            }

            if (!world.worldspaceFormId.empty()) {
                stream << "worldspace:" << world.worldspaceFormId;
                if (!world.location.empty() && world.location != "Unknown Location") {
                    stream << ":location:" << NormalizeName(world.location);
                }
                return stream.str();
            }
        }

        const auto player = ActorPositionResolverFNV::ResolvePlayer();
        if (!player.resolved || !player.cellResolved || player.cellFormId == 0) {
            return "";
        }

        std::ostringstream stream;
        stream << "cell:" << HexFormId(player.cellFormId)
               << ":interior:" << ((player.interiorKnown && player.isInterior) ? "1" : "0");
        return stream.str();
    }

    static bool IsHardSceneBoundaryChange(const std::string& oldSceneKey,
                                          const std::string& newSceneKey) {
        if (oldSceneKey.empty() || newSceneKey.empty() || oldSceneKey == newSceneKey) {
            return false;
        }

        const bool oldRawExteriorCell =
            oldSceneKey.rfind("cell:", 0) == 0 &&
            oldSceneKey.find(":interior:0") != std::string::npos;
        const bool newRawExteriorCell =
            newSceneKey.rfind("cell:", 0) == 0 &&
            newSceneKey.find(":interior:0") != std::string::npos;

        // Exterior parent-cell IDs can jitter around loaded worldspace grids in
        // FNV/TTW without a real scene transition. Interior changes remain hard
        // boundaries, while exterior changes use the stable world-context key.
        if (oldRawExteriorCell && newRawExteriorCell) {
            return false;
        }

        return true;
    }

    static bool ShouldGuardVanillaDialogueForLine(const ScriptLine& line) {
        if (!Config::suppressVanillaDialogueDuringAI || line.actorFormId == 0) {
            return false;
        }

        return !EqualsIgnoreCase(line.actor, "Player") &&
            !EqualsIgnoreCase(line.actor, "The Narrator");
    }

    static bool ShouldIncludeAmbientDialogueGuardActor(
        const ActorPositionResolverFNV::PositionResult& position,
        const ActorPositionResolverFNV::PositionResult& player,
        float maxDistance) {
        if (!position.resolved || position.formId == 0 || position.formId == 0x14) {
            return false;
        }

        if ((position.deadKnown && position.isDead) ||
            (position.disabledKnown && position.isDisabled)) {
            return false;
        }

        if (!ActorPositionResolverFNV::IsPositionInPlayerScene(position)) {
            return false;
        }

        if (player.resolved && maxDistance > 0.0f &&
            DistanceBetween(position.position, player.position) > maxDistance) {
            return false;
        }

        return true;
    }

    static std::vector<uint32_t> BuildDialogueGuardActorRefs(const ScriptLine& line) {
        std::vector<uint32_t> refs;
        if (line.actorFormId != 0) {
            refs.push_back(line.actorFormId);
        }
        return refs;
    }

    static bool WriteTextFile(const char* path, const std::string& value) {
        std::ofstream file(path, std::ios::trunc);
        if (!file.is_open()) {
            return false;
        }

        file << value;
        return true;
    }

    static unsigned long long StatusTimestampMs() {
        return static_cast<unsigned long long>(GetTickCount64());
    }

    static void WriteRechatStatus(const std::string& status,
                                  const std::string& speaker,
                                  const std::string& trigger,
                                  const std::string& reason,
                                  const std::string& target = "") {
        const auto& state = GameLoop::GetGameState();
        std::ostringstream out;
        out << "updated_tick_ms=" << StatusTimestampMs() << "\n"
            << "status=" << status << "\n"
            << "speaker=" << speaker << "\n"
            << "trigger=" << trigger << "\n"
            << "target=" << target << "\n"
            << "reason=" << reason << "\n"
            << "in_game=" << (state.isInGame ? 1 : 0) << "\n"
            << "paused=" << (state.isPaused ? 1 : 0) << "\n"
            << "in_menu=" << (state.isInMenu ? 1 : 0) << "\n"
            << "in_dialogue=" << (state.isInDialogue ? 1 : 0) << "\n"
            << "loading=" << (state.isLoading ? 1 : 0) << "\n";
        WriteTextFile(kRechatStatusPath, out.str());
    }

    static void WriteDialogueGuardRef(const std::string& value,
                                      const std::string& modValues = "",
                                      const std::string& localValues = "") {
        if (value == g_lastDialogueGuardValue) {
            return;
        }

        if (!WriteTextFile(kDialogueGuardRefPath, value)) {
            Log("SpeakManager: Failed to write dialogue guard bridge");
            return;
        }

        WriteTextFile(kDialogueGuardModPath, modValues);
        WriteTextFile(kDialogueGuardLocalPath, localValues);
        g_lastDialogueGuardValue = value;
    }

    static std::string ReadFirstLineFromFile(const char* path) {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) {
            return "";
        }

        std::string line;
        std::getline(file, line);
        return Trim(line);
    }

    static void LogDialogueGuardStatusIfChanged(bool force = false) {
        const auto now = std::chrono::steady_clock::now();
        if (!force &&
            g_lastDialogueGuardStatusRead.time_since_epoch().count() != 0 &&
            now - g_lastDialogueGuardStatusRead < std::chrono::milliseconds(500)) {
            return;
        }
        g_lastDialogueGuardStatusRead = now;

        const std::string status = ReadFirstLineFromFile(kDialogueGuardStatusPath);
        if (status.empty()) {
            return;
        }

        if (!force && status == g_lastDialogueGuardStatus) {
            return;
        }

        g_lastDialogueGuardStatus = status;
        Log("SpeakManager: Dialogue guard status: %s", status.c_str());
    }

    static void StartDialogueGuardBridge(const ScriptLine& line) {
        if (!ShouldGuardVanillaDialogueForLine(line)) {
            return;
        }

        const std::vector<uint32_t> guardRefs = BuildDialogueGuardActorRefs(line);
        if (guardRefs.empty()) {
            return;
        }

        g_dialogueGuardHoldUntil = std::chrono::steady_clock::now() + kVanillaDialogueGuardTail;
        std::ostringstream guard;
        std::ostringstream guardMods;
        std::ostringstream guardLocals;
        for (uint32_t ref : guardRefs) {
            guard << RawHexFormId(ref) << "\n";
            guardMods << ((ref >> 24) & 0xFF) << "\n";
            guardLocals << (ref & 0x00FFFFFF) << "\n";
        }
        const std::string guardValues = guard.str();
        const std::string guardModValues = guardMods.str();
        const std::string guardLocalValues = guardLocals.str();
        auto apply = [guardRefs, guardValues, guardModValues, guardLocalValues,
                      actor = line.actor, actorFormId = line.actorFormId]() {
            // Do not switch an already-active script fallback to native midway;
            // its delayed restore could otherwise overwrite the native mute.
            const bool nativeApplied = g_lastDialogueGuardValue.empty() &&
                XNVSEAdapter::UpdateNativeDialogueGuards(guardRefs);
            if (nativeApplied) {
                g_nativeDialogueGuards.fetch_add(1, std::memory_order_relaxed);
                WriteDialogueGuardRef("");
                Log("SpeakManager: Native vanilla dialogue guard active for %s (%s), suppressing %zu loaded actor(s)",
                    actor.c_str(), HexFormId(actorFormId).c_str(), guardRefs.size());
                return;
            }

            g_scriptDialogueGuards.fetch_add(1, std::memory_order_relaxed);
            WriteDialogueGuardRef(guardValues, guardModValues, guardLocalValues);
            Log("SpeakManager: Script fallback dialogue guard active for %s (%s), suppressing %zu loaded actor(s)",
                actor.c_str(), HexFormId(actorFormId).c_str(), guardRefs.size());
            LogDialogueGuardStatusIfChanged();
        };

        if (GameThreadDispatcher::IsGameThread()) {
            apply();
        } else {
            GameThreadDispatcher::Enqueue("dialogue_guard", "dialogue_guard", RuntimeGeneration::Current(),
                std::move(apply), [](const char* reason) {
                    Log("SpeakManager: Native dialogue guard start dropped reason=%s", reason ? reason : "unknown");
                });
        }
    }

    static void ClearDialogueGuardBridge() {
        g_dialogueGuardHoldUntil = {};
        auto clear = []() {
            XNVSEAdapter::RestoreNativeDialogueGuards();
            WriteDialogueGuardRef("");
            LogDialogueGuardStatusIfChanged(true);
        };
        if (GameThreadDispatcher::IsGameThread()) {
            clear();
        } else {
            GameThreadDispatcher::Enqueue("dialogue_guard", "dialogue_guard", RuntimeGeneration::Current(),
                std::move(clear), [](const char* reason) {
                    Log("SpeakManager: Native dialogue guard restore dropped reason=%s", reason ? reason : "unknown");
                });
        }
    }

    static bool HasQueuedDialogueWork() {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        return !g_scriptQueue.empty();
    }

    static bool HasPendingAudioWork() {
        std::lock_guard<std::mutex> pendingLock(g_pendingMutex);
        return !g_pendingAudioQueue.empty() || g_downloadsInProgress > 0 ||
            !g_pendingLineSequences.empty();
    }

    static bool HasDialoguePipelineWork() {
        return g_currentPlaybackLineActive || AudioManager::IsPlaying() ||
            HasQueuedDialogueWork() || HasPendingAudioWork();
    }

    static void ClearDialogueGuardBridgeIfIdle() {
        if (HasDialoguePipelineWork()) {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        if (g_dialogueGuardHoldUntil.time_since_epoch().count() != 0 &&
            now < g_dialogueGuardHoldUntil) {
            return;
        }

        ClearDialogueGuardBridge();
    }

    static bool IsPlayerHint(const std::string& value) {
        const std::string normalized = NormalizeName(value);
        if (normalized.empty()) {
            return false;
        }

        return normalized == "player" ||
            normalized == "courier" ||
            (!Config::playerName.empty() && normalized == NormalizeName(Config::playerName));
    }

    static bool IsPlayerSpeakerName(const std::string& value) {
        return IsPlayerHint(value);
    }

    static std::string PlayerDisplayName() {
        std::string name = Trim(Config::playerName);
        if (name.empty() || EqualsIgnoreCase(name, "Player")) {
            name = Trim(Misc::GetPlayerName());
        }
        return name.empty() ? "Player" : name;
    }

    static bool IsPlayerTtsListenerHint(const std::string& value) {
        const std::string normalized = NormalizeName(value);
        return normalized == "__player_menu_tts" ||
            normalized == "__player_tts" ||
            normalized == "player_tts";
    }

    static bool IsPlayerTtsLine(const ScriptLine& line) {
        return IsPlayerSpeakerName(line.actor) && IsPlayerTtsListenerHint(line.listenerHint);
    }

    static bool IsPlayerTextOnlyLine(const ScriptLine& line) {
        return IsPlayerSpeakerName(line.actor) &&
            EqualsIgnoreCase(line.listenerHint, "__player_text_only");
    }

    static bool IsNarratorLine(const ScriptLine& line) {
        return EqualsIgnoreCase(line.actor, "The Narrator");
    }

    static bool IsActorDialogueLine(const ScriptLine& line) {
        return !IsPlayerSpeakerName(line.actor) && !IsNarratorLine(line);
    }

    static bool ShouldSendAbortDeliveryState(const ScriptLine& line) {
        return !line.utteranceId.empty() && IsActorDialogueLine(line);
    }

    static void SendAbortDeliveryStateIfTracked(const ScriptLine& line, const char* reason) {
        if (!ShouldSendAbortDeliveryState(line)) {
            return;
        }
        Log("SpeakManager: Marking queued utterance aborted (%s) speaker='%s' utterance='%s'",
            reason ? reason : "abort",
            line.actor.c_str(),
            line.utteranceId.c_str());
        SendDeliveryState(line, "aborted");
    }

    void ClearPlayerInputTtsGate(const char* reason) {
        std::lock_guard<std::mutex> lock(g_playerTtsGateMutex);
        if (!g_playerInputTtsGateActive) {
            return;
        }

        g_playerInputTtsGateActive = false;
        g_playerInputTtsGateUntil = {};
        Log("SpeakManager: Player TTS interrupt gate cleared (%s)",
            reason ? reason : "unknown");
    }

    static bool IsPlayerInputTtsGateActive() {
        std::lock_guard<std::mutex> lock(g_playerTtsGateMutex);
        if (!g_playerInputTtsGateActive) {
            return false;
        }

        if (std::chrono::steady_clock::now() >= g_playerInputTtsGateUntil) {
            g_playerInputTtsGateActive = false;
            g_playerInputTtsGateUntil = {};
            Log("SpeakManager: Player TTS interrupt gate timed out; allowing NPC queue");
            return false;
        }

        return true;
    }

    static uint32_t ResolveFaceTargetHint(const std::string& hint, uint32_t speakerFormId) {
        const std::string cleanHint = Trim(hint);
        if (cleanHint.empty()) {
            return 0;
        }

        if (IsPlayerHint(cleanHint)) {
            return 0x00000014;
        }

        const uint32_t agentFormId = AgentManager::FindAgentFormIdByName(cleanHint);
        if (agentFormId != 0 && agentFormId != speakerFormId) {
            return agentFormId;
        }

        const auto& currentTarget = TargetManager::GetCurrentTarget();
        if (currentTarget.formId != 0 &&
            currentTarget.formId != speakerFormId &&
            currentTarget.isActor &&
            EqualsIgnoreCase(currentTarget.name, cleanHint)) {
            return currentTarget.formId;
        }

        return 0;
    }

    static uint32_t ResolveFaceTargetFormId(const ScriptLine& line) {
        if (!Config::faceTargetDuringAIResponse ||
            line.actorFormId == 0 ||
            line.actorFormId == 0x00000014 ||
            EqualsIgnoreCase(line.actor, "Player") ||
            EqualsIgnoreCase(line.actor, "The Narrator")) {
            return 0;
        }

        uint32_t targetFormId = line.listenerFormId;
        if (targetFormId != 0 && targetFormId != line.actorFormId) {
            return targetFormId;
        }

        targetFormId = ResolveFaceTargetHint(line.listenerHint, line.actorFormId);
        if (targetFormId != 0) {
            return targetFormId;
        }

        targetFormId = line.rechatTargetFormId;
        if (targetFormId != 0 && targetFormId != line.actorFormId) {
            return targetFormId;
        }

        targetFormId = ResolveFaceTargetHint(line.rechatTargetHint, line.actorFormId);
        if (targetFormId != 0) {
            return targetFormId;
        }

        if (line.rechatDepth > 0) {
            Log("SpeakManager: Rechat facing target unresolved speaker=%s target=%s listener=%s; skipping crosshair fallback",
                line.actor.c_str(),
                line.rechatTargetHint.c_str(),
                line.listenerHint.c_str());
            return 0;
        }

        const auto& currentTarget = TargetManager::GetCurrentTarget();
        if (currentTarget.formId != 0 &&
            currentTarget.formId != line.actorFormId &&
            currentTarget.isActor) {
            return currentTarget.formId;
        }

        return 0x00000014;
    }

    static bool CalculateFaceTargetYaw(uint32_t speakerFormId,
                                       uint32_t targetFormId,
                                       float& yawDegrees,
                                       std::string* reason = nullptr) {
        auto speaker = ActorPositionResolverFNV::ResolveActor(speakerFormId);
        auto target = targetFormId == 0x00000014
            ? ActorPositionResolverFNV::ResolvePlayer()
            : ActorPositionResolverFNV::ResolveActor(targetFormId);

        if (!speaker.resolved || !target.resolved) {
            if (reason) {
                *reason = speaker.reason + "/" + target.reason;
            }
            return false;
        }

        const float dx = target.position.x - speaker.position.x;
        const float dy = target.position.y - speaker.position.y;
        if (std::sqrt((dx * dx) + (dy * dy)) < 1.0f) {
            if (reason) {
                *reason = "target_too_close";
            }
            return false;
        }

        static constexpr float kRadiansToDegrees = 57.2957795f;
        yawDegrees = std::atan2(dx, dy) * kRadiansToDegrees;
        while (yawDegrees < 0.0f) {
            yawDegrees += 360.0f;
        }
        while (yawDegrees >= 360.0f) {
            yawDegrees -= 360.0f;
        }
        if (reason) {
            std::ostringstream stream;
            stream << speaker.source << "/" << target.source
                << " dx=" << std::fixed << std::setprecision(1) << dx
                << " dy=" << dy
                << " yaw=" << yawDegrees;
            *reason = stream.str();
        }
        return true;
    }

    static void ApplyFaceTargetAtDialogueStart();

    static void ClearAppliedFaceTarget(uint32_t speakerFormId) {
        auto clear = [speakerFormId]() {
            if (speakerFormId != 0) {
                XNVSEAdapter::ClearNativeFacing(speakerFormId);
            }
        };
        if (GameThreadDispatcher::IsGameThread()) {
            clear();
        } else {
            GameThreadDispatcher::Enqueue("facing", "facing", RuntimeGeneration::Current(), std::move(clear));
        }
    }

    static bool ShouldSuppressFaceTargetForActorState(uint32_t speakerFormId, std::string& reason) {
        RuntimeSnapshot::ActorState actor;
        if (!RuntimeSnapshot::TryGetActor(speakerFormId, actor)) {
            reason = "actor_state_unknown";
            return true;
        }
        if (actor.deleted || actor.dead || !actor.loaded3D) {
            reason = "actor_unavailable";
            return true;
        }
        if (!actor.facingStateKnown) {
            reason = "posture_state_unknown";
            return true;
        }
        if (actor.sitSleepState != 0) {
            reason = actor.seated ? "seated" : "posture_transition_or_sleep";
            return true;
        }
        if (actor.animationBusy) {
            reason = "animation_action=" + std::to_string(actor.animationAction);
            return true;
        }
        if (actor.moving) {
            reason = "moving";
            return true;
        }
        return false;
    }

    static void ClearFaceTargetBridge() {
        const uint32_t previousSpeaker = g_faceTargetSpeakerFormId;
        g_faceTargetBridgeActive = false;
        g_faceTargetSpeakerFormId = 0;
        g_faceTargetTargetFormId = 0;
        ClearAppliedFaceTarget(previousSpeaker);
    }

    static void StartFaceTargetBridge(const ScriptLine& line) {
        const uint32_t targetFormId = ResolveFaceTargetFormId(line);
        if (line.actorFormId == 0 || targetFormId == 0 || targetFormId == line.actorFormId) {
            ClearFaceTargetBridge();
            return;
        }

        g_faceTargetBridgeActive = true;
        g_faceTargetSpeakerFormId = line.actorFormId;
        g_faceTargetTargetFormId = targetFormId;
        Log("SpeakManager: Applying one-shot face target speaker=%s (0x%08X) target=0x%08X",
            line.actor.c_str(), line.actorFormId, targetFormId);
        ApplyFaceTargetAtDialogueStart();
    }

    static void ApplyFaceTargetAtDialogueStart() {
        if (!g_faceTargetBridgeActive ||
            g_faceTargetSpeakerFormId == 0 ||
            g_faceTargetTargetFormId == 0) {
            return;
        }
        g_faceTargetBridgeActive = false;

        std::string suppressionReason;
        if (ShouldSuppressFaceTargetForActorState(g_faceTargetSpeakerFormId, suppressionReason)) {
            Log("SpeakManager: One-shot face target skipped speaker=0x%08X reason=%s",
                g_faceTargetSpeakerFormId, suppressionReason.c_str());
            return;
        }

        float yawDegrees = 0.0f;
        std::string yawReason;
        const bool yawReady = CalculateFaceTargetYaw(g_faceTargetSpeakerFormId,
                                                     g_faceTargetTargetFormId,
                                                     yawDegrees,
                                                     &yawReason);
        if (yawReady) {
            const uint32_t speakerFormId = g_faceTargetSpeakerFormId;
            const uint32_t targetFormId = g_faceTargetTargetFormId;
            auto apply = [speakerFormId, targetFormId, yawDegrees]() {
                if (!XNVSEAdapter::ApplyNativeFacing(speakerFormId, targetFormId, yawDegrees)) {
                    Log("SpeakManager: Native one-shot face target failed speaker=0x%08X target=0x%08X",
                        speakerFormId, targetFormId);
                }
            };
            if (GameThreadDispatcher::IsGameThread()) {
                apply();
            } else {
                GameThreadDispatcher::Enqueue("facing", "facing", RuntimeGeneration::Current(), std::move(apply));
            }
        }
        Log("SpeakManager: One-shot face target speaker=0x%08X target=0x%08X applied=%d reason=%s",
            g_faceTargetSpeakerFormId,
            g_faceTargetTargetFormId,
            yawReady ? 1 : 0,
            yawReason.c_str());
    }

    static void AppendUniqueAudienceName(std::vector<std::string>& names,
                                         std::set<std::string>& seen,
                                         const std::string& rawName) {
        const std::string name = Trim(rawName);
        if (name.empty() || name == "<no name>") {
            return;
        }

        std::string key = NormalizeName(name);
        if (seen.insert(key).second) {
            names.push_back(name);
        }
    }

    static ActorEligibilityFNV::Metadata EligibilityMetadataFromPosition(
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

    static bool RechatNameHintAllowed(const std::string& rawName) {
        ActorEligibilityFNV::Metadata metadata;
        metadata.name = rawName;
        return !ActorEligibilityFNV::IsClearlyDisallowedCreature(metadata);
    }

    static bool IsRechatPositionEligible(const ActorPositionResolverFNV::PositionResult& position,
                                         std::string* reason = nullptr) {
        if (!position.resolved || position.formId == 0) {
            if (reason) {
                *reason = "unresolved actor position";
            }
            return false;
        }
        if (!ActorPositionResolverFNV::IsPositionInPlayerScene(position)) {
            if (reason) {
                *reason = "actor is not in the player's current scene";
            }
            return false;
        }
        if (!ActorPositionResolverFNV::IsActorPositionFresh(position.formId, kRechatActorFreshnessMs)) {
            if (reason) {
                *reason = "actor position is stale for rechat";
            }
            return false;
        }
        if (!ActorPositionResolverFNV::IsActorInLatestScan(position.formId, kRechatActorFreshnessMs)) {
            if (reason) {
                *reason = "actor is not present in the latest spatial scan";
            }
            return false;
        }
        if (position.disabledKnown && position.isDisabled) {
            if (reason) {
                *reason = "actor is disabled";
            }
            return false;
        }
        if (position.deadKnown && position.isDead) {
            if (reason) {
                *reason = "actor is dead";
            }
            return false;
        }
        if (Config::sceneSafetyEnabled && position.sceneBusyKnown && position.sceneBusy) {
            if (reason) {
                *reason = "actor is currently controlled by a scene/dialogue package";
            }
            return false;
        }

        if (!ActivityStatusFNV::IsAutomaticDialogueAllowed(position.formId, reason)) {
            return false;
        }

        // Auto-managed actors have already passed the full activation category
        // check. Preserve that decision when partial position metadata briefly
        // exposes an allowed robot as a generic TESCreature during playback.
        const bool manuallyActivated = AgentManager::IsManuallyActivated(position.formId);
        if (manuallyActivated || AgentManager::IsAutoManaged(position.formId)) {
            return true;
        }
        return ActorEligibilityFNV::IsRechatAllowed(
            EligibilityMetadataFromPosition(position),
            false,
            reason);
    }

    static bool IsRechatAgentEligible(uint32_t formId, const std::string& name, std::string* reason = nullptr) {
        if (formId == 0) {
            if (reason) {
                *reason = RechatNameHintAllowed(name)
                    ? "missing form id for rechat candidate"
                    : "name hint is not an eligible rechat candidate";
            }
            return false;
        }

        const auto position = ActorPositionResolverFNV::ResolveActor(formId);
        if (position.resolved) {
            return IsRechatPositionEligible(position, reason);
        }

        ActorEligibilityFNV::Metadata metadata;
        metadata.name = name;
        if (ActorEligibilityFNV::IsClearlyDisallowedCreature(metadata, reason)) {
            return false;
        }

        return true;
    }

    static std::string AudiencePeoplePipe(const std::vector<std::string>& names) {
        std::ostringstream people;
        people << "|";
        for (const auto& name : names) {
            people << name << "|";
        }
        return people.str();
    }

    static std::string JsonStringArray(const std::vector<std::string>& names) {
        std::ostringstream json;
        json << "[";
        for (size_t i = 0; i < names.size(); ++i) {
            if (i > 0) {
                json << ",";
            }
            json << "\"" << HTTPManager::EscapeJson(names[i]) << "\"";
        }
        json << "]";
        return json.str();
    }

    static std::string NewRechatChainId() {
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const DWORD tick = GetTickCount();
        std::ostringstream stream;
        stream << "fnv-" << now << "-" << std::hex << tick;
        return stream.str();
    }

    static std::string SanitizeBridgeLine(std::string value) {
        std::replace(value.begin(), value.end(), '\r', ' ');
        std::replace(value.begin(), value.end(), '\n', ' ');
        std::replace(value.begin(), value.end(), '|', '-');
        value = Trim(value);
        if (value.size() > 240) {
            value = value.substr(0, 237) + "...";
        }
        return value;
    }

    static std::string NormalizeSubtitleText(std::string value) {
        std::replace(value.begin(), value.end(), '\r', ' ');
        std::replace(value.begin(), value.end(), '\n', ' ');
        std::replace(value.begin(), value.end(), '\t', ' ');
        value = Trim(value);

        std::string normalized;
        normalized.reserve(value.size());
        bool lastWasSpace = false;
        for (char ch : value) {
            const bool isSpace = std::isspace(static_cast<unsigned char>(ch)) != 0;
            if (isSpace) {
                if (!lastWasSpace) {
                    normalized.push_back(' ');
                    lastWasSpace = true;
                }
                continue;
            }
            normalized.push_back(ch == '|' ? '-' : ch);
            lastWasSpace = false;
        }

        normalized = Trim(normalized);
        if (normalized.size() > 1000) {
            normalized = normalized.substr(0, 997) + "...";
        }
        return normalized;
    }

    static std::string NormalizeSubtitleCompareKey(std::string value) {
        value = NormalizeSubtitleText(value);
        std::string normalized;
        normalized.reserve(value.size());
        bool lastWasSpace = false;
        for (char raw : value) {
            unsigned char ch = static_cast<unsigned char>(raw);
            if (std::isspace(ch)) {
                if (!lastWasSpace) {
                    normalized.push_back(' ');
                    lastWasSpace = true;
                }
                continue;
            }
            normalized.push_back(static_cast<char>(std::tolower(ch)));
            lastWasSpace = false;
        }
        return Trim(normalized);
    }

    static void AddRecentAiSubtitleKeyLocked(const std::string& text,
                                             std::chrono::steady_clock::time_point now) {
        const std::string key = NormalizeSubtitleCompareKey(text);
        if (key.empty()) {
            return;
        }

        for (auto& existing : g_recentAiSubtitleKeys) {
            if (existing.first == key) {
                existing.second = now;
                return;
            }
        }
        g_recentAiSubtitleKeys.push_back({ key, now });
    }

    static void PruneRecentAiSubtitleKeysLocked(std::chrono::steady_clock::time_point now) {
        g_recentAiSubtitleKeys.erase(
            std::remove_if(g_recentAiSubtitleKeys.begin(), g_recentAiSubtitleKeys.end(),
                [now](const auto& item) {
                    return now - item.second > kRecentAiSubtitleWindow;
                }),
            g_recentAiSubtitleKeys.end());
    }

    static void RememberRecentAiSubtitleText(const std::string& text) {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(g_recentSubtitleMutex);
        PruneRecentAiSubtitleKeysLocked(now);
        AddRecentAiSubtitleKeyLocked(text, now);

        const size_t colon = text.find(':');
        if (colon != std::string::npos && colon > 0 && colon <= 64) {
            AddRecentAiSubtitleKeyLocked(text.substr(colon + 1), now);
        }
    }

    static std::string BuildSpeakerSubtitleText(const ScriptLine& line) {
        const std::string displayText = NormalizeSubtitleText(line.text);
        const std::string speakerName = IsPlayerSpeakerName(line.actor)
            ? PlayerDisplayName()
            : (!line.displayName.empty() ? line.displayName : line.actor);
        const std::string speaker = NormalizeSubtitleText(speakerName);
        if (displayText.empty() || speaker.empty()) {
            return displayText;
        }

        const std::string prefix = speaker + ":";
        if (displayText.size() >= prefix.size() &&
            EqualsIgnoreCase(displayText.substr(0, prefix.size()), prefix)) {
            return displayText;
        }

        return speaker + ": " + displayText;
    }

    static bool EndsSubtitleSentence(char ch) {
        return ch == '.' || ch == '!' || ch == '?' || ch == ';' || ch == ':';
    }

    static std::vector<std::string> SplitSubtitleChunks(const std::string& text) {
        static constexpr size_t kPreferredMaxChars = 105;
        static constexpr size_t kHardMaxChars = 135;

        std::vector<std::string> chunks;
        std::istringstream words(text);
        std::string word;
        std::string current;

        auto pushCurrent = [&]() {
            current = Trim(current);
            if (!current.empty()) {
                chunks.push_back(current);
                current.clear();
            }
        };

        while (words >> word) {
            const size_t nextSize = current.empty()
                ? word.size()
                : current.size() + 1 + word.size();
            if (!current.empty() && nextSize > kHardMaxChars) {
                pushCurrent();
            }

            if (!current.empty()) {
                current.push_back(' ');
            }
            current += word;

            if (!current.empty() &&
                current.size() >= 35 &&
                EndsSubtitleSentence(current.back())) {
                pushCurrent();
            } else if (current.size() >= kPreferredMaxChars) {
                pushCurrent();
            }
        }
        pushCurrent();

        if (chunks.empty() && !text.empty()) {
            chunks.push_back(text);
        }
        return chunks;
    }

    static std::vector<PassiveSubtitleSegment> BuildPassiveSubtitleSegments(const std::string& text,
                                                                           double durationSeconds) {
        std::vector<PassiveSubtitleSegment> segments;
        const std::vector<std::string> chunks = SplitSubtitleChunks(text);
        if (chunks.empty()) {
            return segments;
        }

        double totalWeight = 0.0;
        for (const auto& chunk : chunks) {
            totalWeight += static_cast<double>(std::max<size_t>(1, chunk.size()));
        }
        if (totalWeight <= 0.0) {
            totalWeight = static_cast<double>(chunks.size());
        }

        const double usableDuration = std::max(0.25, durationSeconds);
        double cursor = 0.0;
        segments.reserve(chunks.size());
        for (size_t i = 0; i < chunks.size(); ++i) {
            const double weight = static_cast<double>(std::max<size_t>(1, chunks[i].size()));
            const double segmentDuration = (i + 1 == chunks.size())
                ? std::max(0.05, usableDuration - cursor)
                : std::max(0.20, usableDuration * (weight / totalWeight));
            const double end = (i + 1 == chunks.size())
                ? usableDuration
                : std::min(usableDuration, cursor + segmentDuration);
            segments.push_back({ cursor, end, chunks[i] });
            cursor = end;
        }

        return segments;
    }

    static void WriteSubtitleBridgeText(const std::string& displayText,
                                        const std::string& utteranceId,
                                        size_t segmentIndex,
                                        size_t segmentCount,
                                        double startSeconds,
                                        double endSeconds) {
        const std::string subtitle = NormalizeSubtitleText(displayText);
        RememberRecentAiSubtitleText(subtitle);
        auto apply = [subtitle, utteranceId, segmentIndex, segmentCount, startSeconds, endSeconds]() {
            if (XNVSEAdapter::SetNativePassiveSubtitle(subtitle)) {
                if (g_subtitleBridgeFallbackActive) {
                    std::ofstream clear(kSubtitleBridgePath, std::ios::binary | std::ios::trunc);
                    g_subtitleBridgeFallbackActive = false;
                }
                Log("SpeakManager: Native passive subtitle updated segment=%zu/%zu text=%s",
                    segmentIndex + 1, segmentCount, subtitle.c_str());
                return;
            }

            std::ofstream out(kSubtitleBridgePath, std::ios::binary | std::ios::trunc);
            if (!out.is_open()) {
                Log("SpeakManager: Failed to open subtitle bridge file: %s", kSubtitleBridgePath);
                return;
            }
            out << subtitle << "\n";
            out << "schema=dialectic.passive_subtitle.v1"
                << "|utterance_id=" << SanitizeBridgeLine(utteranceId)
                << "|segment=" << (segmentIndex + 1)
                << "|segments=" << segmentCount
                << "|start=" << std::fixed << std::setprecision(3) << startSeconds
                << "|end=" << std::fixed << std::setprecision(3) << endSeconds
                << "\n";
            g_subtitleScriptFallbacks.fetch_add(1, std::memory_order_relaxed);
            g_subtitleBridgeFallbackActive = true;
            Log("SpeakManager: Passive subtitle using script fallback segment=%zu/%zu text=%s",
                segmentIndex + 1, segmentCount, subtitle.c_str());
        };
        if (GameThreadDispatcher::IsGameThread()) {
            apply();
        } else {
            GameThreadDispatcher::Enqueue("subtitle", "subtitle", RuntimeGeneration::Current(),
                std::move(apply));
        }
    }

    static void ClearSubtitleBridge() {
        g_subtitleActive = false;
        g_subtitleSegments.clear();
        g_subtitleSegmentIndex = 0;
        g_subtitleUtteranceId.clear();
        g_subtitleLastText.clear();
        g_playerTextOnlySubtitleActive = false;
        g_playerTextOnlySubtitleUntil = {};
        g_npcTextOnlyFallbackActive = false;
        g_npcTextOnlyFallbackUntil = {};

        auto clear = []() {
            XNVSEAdapter::ClearNativePassiveSubtitle();
            std::ofstream out(kSubtitleBridgePath, std::ios::binary | std::ios::trunc);
            g_subtitleBridgeFallbackActive = false;
            Log("SpeakManager: Passive subtitle cleared");
        };
        if (GameThreadDispatcher::IsGameThread()) {
            clear();
        } else {
            GameThreadDispatcher::Enqueue("subtitle", "subtitle", RuntimeGeneration::Current(),
                std::move(clear));
        }
    }

    static void StartPlayerTextOnlySubtitle(const ScriptLine& line) {
        ClearSubtitleBridge();
        if (!Config::showAISubtitles) {
            Log("SpeakManager: Player text-only subtitle hidden because subtitles are disabled");
            return;
        }

        const std::string displayText = BuildSpeakerSubtitleText(line);
        if (displayText.empty()) {
            return;
        }

        g_playerTextOnlySubtitleActive = true;
        g_playerTextOnlySubtitleUntil = std::chrono::steady_clock::now() + kPlayerTextOnlySubtitleDuration;
        WriteSubtitleBridgeText(displayText, line.requestId, 0, 1, 0.0, 2.0);
        Log("SpeakManager: Player text-only subtitle started duration_ms=%lld text='%s'",
            static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                kPlayerTextOnlySubtitleDuration).count()),
            PreviewText(displayText).c_str());
    }

    static void UpdatePlayerTextOnlySubtitle() {
        if (!g_playerTextOnlySubtitleActive) {
            return;
        }

        if (!Config::showAISubtitles ||
            std::chrono::steady_clock::now() >= g_playerTextOnlySubtitleUntil) {
            Log("SpeakManager: Player text-only subtitle finished");
            ClearSubtitleBridge();
        }
    }

    static std::chrono::milliseconds EstimateNpcTextOnlyFallbackDuration(const ScriptLine& line) {
        const size_t visibleCharacters = NormalizeSubtitleText(line.text).size();
        const long long estimatedMs = static_cast<long long>(visibleCharacters) * 55LL + 1200LL;
        return std::chrono::milliseconds(std::clamp<long long>(estimatedMs, 2500LL, 12000LL));
    }

    static void StartNpcTextOnlyFallback(const ScriptLine& line) {
        ClearSubtitleBridge();
        const auto duration = Config::showAISubtitles
            ? EstimateNpcTextOnlyFallbackDuration(line)
            : std::chrono::milliseconds(100);
        g_npcTextOnlyFallbackActive = true;
        g_npcTextOnlyFallbackUntil = std::chrono::steady_clock::now() + duration;

        if (Config::showAISubtitles) {
            const std::string displayText = BuildSpeakerSubtitleText(line);
            if (!displayText.empty()) {
                WriteSubtitleBridgeText(displayText,
                                        line.utteranceId.empty() ? line.requestId : line.utteranceId,
                                        0,
                                        1,
                                        0.0,
                                        std::chrono::duration<double>(duration).count());
            }
        }

        Log("SpeakManager: Audio unavailable; delivering text-only fallback speaker='%s' duration_ms=%lld text='%s'",
            line.actor.c_str(),
            static_cast<long long>(duration.count()),
            PreviewText(line.text).c_str());
        SendDeliveryState(line, "text_only");
    }

    static bool UpdateNpcTextOnlyFallback() {
        if (!g_npcTextOnlyFallbackActive) {
            return false;
        }
        if (std::chrono::steady_clock::now() < g_npcTextOnlyFallbackUntil) {
            return true;
        }

        g_npcTextOnlyFallbackActive = false;
        g_npcTextOnlyFallbackUntil = {};
        ClearSubtitleBridge();
        return false;
    }

    struct WavInfo {
        const uint8_t* pcmData = nullptr;
        size_t pcmSize = 0;
        uint16_t channels = 0;
        uint32_t sampleRate = 0;
        uint16_t bitsPerSample = 0;
        uint16_t blockAlign = 0;
        double durationSeconds = 0.0;
    };

    struct LipSyncFrame {
        double startSeconds = 0.0;
        double endSeconds = 0.0;
        int phoneme = -1;
        int intensity = 0;
        int decayIntensity = 0;
    };

    struct LipSyncTextSegment {
        int viseme = -1;
        double durationSeconds = 0.0;
        int visemeLength = 1;
    };

    struct LipSyncSilenceSegment {
        double startSeconds = 0.0;
        double endSeconds = 0.0;
    };

    static constexpr bool kNativeLipSyncEnabled = true;
    static constexpr double kFlatLipSyncFrameSeconds = 0.016;
    static constexpr double kReferenceTextRate = 92.0 / 5.45;

    static std::mutex g_lipSyncMutex;
    static bool g_lipSyncActive = false;
    static uint32_t g_lipSyncActorFormId = 0;
    static std::vector<LipSyncFrame> g_lipSyncFrames;
    static size_t g_lipSyncFrameIndex = 0;
    static int g_lipSyncLastPhoneme = -2;
    static int g_lipSyncLastIntensity = -1;
    static uint64_t g_lipSyncCommandCount = 0;
    static int g_lipSyncConsecutiveNativeFailures = 0;
    static uint64_t g_lipSyncRuntimeGeneration = 0;
    static uint64_t g_lipSyncAudioGeneration = 0;
    static uint64_t g_lipSyncLineSequence = 0;
    static std::chrono::steady_clock::time_point g_lipSyncLastCommandAt = {};
    static constexpr int kMaxConsecutiveNativeLipSyncFailures = 3;
    static constexpr int kMaxLipSyncResetAttempts = 5;
    static constexpr auto kLipSyncResetRetryDelay = std::chrono::milliseconds(100);

    struct PendingLipSyncReset {
        uint32_t actorFormId = 0;
        uint64_t lineSequence = 0;
        int attempts = 0;
        std::chrono::steady_clock::time_point nextAttempt = {};
        std::string reason;
    };

    static std::deque<PendingLipSyncReset> g_pendingLipSyncResets;

    static uint16_t ReadLE16(const uint8_t* data) {
        return static_cast<uint16_t>(data[0] | (data[1] << 8));
    }

    static uint32_t ReadLE32(const uint8_t* data) {
        return static_cast<uint32_t>(data[0]) |
            (static_cast<uint32_t>(data[1]) << 8) |
            (static_cast<uint32_t>(data[2]) << 16) |
            (static_cast<uint32_t>(data[3]) << 24);
    }

    static bool ParseWavInfo(const std::vector<uint8_t>& wavData, WavInfo& outInfo) {
        if (wavData.size() < 44 ||
            std::memcmp(wavData.data(), "RIFF", 4) != 0 ||
            std::memcmp(wavData.data() + 8, "WAVE", 4) != 0) {
            return false;
        }

        bool foundFmt = false;
        bool foundData = false;
        uint16_t formatTag = 0;

        size_t offset = 12;
        while (offset + 8 <= wavData.size()) {
            const uint8_t* chunk = wavData.data() + offset;
            const uint32_t chunkSize = ReadLE32(chunk + 4);
            const size_t dataOffset = offset + 8;
            if (dataOffset + chunkSize > wavData.size()) {
                break;
            }

            if (std::memcmp(chunk, "fmt ", 4) == 0 && chunkSize >= 16) {
                formatTag = ReadLE16(wavData.data() + dataOffset);
                outInfo.channels = ReadLE16(wavData.data() + dataOffset + 2);
                outInfo.sampleRate = ReadLE32(wavData.data() + dataOffset + 4);
                outInfo.blockAlign = ReadLE16(wavData.data() + dataOffset + 12);
                outInfo.bitsPerSample = ReadLE16(wavData.data() + dataOffset + 14);
                foundFmt = true;
            } else if (std::memcmp(chunk, "data", 4) == 0) {
                outInfo.pcmData = wavData.data() + dataOffset;
                outInfo.pcmSize = chunkSize;
                foundData = true;
            }

            offset = dataOffset + chunkSize + (chunkSize & 1);
        }

        if (!foundFmt || !foundData || formatTag != 1 ||
            outInfo.channels == 0 || outInfo.sampleRate == 0 ||
            outInfo.blockAlign == 0 || outInfo.pcmSize == 0) {
            return false;
        }

        outInfo.durationSeconds = static_cast<double>(outInfo.pcmSize) /
            static_cast<double>(outInfo.sampleRate * outInfo.blockAlign);
        return outInfo.durationSeconds > 0.0;
    }

    static double EstimateSubtitleDurationSeconds(const std::string& text) {
        int words = 0;
        bool inWord = false;
        int punctuationPauseMs = 0;
        for (char rawChar : text) {
            const unsigned char ch = static_cast<unsigned char>(rawChar);
            const bool wordChar = std::isalnum(ch) != 0;
            if (wordChar && !inWord) {
                ++words;
                inWord = true;
            } else if (!wordChar) {
                inWord = false;
            }

            if (ch == '.' || ch == '!' || ch == '?') {
                punctuationPauseMs += 260;
            } else if (ch == ',' || ch == ';' || ch == ':') {
                punctuationPauseMs += 120;
            }
        }

        const int chars = static_cast<int>(text.size());
        int baseMs = words > 0 ? words * 360 : chars * 55;
        baseMs += punctuationPauseMs + 220;
        baseMs = std::max(900, std::min(45000, baseMs));
        return static_cast<double>(baseMs) / 1000.0;
    }

    static void StartSubtitleBridge(const ScriptLine& line,
                                    const std::vector<uint8_t>& audioData) {
        g_playerTextOnlySubtitleActive = false;
        g_playerTextOnlySubtitleUntil = {};
        const std::string displayText = BuildSpeakerSubtitleText(line);
        if (!Config::showAISubtitles ||
            displayText.empty()) {
            ClearSubtitleBridge();
            return;
        }

        WavInfo wav;
        double durationSeconds = EstimateSubtitleDurationSeconds(displayText);
        if (ParseWavInfo(audioData, wav)) {
            durationSeconds = wav.durationSeconds;
        }

        std::vector<PassiveSubtitleSegment> segments =
            BuildPassiveSubtitleSegments(displayText, durationSeconds);
        if (segments.empty()) {
            ClearSubtitleBridge();
            return;
        }

        g_subtitleSegments = std::move(segments);
        g_subtitleSegmentIndex = 0;
        g_subtitleUtteranceId = !line.utteranceId.empty()
            ? line.utteranceId
            : (!line.ttsCacheKey.empty() ? line.ttsCacheKey : "local");
        g_subtitleLastText.clear();
        g_subtitleActive = true;

        const PassiveSubtitleSegment& first = g_subtitleSegments.front();
        WriteSubtitleBridgeText(first.text,
                                g_subtitleUtteranceId,
                                0,
                                g_subtitleSegments.size(),
                                first.startSeconds,
                                first.endSeconds);
        g_subtitleLastText = first.text;
        Log("SpeakManager: Passive subtitles started for %s segments=%zu duration=%.3f",
            line.actor.c_str(),
            g_subtitleSegments.size(),
            durationSeconds);
    }

    static void UpdateSubtitleBridge() {
        if (!Config::showAISubtitles) {
            if (g_subtitleActive) {
                ClearSubtitleBridge();
            }
            return;
        }

        if (!g_subtitleActive || g_subtitleSegments.empty()) {
            return;
        }

        const double elapsed = AudioManager::GetCurrentPlayTime();
        size_t nextIndex = g_subtitleSegmentIndex;
        while (nextIndex + 1 < g_subtitleSegments.size() &&
               elapsed >= g_subtitleSegments[nextIndex].endSeconds) {
            ++nextIndex;
        }

        if (nextIndex == g_subtitleSegmentIndex &&
            g_subtitleLastText == g_subtitleSegments[nextIndex].text) {
            return;
        }

        g_subtitleSegmentIndex = nextIndex;
        const PassiveSubtitleSegment& segment = g_subtitleSegments[g_subtitleSegmentIndex];
        WriteSubtitleBridgeText(segment.text,
                                g_subtitleUtteranceId,
                                g_subtitleSegmentIndex,
                                g_subtitleSegments.size(),
                                segment.startSeconds,
                                segment.endSeconds);
        g_subtitleLastText = segment.text;
    }

    static std::string ConsoleFormId(uint32_t formId) {
        std::ostringstream stream;
        stream << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << formId;
        return stream.str();
    }

    static void WriteLipSyncStatus(const std::string& status) {
        std::ofstream out(kLipSyncStatusPath, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            return;
        }

        out << status << "\r\n";
    }

    static int PhonemeForTextAt(const std::string& text, size_t index, size_t& consumed) {
        consumed = 1;
        if (index >= text.size()) {
            return -1;
        }

        const char ch = static_cast<char>(std::tolower(static_cast<unsigned char>(text[index])));
        const char next = (index + 1 < text.size())
            ? static_cast<char>(std::tolower(static_cast<unsigned char>(text[index + 1])))
            : '\0';

        if (!std::isalpha(static_cast<unsigned char>(ch))) {
            return -1;
        }

        if (ch == 't' && next == 'h') {
            consumed = 2;
            return 14; // Th
        }
        if (ch == 'n' && next == 'g') {
            consumed = 2;
            return 4; // NG -> D/S/T
        }
        if (ch == 's' && next == 'h') {
            consumed = 2;
            return 3; // Ch/J/Sh
        }
        if (ch == 'c' && next == 'h') {
            consumed = 2;
            return 3; // Ch/J/Sh
        }
        if (ch == 'z' && next == 'h') {
            consumed = 2;
            return 3; // Ch/J/Sh
        }
        if (ch == 'o' && next == 'o') {
            consumed = 2;
            return 11; // Ooh/Q maps to the OH slot in practice
        }

        switch (ch) {
        case 'a': return 1;  // Big Aah
        case 'e': return 6;  // Eh
        case 'i': return 5;  // Eee
        case 'o': return 11; // Oh
        case 'u': return 11; // Ooh/Q maps to the OH slot in practice
        case 'y': return 5;
        case 'b':
        case 'm':
        case 'p': return 2;  // BMP
        case 'f':
        case 'v': return 7;  // FV
        case 'd':
        case 's':
        case 't':
        case 'z': return 4;  // DST
        case 'c':
        case 'g':
        case 'k':
        case 'x': return 3;  // Hard consonants map to Ch/J/Sh
        case 'h': return 6;  // HH -> Eh
        case 'j': return 3;  // Ch/J/Sh
        case 'n': return 10; // N
        case 'r': return 13; // R
        case 'q':
        case 'w': return 15; // W
        case 'l': return 14; // L -> Th
        default: return -1;
        }
    }

    static std::vector<int> BuildPhonemeSequence(const std::string& text) {
        std::vector<int> phonemes;
        phonemes.reserve(text.size());

        for (size_t i = 0; i < text.size();) {
            size_t consumed = 1;
            const int phoneme = PhonemeForTextAt(text, i, consumed);
            if (phoneme >= 0) {
                phonemes.push_back(phoneme);
            } else if (text[i] == ',' || text[i] == '.' || text[i] == '!' ||
                       text[i] == '?' || text[i] == ';' || text[i] == ':') {
                if (!phonemes.empty() && phonemes.back() != -1) {
                    phonemes.push_back(-1);
                }
            }
            i += consumed == 0 ? 1 : consumed;
        }

        while (!phonemes.empty() && phonemes.back() == -1) {
            phonemes.pop_back();
        }
        return phonemes;
    }

    static int ClampIntensity(int value) {
        if (value < 0) return 0;
        if (value > 100) return 100;
        return value;
    }

    static int EstimateFrameIntensity(const WavInfo& wav, double startSeconds, double endSeconds) {
        if (wav.bitsPerSample != 16 || wav.channels == 0 || wav.blockAlign == 0 || !wav.pcmData) {
            return ClampIntensity(static_cast<int>(35.0f * g_animationIntensity));
        }

        size_t startFrame = static_cast<size_t>(startSeconds * wav.sampleRate);
        size_t endFrame = static_cast<size_t>(endSeconds * wav.sampleRate);
        const size_t totalFrames = wav.pcmSize / wav.blockAlign;
        if (startFrame >= totalFrames) {
            return 0;
        }
        if (endFrame > totalFrames) {
            endFrame = totalFrames;
        }
        if (endFrame <= startFrame) {
            endFrame = startFrame + 1;
        }

        double sumSquares = 0.0;
        size_t sampleCount = 0;
        for (size_t frame = startFrame; frame < endFrame; ++frame) {
            const uint8_t* framePtr = wav.pcmData + frame * wav.blockAlign;
            for (uint16_t channel = 0; channel < wav.channels; ++channel) {
                const uint8_t* samplePtr = framePtr + channel * 2;
                const int16_t sample = static_cast<int16_t>(ReadLE16(samplePtr));
                const double normalized = static_cast<double>(sample) / 32768.0;
                sumSquares += normalized * normalized;
                ++sampleCount;
            }
        }

        if (sampleCount == 0) {
            return 0;
        }

        const double rms = std::sqrt(sumSquares / static_cast<double>(sampleCount));
        if (rms < 0.010) {
            return 0;
        }

        int intensity = static_cast<int>(std::round(rms * 520.0 * g_animationIntensity));
        if (intensity > 0 && intensity < 18) {
            intensity = 18;
        }
        return ClampIntensity(intensity);
    }

    static std::vector<LipSyncTextSegment> BuildFlatTrimTextSegments(const std::string& text,
                                                                     double soundDuration) {
        std::vector<LipSyncTextSegment> segments;
        segments.reserve(text.size() + 1);

        for (size_t i = 0; i < text.size();) {
            size_t consumed = 1;
            const int viseme = PhonemeForTextAt(text, i, consumed);
            const unsigned char ch = static_cast<unsigned char>(text[i]);
            if (viseme >= 0) {
                segments.push_back({ viseme, 0.0, static_cast<int>(std::max<size_t>(1, consumed)) });
            } else if (std::isspace(ch) || text[i] == ',' || text[i] == '.' ||
                       text[i] == '!' || text[i] == '?' || text[i] == ';' ||
                       text[i] == ':') {
                if (segments.empty() || segments.back().viseme != -1) {
                    segments.push_back({ -1, 0.0, 1 });
                }
            }
            i += consumed == 0 ? 1 : consumed;
        }

        while (!segments.empty() && segments.back().viseme == -1) {
            segments.pop_back();
        }

        int totalLength = 0;
        for (const auto& segment : segments) {
            totalLength += std::max(1, segment.visemeLength);
        }

        if (totalLength > 0 && soundDuration > 0.0) {
            for (auto& segment : segments) {
                segment.durationSeconds =
                    soundDuration * static_cast<double>(std::max(1, segment.visemeLength)) /
                    static_cast<double>(totalLength);
            }
        }

        segments.push_back({ -1, 0.15, 1 });
        return segments;
    }

    static std::vector<LipSyncSilenceSegment> DetectFlatTrimSilences(const WavInfo& wav,
                                                                     double preClipSeconds,
                                                                     double silenceThreshold = 0.05,
                                                                     double minSilenceDurationSeconds = 0.150) {
        std::vector<LipSyncSilenceSegment> silences;
        if (wav.bitsPerSample != 16 || wav.channels == 0 || wav.blockAlign == 0 || !wav.pcmData) {
            return silences;
        }

        const size_t totalFrames = wav.pcmSize / wav.blockAlign;
        const int silenceSampleThreshold = static_cast<int>(
            silenceThreshold * static_cast<double>((1 << (wav.bitsPerSample - 1)) - 1));

        bool inSilence = false;
        size_t silenceStartFrame = 0;

        for (size_t frame = 0; frame < totalFrames; ++frame) {
            const uint8_t* framePtr = wav.pcmData + frame * wav.blockAlign;
            bool isSilent = false;
            for (uint16_t channel = 0; channel < wav.channels; ++channel) {
                const uint8_t* samplePtr = framePtr + channel * 2;
                const int16_t sample = static_cast<int16_t>(ReadLE16(samplePtr));
                if (std::abs(static_cast<int>(sample)) < silenceSampleThreshold) {
                    isSilent = true;
                    break;
                }
            }

            if (isSilent) {
                if (!inSilence) {
                    inSilence = true;
                    silenceStartFrame = frame;
                }
            } else if (inSilence) {
                const double startSeconds = static_cast<double>(silenceStartFrame) /
                    static_cast<double>(wav.sampleRate);
                const double endSeconds = static_cast<double>(frame) /
                    static_cast<double>(wav.sampleRate);
                if (endSeconds - startSeconds >= minSilenceDurationSeconds &&
                    startSeconds - preClipSeconds > 0.0) {
                    silences.push_back({ startSeconds, endSeconds });
                }
                inSilence = false;
            }
        }

        if (inSilence) {
            const double startSeconds = static_cast<double>(silenceStartFrame) /
                static_cast<double>(wav.sampleRate);
            const double endSeconds = static_cast<double>(totalFrames) /
                static_cast<double>(wav.sampleRate);
            if (endSeconds - startSeconds >= minSilenceDurationSeconds) {
                silences.push_back({ startSeconds, endSeconds });
            }
        }

        return silences;
    }

    static bool IsSilentAt(const std::vector<LipSyncSilenceSegment>& silences, double elapsedSeconds) {
        for (const auto& segment : silences) {
            if (elapsedSeconds >= segment.startSeconds && elapsedSeconds <= segment.endSeconds) {
                return true;
            }
        }
        return false;
    }

    static std::vector<LipSyncFrame> BuildLipSyncFrames(const ScriptLine& line,
                                                         const std::vector<uint8_t>& audioData) {
        WavInfo wav;
        std::vector<LipSyncFrame> frames;
        if (!ParseWavInfo(audioData, wav)) {
            Log("SpeakManager: Lip sync skipped because WAV parsing failed");
            return frames;
        }

        const double preClip = std::max(0.0f, g_preClipMs) / 1000.0;
        const double postClip = std::max(0.0f, g_postClipMs) / 1000.0;
        const double speechDuration = std::max(0.05, wav.durationSeconds - postClip);
        const std::vector<LipSyncTextSegment> textSegments =
            BuildFlatTrimTextSegments(line.text, speechDuration);
        if (textSegments.empty()) {
            Log("SpeakManager: Lip sync skipped because no text segments were generated");
            return frames;
        }

        const std::vector<LipSyncSilenceSegment> silences = DetectFlatTrimSilences(wav, preClip);
        double totalSegmentDuration = 0.0;
        for (const auto& segment : textSegments) {
            totalSegmentDuration += std::max(0.0, segment.durationSeconds);
        }

        const double frameEnd = std::min(wav.durationSeconds,
            std::max(speechDuration, totalSegmentDuration));
        const size_t reserveCount = static_cast<size_t>(
            std::ceil(std::max(frameEnd, totalSegmentDuration) / kFlatLipSyncFrameSeconds)) + 2;
        frames.reserve(reserveCount);

        const double referenceDuration = std::max(0.10, wav.durationSeconds);
        const double intensityModifierDyn =
            (static_cast<double>(std::max<size_t>(1, line.text.size())) / referenceDuration) /
            kReferenceTextRate;
        const float intensityModifier = std::max(0.0f, g_animationIntensity) *
            static_cast<float>(intensityModifierDyn);

        size_t segmentIndex = 0;
        double segmentEnd = textSegments.empty() ? 0.0 : textSegments.front().durationSeconds;
        int lastViseme = 7;
        float intensity = 0.0f;

        for (double start = 0.0; start < frameEnd;) {
            const double end = std::min(start + kFlatLipSyncFrameSeconds, frameEnd);
            const double elapsed = start;

            while (segmentIndex + 1 < textSegments.size() && elapsed > segmentEnd) {
                ++segmentIndex;
                segmentEnd += textSegments[segmentIndex].durationSeconds;
            }

            int visemeCode = textSegments[segmentIndex].viseme;
            if (IsSilentAt(silences, elapsed)) {
                visemeCode = -1;
            }

            const double deltaSeconds = std::max(0.001, end - start);
            const float intensityStep =
                0.02f * static_cast<float>(deltaSeconds / 0.0019) * intensityModifier;
            const float intensityStepDecal = intensityStep;

            int candidateLastViseme = visemeCode;
            float candidateIntensity = intensity;
            if (lastViseme == visemeCode) {
                candidateIntensity = (candidateIntensity + intensityStep) * 1.01f;
            } else {
                candidateIntensity = 0.0f;
            }
            if (candidateIntensity > 0.99f) {
                candidateIntensity = 1.0f;
            }
            if (visemeCode < 0) {
                candidateIntensity = 0.0f;
            }

            const int frameIntensity = ClampIntensity(
                static_cast<int>(std::round(std::clamp(candidateIntensity, 0.0f, 1.0f) * 100.0f)));
            const int decayIntensity = ClampIntensity(
                static_cast<int>(std::round(std::clamp(intensityStepDecal, 0.0f, 1.0f) * 100.0f)));
            frames.push_back({ start, end, visemeCode, frameIntensity, std::max(1, decayIntensity) });

            lastViseme = candidateLastViseme;
            intensity = candidateIntensity;
            start = end;
        }

        if (frames.empty() || frames.back().phoneme != -1 || frames.back().intensity != 0) {
            const double closeStart = frames.empty() ? 0.0 : frames.back().endSeconds;
            const double closeEnd = std::min(wav.durationSeconds, closeStart + 0.15);
            if (closeEnd > closeStart) {
                frames.push_back({ closeStart, closeEnd, -1, 0, 100 });
            }
        }

        Log("SpeakManager: Lip sync frames built speaker=%s frames=%zu segments=%zu silences=%zu duration=%.3f speech=%.3f intensity_mod=%.3f",
            line.actor.c_str(),
            frames.size(),
            textSegments.size(),
            silences.size(),
            wav.durationSeconds,
            speechDuration,
            intensityModifier);

        return frames;
    }

    static void ClearLipSyncSessionLocked() {
        g_lipSyncActive = false;
        g_lipSyncActorFormId = 0;
        g_lipSyncFrames.clear();
        g_lipSyncFrameIndex = 0;
        g_lipSyncLastPhoneme = -2;
        g_lipSyncLastIntensity = -1;
        g_lipSyncCommandCount = 0;
        g_lipSyncConsecutiveNativeFailures = 0;
        g_lipSyncRuntimeGeneration = 0;
        g_lipSyncAudioGeneration = 0;
        g_lipSyncLineSequence = 0;
        g_lipSyncLastCommandAt = {};
    }

    static void QueueLipSyncResetRetry(uint32_t formId,
                                       uint64_t lineSequence,
                                       int attempts,
                                       const char* reason) {
        std::lock_guard<std::mutex> lock(g_lipSyncMutex);
        for (PendingLipSyncReset& pending : g_pendingLipSyncResets) {
            if (pending.actorFormId == formId && pending.lineSequence == lineSequence) {
                pending.attempts = std::max(pending.attempts, attempts);
                pending.nextAttempt = std::chrono::steady_clock::now() + kLipSyncResetRetryDelay;
                return;
            }
        }
        g_pendingLipSyncResets.push_back({
            formId,
            lineSequence,
            attempts,
            std::chrono::steady_clock::now() + kLipSyncResetRetryDelay,
            reason ? reason : "unknown"
        });
    }

    static void FinalizeLipSync(const char* reason) {
        uint32_t formId = 0;
        uint64_t lineSequence = 0;
        {
            std::lock_guard<std::mutex> lock(g_lipSyncMutex);
            formId = g_lipSyncActorFormId;
            lineSequence = g_lipSyncLineSequence;
            ClearLipSyncSessionLocked();
        }

        if (formId == 0) {
            return;
        }

        if (XNVSEAdapter::ResetNativeLipSync(formId)) {
            Log("SpeakManager: Lip sync finalized reason=%s ref=0x%08X line=%llu reset=direct",
                reason ? reason : "unknown",
                formId,
                static_cast<unsigned long long>(lineSequence));
            WriteLipSyncStatus(std::string("status=reset_direct reason=") +
                               (reason ? reason : "unknown") +
                               " ref=" + ConsoleFormId(formId));
            return;
        }

        Log("SpeakManager: Lip sync reset deferred reason=%s ref=0x%08X line=%llu",
            reason ? reason : "unknown",
            formId,
            static_cast<unsigned long long>(lineSequence));
        QueueLipSyncResetRetry(formId, lineSequence, 1, reason);
        WriteLipSyncStatus(std::string("status=reset_pending reason=") +
                           (reason ? reason : "unknown") +
                           " ref=" + ConsoleFormId(formId));
    }

    static void ProcessPendingLipSyncResets() {
        PendingLipSyncReset pending;
        bool hasPending = false;
        {
            std::lock_guard<std::mutex> lock(g_lipSyncMutex);
            const auto now = std::chrono::steady_clock::now();
            for (auto it = g_pendingLipSyncResets.begin(); it != g_pendingLipSyncResets.end();) {
                if (g_lipSyncActive && g_lipSyncActorFormId == it->actorFormId) {
                    if (g_lipSyncLineSequence != it->lineSequence) {
                        Log("SpeakManager: Discarding obsolete lip sync reset ref=0x%08X oldLine=%llu newLine=%llu",
                            it->actorFormId,
                            static_cast<unsigned long long>(it->lineSequence),
                            static_cast<unsigned long long>(g_lipSyncLineSequence));
                        it = g_pendingLipSyncResets.erase(it);
                        continue;
                    }
                    ++it;
                    continue;
                }
                if (it->nextAttempt <= now) {
                    pending = *it;
                    g_pendingLipSyncResets.erase(it);
                    hasPending = true;
                    break;
                }
                ++it;
            }
        }

        if (!hasPending) {
            return;
        }
        if (XNVSEAdapter::ResetNativeLipSync(pending.actorFormId)) {
            Log("SpeakManager: Lip sync reset retry succeeded reason=%s ref=0x%08X attempts=%d",
                pending.reason.c_str(), pending.actorFormId, pending.attempts + 1);
            WriteLipSyncStatus(std::string("status=reset_retry_succeeded reason=") +
                               pending.reason + " ref=" + ConsoleFormId(pending.actorFormId));
            return;
        }

        ++pending.attempts;
        if (pending.attempts >= kMaxLipSyncResetAttempts) {
            Log("SpeakManager: Lip sync reset exhausted reason=%s ref=0x%08X attempts=%d",
                pending.reason.c_str(), pending.actorFormId, pending.attempts);
            WriteLipSyncStatus(std::string("status=reset_failed reason=") +
                               pending.reason + " ref=" + ConsoleFormId(pending.actorFormId));
            return;
        }
        QueueLipSyncResetRetry(pending.actorFormId,
                               pending.lineSequence,
                               pending.attempts,
                               pending.reason.c_str());
    }

    static uint32_t ResolveSpeakerFormId(const ScriptLine& line);

    static size_t FindFirstActiveLipSyncFrame(const std::vector<LipSyncFrame>& frames) {
        for (size_t i = 0; i < frames.size(); ++i) {
            if (frames[i].phoneme >= 0 && frames[i].intensity > 0) {
                return i;
            }
        }
        return frames.size();
    }

    static bool PrimeLipSyncNow(uint32_t formId,
                                const std::vector<LipSyncFrame>& frames,
                                size_t frameIndex,
                                const std::string& speaker) {
        if (formId == 0 || frames.empty() || frameIndex >= frames.size()) {
            return false;
        }

        const LipSyncFrame& frame = frames[frameIndex];
        if (frame.phoneme < 0 || frame.intensity <= 0) {
            return false;
        }

        const int intensity = std::max(85, frame.intensity);
        if (XNVSEAdapter::ApplyNativeFaceGenLipSync(
                formId, frame.phoneme, intensity, frame.decayIntensity, false)) {
            Log("SpeakManager: Lip sync primed direct for %s ref=0x%08X phoneme=%d intensity=%d frame=%zu",
                speaker.c_str(),
                formId,
                frame.phoneme,
                intensity,
                frameIndex);
            return true;
        }

        Log("SpeakManager: Lip sync skipped for %s ref=0x%08X because native FaceGen is unavailable; "
            "high-frequency MFG fallback is disabled",
            speaker.c_str(),
            formId);
        return false;
    }

    static void StartLipSync(const ScriptLine& line, const std::vector<uint8_t>& audioData) {
        FinalizeLipSync("replacement_line");

        const uint32_t formId = ResolveSpeakerFormId(line);
        if (IsPlayerSpeakerName(line.actor) ||
            formId == 0x00000014 ||
            EqualsIgnoreCase(line.actor, "Player") ||
            EqualsIgnoreCase(line.actor, "The Narrator")) {
            Log("SpeakManager: Lip sync skipped for non-NPC speaker '%s'", line.actor.c_str());
            WriteLipSyncStatus(std::string("status=skipped reason=non_npc speaker=") + line.actor);
            return;
        }
        if (formId == 0) {
            Log("SpeakManager: Lip sync skipped for %s because speaker form id could not be resolved "
                "(lineForm=0x%08X listener=%s rechatTarget=%s depth=%d)",
                line.actor.c_str(),
                line.actorFormId,
                line.listenerHint.c_str(),
                line.rechatTargetHint.c_str(),
                line.rechatDepth);
            WriteLipSyncStatus(std::string("status=skipped reason=unresolved_speaker speaker=") + line.actor);
            return;
        }

        Log("SpeakManager: Lip sync resolved current speaker %s ref=0x%08X",
            line.actor.c_str(), formId);

        if (!kNativeLipSyncEnabled) {
            Log("SpeakManager: Lip sync skipped for %s ref=0x%08X because native FaceGen lip sync is disabled",
                line.actor.c_str(),
                formId);
            WriteLipSyncStatus(std::string("status=skipped reason=native_facegen_disabled speaker=") +
                               line.actor + " ref=" + ConsoleFormId(formId));
            return;
        }

        const uint64_t runtimeGeneration = line.runtimeGeneration != 0
            ? line.runtimeGeneration
            : RuntimeGeneration::Current();
        const uint64_t audioGeneration = g_audioGeneration.load();
        if (!RuntimeGeneration::IsCurrent(runtimeGeneration)) {
            Log("SpeakManager: Lip sync skipped for %s ref=0x%08X because runtime generation %llu is stale",
                line.actor.c_str(),
                formId,
                static_cast<unsigned long long>(runtimeGeneration));
            WriteLipSyncStatus(std::string("status=skipped reason=stale_runtime_generation speaker=") +
                               line.actor + " ref=" + ConsoleFormId(formId));
            return;
        }

        std::vector<LipSyncFrame> frames = BuildLipSyncFrames(line, audioData);
        if (frames.empty()) {
            Log("SpeakManager: Lip sync skipped for %s ref=0x%08X because no frames were generated",
                line.actor.c_str(),
                formId);
            WriteLipSyncStatus(std::string("status=skipped reason=no_frames speaker=") + line.actor + " ref=" + ConsoleFormId(formId));
            return;
        }
        const size_t frameCount = frames.size();
        size_t initialFrameIndex = FindFirstActiveLipSyncFrame(frames);
        if (initialFrameIndex >= frames.size()) {
            const std::vector<int> phonemes = BuildPhonemeSequence(line.text);
            for (int phoneme : phonemes) {
                if (phoneme >= 0) {
                    frames[0].phoneme = phoneme;
                    frames[0].intensity = 85;
                    frames[0].decayIntensity = 20;
                    initialFrameIndex = 0;
                    Log("SpeakManager: Lip sync synthesized first active frame for %s ref=0x%08X phoneme=%d",
                        line.actor.c_str(),
                        formId,
                        phoneme);
                    break;
                }
            }
        }
        if (initialFrameIndex >= frames.size()) {
            Log("SpeakManager: Lip sync skipped for %s ref=0x%08X because no active phoneme frame was available",
                line.actor.c_str(),
                formId);
            WriteLipSyncStatus(std::string("status=skipped reason=no_active_phoneme speaker=") + line.actor + " ref=" + ConsoleFormId(formId));
            return;
        }

        const bool primedLipSync = PrimeLipSyncNow(formId, frames, initialFrameIndex, line.actor);
        if (!primedLipSync) {
            WriteLipSyncStatus(std::string("status=skipped reason=native_facegen_unavailable speaker=") +
                               line.actor + " ref=" + ConsoleFormId(formId));
            return;
        }

        {
            std::lock_guard<std::mutex> lock(g_lipSyncMutex);
            g_lipSyncActorFormId = formId;
            g_lipSyncFrames = std::move(frames);
            g_lipSyncFrameIndex = initialFrameIndex;
            if (g_lipSyncFrameIndex < g_lipSyncFrames.size()) {
                g_lipSyncLastPhoneme = g_lipSyncFrames[g_lipSyncFrameIndex].phoneme;
                g_lipSyncLastIntensity = std::max(85, g_lipSyncFrames[g_lipSyncFrameIndex].intensity);
                g_lipSyncLastCommandAt = std::chrono::steady_clock::now();
            } else {
                g_lipSyncLastPhoneme = -2;
                g_lipSyncLastIntensity = -1;
                g_lipSyncLastCommandAt = {};
            }
            g_lipSyncConsecutiveNativeFailures = 0;
            g_lipSyncRuntimeGeneration = runtimeGeneration;
            g_lipSyncAudioGeneration = audioGeneration;
            g_lipSyncLineSequence = line.sequence;
            g_lipSyncActive = true;
        }

        Log("SpeakManager: Lip sync started for %s ref=0x%08X frames=%zu initialFrame=%zu rechatDepth=%d listener=%s target=%s",
            line.actor.c_str(),
            formId,
            frameCount,
            initialFrameIndex,
            line.rechatDepth,
            line.listenerHint.c_str(),
            line.rechatTargetHint.c_str());
        WriteLipSyncStatus(std::string("status=started speaker=") + line.actor +
                           " ref=" + ConsoleFormId(formId) +
                           " frames=" + std::to_string(frameCount) +
                           " initial=" + std::to_string(initialFrameIndex) +
                           " native=1");
    }

    static void RecordNativeLipSyncSuccess(uint32_t formId) {
        std::lock_guard<std::mutex> lock(g_lipSyncMutex);
        if (g_lipSyncActive && g_lipSyncActorFormId == formId) {
            g_lipSyncConsecutiveNativeFailures = 0;
        }
    }

    static bool RecordNativeLipSyncFailure(uint32_t formId) {
        std::lock_guard<std::mutex> lock(g_lipSyncMutex);
        if (!g_lipSyncActive || g_lipSyncActorFormId != formId) {
            return false;
        }
        ++g_lipSyncConsecutiveNativeFailures;
        if (g_lipSyncConsecutiveNativeFailures < kMaxConsecutiveNativeLipSyncFailures) {
            return false;
        }
        g_lipSyncActive = false;
        return true;
    }

    static void DisableLipSyncAfterNativeFailures(uint32_t formId) {
        Log("SpeakManager: Lip sync disabled for ref=0x%08X after %d consecutive native FaceGen failures; "
            "avoiding high-frequency MFG script fallback",
            formId,
            kMaxConsecutiveNativeLipSyncFailures);
        FinalizeLipSync("native_facegen_failure");
        WriteLipSyncStatus(std::string("status=disabled reason=repeated_native_facegen_failure ref=") +
                           ConsoleFormId(formId));
    }

    static void UpdateLipSyncBridge() {
        uint32_t formId = 0;
        int phoneme = -1;
        int intensity = 0;
        int decayIntensity = 0;
        bool shouldWrite = false;
        bool shouldReset = false;
        bool staleSession = false;
        const char* staleReason = nullptr;

        {
            std::lock_guard<std::mutex> lock(g_lipSyncMutex);
            if (!g_lipSyncActive || g_lipSyncActorFormId == 0 || g_lipSyncFrames.empty()) {
                return;
            }

            if (g_lipSyncRuntimeGeneration == 0 ||
                !RuntimeGeneration::IsCurrent(g_lipSyncRuntimeGeneration)) {
                staleSession = true;
                staleReason = "runtime_generation";
            } else if (g_lipSyncAudioGeneration != g_audioGeneration.load()) {
                staleSession = true;
                staleReason = "audio_generation";
            } else if (!g_currentPlaybackLineActive ||
                       (g_lipSyncLineSequence != 0 &&
                        g_currentPlaybackLine.sequence != g_lipSyncLineSequence) ||
                       (g_currentSpeakerFormId != 0 &&
                        g_currentSpeakerFormId != g_lipSyncActorFormId)) {
                staleSession = true;
                staleReason = "playback_line";
            }

            if (staleSession) {
                shouldWrite = false;
            } else {

                const double elapsed = AudioManager::GetCurrentPlayTime();
                while (g_lipSyncFrameIndex + 1 < g_lipSyncFrames.size() &&
                       elapsed >= g_lipSyncFrames[g_lipSyncFrameIndex].endSeconds) {
                    ++g_lipSyncFrameIndex;
                }

                if (elapsed > g_lipSyncFrames.back().endSeconds + 0.1) {
                    formId = g_lipSyncActorFormId;
                    shouldReset = true;
                } else {
                    const LipSyncFrame& frame = g_lipSyncFrames[g_lipSyncFrameIndex];
                    formId = g_lipSyncActorFormId;
                    phoneme = frame.phoneme;
                    intensity = frame.intensity;
                    decayIntensity = frame.decayIntensity;

                    const auto now = std::chrono::steady_clock::now();
                    // Keep native FaceGen smooth without returning to per-frame writes.
                    // The default 500 ms resolution maps to a 20 Hz mouth update cadence.
                    const int cadenceMs = std::clamp(g_animationResolution / 10, 30, 80);
                    const bool cadenceElapsed = g_lipSyncLastCommandAt.time_since_epoch().count() == 0 ||
                        now - g_lipSyncLastCommandAt >= std::chrono::milliseconds(cadenceMs);
                    const bool mouthStateChanged =
                        phoneme != g_lipSyncLastPhoneme ||
                        std::abs(intensity - g_lipSyncLastIntensity) >= 8;
                    shouldWrite = cadenceElapsed && mouthStateChanged;

                    if (shouldWrite) {
                        g_lipSyncLastPhoneme = phoneme;
                        g_lipSyncLastIntensity = intensity;
                        g_lipSyncLastCommandAt = now;
                    }
                }
            }
        }

        if (staleSession) {
            const std::string reason = std::string("stale_") +
                (staleReason ? staleReason : "ownership");
            FinalizeLipSync(reason.c_str());
            return;
        }

        if (shouldReset && formId != 0) {
            FinalizeLipSync("frames_complete");
            return;
        }

        if (!shouldWrite || formId == 0) {
            return;
        }

        if (phoneme < 0 || intensity <= 0) {
            if (XNVSEAdapter::ApplyNativeFaceGenLipSync(
                    formId, phoneme, intensity, decayIntensity, false)) {
                RecordNativeLipSyncSuccess(formId);
            } else if (RecordNativeLipSyncFailure(formId)) {
                DisableLipSyncAfterNativeFailures(formId);
            }
            return;
        }

        if (XNVSEAdapter::ApplyNativeFaceGenLipSync(
                formId, phoneme, intensity, decayIntensity, false)) {
            RecordNativeLipSyncSuccess(formId);
            return;
        }
        if (RecordNativeLipSyncFailure(formId)) {
            DisableLipSyncAfterNativeFailures(formId);
        }
    }

    void Initialize() {
        Log("SpeakManager: Initializing...");
        g_preClipMs = Config::preClipMs;
        g_postClipMs = Config::postClipMs;
        g_animationResolution = Config::animationResolution;
        g_animationIntensity = Config::animationIntensity;
        Log("SpeakManager: Initialized");
    }

    void Shutdown() {
        Log("SpeakManager: Shutting down");
        TaskManager::CancelByType("audio_prepare");
        ClearFaceTargetBridge();
        ClearDialogueGuardBridge();
        FinalizeLipSync("shutdown");
        std::lock_guard<std::mutex> lock(g_queueMutex);
        while (!g_scriptQueue.empty()) {
            g_scriptQueue.pop();
        }
        {
            std::lock_guard<std::mutex> pendingLock(g_pendingMutex);
            g_pendingAudioQueue.clear();
            g_pendingLineSequences.clear();
        }
    }

    void InsertInQueue(ScriptLine scriptLine) {
        if (scriptLine.sequence == 0) {
            scriptLine.sequence = g_nextLineSequence++;
        }
        std::lock_guard<std::mutex> lock(g_queueMutex);
        g_scriptQueue.push(scriptLine);
        {
            std::lock_guard<std::mutex> pendingLock(g_pendingMutex);
            g_pendingLineSequences.insert(scriptLine.sequence);
        }
    }

    void InsertInQueueFront(ScriptLine scriptLine) {
        if (scriptLine.sequence == 0) {
            scriptLine.sequence = g_nextPriorityLineSequence++;
        }

        std::lock_guard<std::mutex> lock(g_queueMutex);
        std::queue<ScriptLine> reordered;
        reordered.push(scriptLine);
        while (!g_scriptQueue.empty()) {
            reordered.push(g_scriptQueue.front());
            g_scriptQueue.pop();
        }
        g_scriptQueue = std::move(reordered);

        {
            std::lock_guard<std::mutex> pendingLock(g_pendingMutex);
            g_pendingLineSequences.insert(scriptLine.sequence);
        }
    }

    void DequeueFirstItem() {
        ScriptLine removed;
        {
            std::lock_guard<std::mutex> lock(g_queueMutex);
            if (!g_scriptQueue.empty()) {
                removed = g_scriptQueue.front();
                g_scriptQueue.pop();
            }
        }
        SendAbortDeliveryStateIfTracked(removed, "dequeue");
    }

    bool HasItems() {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        return !g_scriptQueue.empty();
    }

    int CountItems() {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        return static_cast<int>(g_scriptQueue.size());
    }

    bool GetProcessing() {
        return g_isProcessing;
    }

    void SetProcessing(bool processing) {
        g_isProcessing = processing;
    }

    void SetPreclip(float ms) { g_preClipMs = ms; }
    void SetPostclip(float ms) { g_postClipMs = ms; }
    float GetPreclip() { return g_preClipMs; }
    float GetPostclip() { return g_postClipMs; }

    void SetAnimationResolution(int ms) { g_animationResolution = ms; }
    void SetAnimationIntensity(float intensity) { g_animationIntensity = intensity; }
    int GetResolution() { return g_animationResolution; }
    float GetAnimIntensity() { return g_animationIntensity; }

    static AudioManager::Vector3 ToAudioVector(const ActorPositionResolverFNV::Vector3& value) {
        return { value.x, value.y, value.z };
    }

    static float ClampFloat(float value, float minValue, float maxValue) {
        return std::max(minValue, std::min(value, maxValue));
    }

    static float GetBaseVoiceVolume() {
        return ClampFloat(Config::voiceVolume / 100.0f, 0.0f, 1.0f);
    }

    static float GetCurrentLineVoiceVolume() {
        return HeadVoiceVolumeUtils::ApplyToLine(
            GetBaseVoiceVolume(),
            g_currentPlaybackIsHeadVoice,
            HeadVoiceVolumeUtils::PercentToMultiplier(Config::headVoiceVolume));
    }

    static float ClampPlaybackDropoffPercent(float percent) {
        if (!std::isfinite(percent)) {
            return 70.0f;
        }

        return ClampFloat(percent, 25.0f, 200.0f);
    }

    static float ApplyDropoffAggressiveness(float volume, float dropoffPercent) {
        const float clampedVolume = ClampFloat(volume, 0.0f, 1.0f);
        const float attenuation = 1.0f - clampedVolume;
        const float attenuationScale = ClampPlaybackDropoffPercent(dropoffPercent) / 100.0f;
        const float tunedAttenuation = attenuation * attenuationScale;
        return ClampFloat(1.0f - tunedAttenuation, 0.0f, 1.0f);
    }

    static float CalculatePlaybackVolume(
        const ActorPositionResolverFNV::PositionResult& speaker,
        const ActorPositionResolverFNV::PositionResult& listener,
        const SpatialAwarenessFNV::Result& spatial) {
        const float baseVolume = GetCurrentLineVoiceVolume();
        if (!speaker.resolved || !listener.resolved) {
            return baseVolume;
        }

        const bool interiorPlayback = speaker.interiorKnown && listener.interiorKnown &&
            speaker.isInterior && listener.isInterior && spatial.sameCell;
        const float dropoffPercent = interiorPlayback
            ? Config::audioPlaybackDropoffInteriorPercent
            : Config::audioPlaybackDropoffExteriorPercent;

        const bool closedDoorBarrier = spatial.closedDoorCount > 0 ||
            spatial.reason == "closed_door_between";
        const bool hardBarrier = closedDoorBarrier ||
            spatial.reason == "different_interior_cells" ||
            spatial.reason == "interior_exterior_boundary";

        float spatialVolume = 1.0f;
        if (spatial.canCommunicate) {
            float sourceVolume = spatial.volume;
            if (sourceVolume <= 0.0f && spatial.maxDistance > 0.0f) {
                const float normalizedDistance = ClampFloat(spatial.airDistance / spatial.maxDistance, 0.0f, 1.0f);
                const float falloff = 1.0f - normalizedDistance;
                sourceVolume = std::pow(falloff, std::max(0.25f, Config::audioDistanceScale));
            }
            spatialVolume = ApplyDropoffAggressiveness(ClampFloat(sourceVolume, 0.35f, 1.0f), dropoffPercent);
        } else if (hardBarrier) {
            spatialVolume = 0.15f;
        } else {
            spatialVolume = 0.25f;
        }

        float navmeshPenalty = 1.0f;
        if (spatial.navmeshPathUsed) {
            if (spatial.navmeshPathFound && std::isfinite(spatial.pathRatio) && spatial.pathRatio > 1.0f) {
                navmeshPenalty = ClampFloat(1.0f / (1.0f + (spatial.pathRatio - 1.0f) * 0.05f), 0.95f, 1.0f);
            } else if (!spatial.navmeshPathFound) {
                navmeshPenalty = 0.90f;
            }
        }

        float losPenalty = 1.0f;
        if (spatial.losQueryOk && !spatial.hasLineOfSight) {
            losPenalty = 0.85f;
        }
        if (closedDoorBarrier) {
            losPenalty = std::min(losPenalty, 0.55f);
        }

        const float minPlaybackFloor = closedDoorBarrier ? 0.06f : 0.20f;
        const float multiplier = ClampFloat(spatialVolume * navmeshPenalty * losPenalty, minPlaybackFloor, 1.0f);
        return baseVolume * multiplier;
    }

    static void LogSpatialState(
        const std::string& key,
        const ActorPositionResolverFNV::PositionResult& speaker,
        const ActorPositionResolverFNV::PositionResult& listener,
        const SpatialAwarenessFNV::Result& spatial) {
        const auto now = std::chrono::steady_clock::now();
        if (key == g_lastSpatialLogKey &&
            now - g_lastSpatialLogTime < std::chrono::seconds(2)) {
            return;
        }

        g_lastSpatialLogKey = key;
        g_lastSpatialLogTime = now;

        if (speaker.resolved && listener.resolved) {
            Log("SpeakManager: AudioPlayback speaker=%s ref=0x%08X awareness=%s resolver=%s distance=%.1f max=%.1f awarenessVolume=%.2f source=(%.1f, %.1f, %.1f) listener=(%.1f, %.1f, %.1f) yaw=%.3f",
                g_currentSpeaker.c_str(),
                g_currentSpeakerFormId,
                spatial.reason.c_str(),
                speaker.source.c_str(),
                spatial.airDistance,
                spatial.maxDistance,
                spatial.volume,
                speaker.position.x,
                speaker.position.y,
                speaker.position.z,
                listener.position.x,
                listener.position.y,
                listener.position.z,
                listener.yaw);
        } else {
            Log("SpeakManager: AudioPlayback fallback speaker=%s ref=0x%08X sourceResolved=%d listenerResolved=%d reason=%s/%s",
                g_currentSpeaker.c_str(),
                g_currentSpeakerFormId,
                speaker.resolved ? 1 : 0,
                listener.resolved ? 1 : 0,
                speaker.reason.c_str(),
                listener.reason.c_str());
        }
    }

    static void UpdateCurrentSpatialPlayback() {
        if (!AudioManager::IsPlaying()) {
            g_currentSpeakerFormId = 0;
            g_currentSpeaker.clear();
            g_currentPlaybackIsHeadVoice = false;
            g_lastSpatialPlaybackUpdateTime = {};
            g_lastSpatialPlaybackSpeakerFormId = 0;
            AudioManager::Set3DPlaybackEnabled(false);
            return;
        }

        if (g_currentSpeakerFormId == 0) {
            g_lastSpatialPlaybackUpdateTime = {};
            g_lastSpatialPlaybackSpeakerFormId = 0;
            AudioManager::Set3DPlaybackEnabled(false);
            AudioManager::SetVolume(GetCurrentLineVoiceVolume());
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        if (g_lastSpatialPlaybackSpeakerFormId == g_currentSpeakerFormId &&
            g_lastSpatialPlaybackUpdateTime.time_since_epoch().count() != 0 &&
            now - g_lastSpatialPlaybackUpdateTime < kSpatialPlaybackUpdateInterval) {
            return;
        }
        g_lastSpatialPlaybackUpdateTime = now;
        g_lastSpatialPlaybackSpeakerFormId = g_currentSpeakerFormId;

        auto speaker = ActorPositionResolverFNV::ResolveActor(g_currentSpeakerFormId);
        auto listener = ActorPositionResolverFNV::ResolvePlayer();
        auto spatial = SpatialAwarenessFNV::Evaluate(speaker, listener);

        std::ostringstream key;
        key << g_currentSpeakerFormId << ":" << spatial.reason << ":"
            << static_cast<int>(spatial.airDistance / 100.0f) << ":"
            << (speaker.resolved ? 1 : 0) << ":" << (listener.resolved ? 1 : 0);
        LogSpatialState(key.str(), speaker, listener, spatial);

        if (!speaker.resolved || !listener.resolved) {
            AudioManager::Set3DPlaybackEnabled(false);
            AudioManager::SetVolume(GetBaseVoiceVolume());
            return;
        }

        AudioManager::Vector3 listenerForward = { 0.0f, 1.0f, 0.0f };
        float listenerYaw = listener.yawResolved ? listener.yaw : 0.0f;
        if (Config::audioInvertHeading) {
            listenerYaw += 3.14159265f;
        }

        AudioManager::Set3DPlaybackEnabled(Config::audio3DPlaybackEnabled);
        AudioManager::SetCameraBasedAudio(Config::audioCameraBased);
        AudioManager::Set3DPlaybackStrength(Config::audio3DPanStrength);
        AudioManager::Update(
            ToAudioVector(speaker.position),
            ToAudioVector(listener.position),
            listenerForward,
            listenerYaw);

        AudioManager::SetVolume(CalculatePlaybackVolume(speaker, listener, spatial));
    }

    void Abort() {
        g_aborted = true;
        g_audioGeneration.fetch_add(1);
        HTTPManager::CancelPendingResponses();
        AudioManager::Stop();
        AudioManager::Set3DPlaybackEnabled(false);
        ClearFaceTargetBridge();
        ClearDialogueGuardBridge();
        FinalizeLipSync("abort");
    }

    bool IsAborted() {
        bool result = g_aborted;
        g_aborted = false;
        return result;
    }

    bool BeginRechatAttempt(const std::string& speaker) {
        const std::string cleanSpeaker = Trim(speaker);
        if (cleanSpeaker.empty()) {
            return false;
        }

        std::lock_guard<std::mutex> lock(g_rechatMutex);
        g_rechatInFlight = true;
        g_rechatInFlightSpeaker = cleanSpeaker;
        return true;
    }

    void QueueRechatRetry(const std::string& speaker,
                          const std::string& listenerHint,
                          const std::string& explicitTarget,
                          const std::string& originLine,
                          int rechatDepth,
                          uint32_t speakerFormId,
                          uint32_t listenerFormId,
                          uint32_t targetFormId) {
        const std::string cleanSpeaker = Trim(speaker);
        if (cleanSpeaker.empty()) {
            return;
        }

        std::lock_guard<std::mutex> lock(g_rechatMutex);
        if (!g_rechatInFlight || g_rechatInFlightSpeaker != cleanSpeaker) {
            return;
        }

        g_pendingRechatRetry.active = true;
        g_pendingRechatRetry.speaker = cleanSpeaker;
        g_pendingRechatRetry.listenerHint = Trim(listenerHint);
        g_pendingRechatRetry.explicitTarget = Trim(explicitTarget);
        g_pendingRechatRetry.originLine = originLine;
        g_pendingRechatRetry.rechatDepth = rechatDepth;
        g_pendingRechatRetry.speakerFormId = speakerFormId;
        g_pendingRechatRetry.listenerFormId = listenerFormId;
        g_pendingRechatRetry.targetFormId = targetFormId;
    }

    void CompleteRechatAttempt(const std::string& speaker, bool success) {
        PendingRechatRetry retry;
        bool shouldRetry = false;
        const std::string cleanSpeaker = Trim(speaker);

        {
            std::lock_guard<std::mutex> lock(g_rechatMutex);
            if (!g_rechatInFlight || g_rechatInFlightSpeaker != cleanSpeaker) {
                return;
            }

            g_rechatInFlight = false;
            g_rechatInFlightSpeaker.clear();

            if (success) {
                g_rechatChainClosed = false;
                g_lastRechatter = cleanSpeaker;
                g_pendingRechatRetry = PendingRechatRetry{};
                return;
            }

            if (Config::rechatRetryOnEmpty &&
                g_pendingRechatRetry.active &&
                g_pendingRechatRetry.speaker == cleanSpeaker) {
                retry = g_pendingRechatRetry;
                shouldRetry = true;
            }

            if (!shouldRetry) {
                g_rechatChainClosed = true;
                g_rechatCooldownUntil = std::chrono::steady_clock::now() +
                    std::chrono::seconds(Config::rechatEndConversationCooldown);
            }
            g_pendingRechatRetry = PendingRechatRetry{};
        }

        if (shouldRetry &&
            Rechat(retry.speaker,
                   retry.listenerHint,
                   retry.rechatDepth,
                   retry.originLine,
                   retry.explicitTarget,
                   retry.speakerFormId,
                   retry.listenerFormId,
                   retry.targetFormId) > 0) {
            BeginRechatAttempt(retry.speaker);
        }
    }

    bool IsRechatInFlightFor(const std::string& speaker) {
        std::lock_guard<std::mutex> lock(g_rechatMutex);
        return g_rechatInFlight && g_rechatInFlightSpeaker == Trim(speaker);
    }

    void ResetRechatChainState() {
        std::lock_guard<std::mutex> lock(g_rechatMutex);
        g_rechatInFlight = false;
        g_rechatInFlightSpeaker.clear();
        g_rechatChainClosed = false;
        g_rechatChainAutonomous = false;
        g_rechatChainId.clear();
        g_lastRechatter.clear();
        g_pendingRechatRetry = PendingRechatRetry{};
        g_pendingRechatLaunchLine = ScriptLine{};
        g_pendingRechatLaunchActive = false;
        g_pendingRechatLaunchUntil = {};
        g_pendingRechatNextCheck = {};
    }

    void StartRechatChainForAutonomousEvent() {
        ResetRechatChainState();
        {
            std::lock_guard<std::mutex> lock(g_rechatMutex);
            g_rechatChainAutonomous = true;
        }
        Log("SpeakManager: Opened a fresh rechat chain from autonomous event");
    }

    bool IsRechatChainClosed() {
        std::lock_guard<std::mutex> lock(g_rechatMutex);
        return g_rechatChainClosed;
    }

    std::string GetLastRechatter() {
        std::lock_guard<std::mutex> lock(g_rechatMutex);
        return g_lastRechatter;
    }

    void SetLastRechatter(const std::string& speaker) {
        std::lock_guard<std::mutex> lock(g_rechatMutex);
        g_lastRechatter = Trim(speaker);
    }

    std::string EnsureRechatChainId(const std::string& speaker,
                                    const std::string& listenerHint,
                                    const std::string& explicitTarget) {
        std::lock_guard<std::mutex> lock(g_rechatMutex);
        if (g_rechatChainId.empty()) {
            g_rechatChainId = NewRechatChainId();
            Log("SpeakManager: Created rechat chain %s speaker=%s listener=%s target=%s",
                g_rechatChainId.c_str(),
                Trim(speaker).c_str(),
                Trim(listenerHint).c_str(),
                Trim(explicitTarget).c_str());
        }
        return g_rechatChainId;
    }

    static std::vector<std::string> BuildRechatAudienceNames(const std::string& speaker,
                                                             const std::string& listenerHint,
                                                             const std::string& targetHint,
                                                             uint32_t speakerFormId);

    int Rechat(const std::string& speaker,
               const std::string& listenerHint,
               int rechatDepth,
               const std::string& originLine,
               const std::string& explicitTarget,
               uint32_t speakerFormId,
               uint32_t listenerFormId,
               uint32_t targetFormId) {
        const std::string cleanSpeaker = Trim(speaker);
        const std::string cleanListener = Trim(listenerHint);
        const std::string cleanTarget = Trim(explicitTarget);

        if (!Config::rechatEnabled) {
            Log("SpeakManager: Rechat skipped because plugin rechat is disabled");
            WriteRechatStatus("skipped", cleanSpeaker, "request", "plugin_rechat_disabled", cleanTarget);
            return 0;
        }
        if (EqualsIgnoreCase(Config::currentMode, "WHISPER") ||
            EqualsIgnoreCase(Config::currentMode, "CLOSE")) {
            Log("SpeakManager: Rechat skipped for %s because %s mode is private",
                cleanSpeaker.c_str(), Config::currentMode.c_str());
            WriteRechatStatus("skipped", cleanSpeaker, "request", "private_mode", cleanTarget);
            return 0;
        }
        if (cleanSpeaker.empty()) {
            Log("SpeakManager: Rechat skipped because speaker is empty");
            WriteRechatStatus("skipped", cleanSpeaker, "request", "speaker_empty", cleanTarget);
            return 0;
        }
        if (speakerFormId == 0) {
            Log("SpeakManager: Rechat skipped for %s because speaker form id is missing",
                cleanSpeaker.c_str());
            WriteRechatStatus("skipped", cleanSpeaker, "request", "speaker_formid_missing", cleanTarget);
            return 0;
        }
        if (rechatDepth >= Config::rechatMaxDepth) {
            Log("SpeakManager: Rechat skipped for %s because max depth %d was reached",
                cleanSpeaker.c_str(), Config::rechatMaxDepth);
            WriteRechatStatus("skipped", cleanSpeaker, "request", "max_depth_reached", cleanTarget);
            return 0;
        }

        ActorPositionResolverFNV::PositionResult speakerPosition;
        if (speakerFormId != 0) {
            speakerPosition = ActorPositionResolverFNV::ResolveActor(speakerFormId);
            std::string reason;
            if (!IsRechatPositionEligible(speakerPosition, &reason)) {
                Log("SpeakManager: Rechat skipped for %s (0x%08X): %s",
                    cleanSpeaker.c_str(),
                    speakerFormId,
                    reason.c_str());
                WriteRechatStatus("skipped", cleanSpeaker, "request", reason, cleanTarget);
                return 0;
            }
        }

        if (targetFormId != 0 && speakerPosition.resolved) {
            if (!GameLoop::IsCombatDialogueAllowed(targetFormId)) {
                Log("SpeakManager: Rechat skipped for %s target %s (0x%08X): combat dialogue is disabled",
                    cleanSpeaker.c_str(),
                    cleanTarget.c_str(),
                    targetFormId);
                WriteRechatStatus("skipped", cleanSpeaker, "request",
                    "target_combat_dialogue_disabled", cleanTarget);
                return 0;
            }

            auto targetPosition = ActorPositionResolverFNV::ResolveActor(targetFormId);
            std::string reason;
            if (!IsRechatPositionEligible(targetPosition, &reason)) {
                Log("SpeakManager: Rechat skipped for %s target %s (0x%08X): %s",
                    cleanSpeaker.c_str(),
                    cleanTarget.c_str(),
                    targetFormId,
                    reason.c_str());
                WriteRechatStatus("skipped", cleanSpeaker, "request", reason, cleanTarget);
                return 0;
            }

            const auto spatial = SpatialAwarenessFNV::Evaluate(speakerPosition, targetPosition);
            if (!spatial.canCommunicate) {
                Log("SpeakManager: Rechat skipped for %s target %s (0x%08X): cannot communicate reason=%s distance=%.1f max=%.1f",
                    cleanSpeaker.c_str(),
                    cleanTarget.c_str(),
                    targetFormId,
                    spatial.reason.c_str(),
                    spatial.airDistance,
                    spatial.maxDistance);
                WriteRechatStatus("skipped", cleanSpeaker, "request", "target_cannot_communicate:" + spatial.reason, cleanTarget);
                return 0;
            }
        }

        {
            std::lock_guard<std::mutex> lock(g_rechatMutex);
            if (g_rechatChainClosed) {
                if (std::chrono::steady_clock::now() < g_rechatCooldownUntil) {
                    Log("SpeakManager: Rechat skipped for %s because chain is closed and cooldown is active",
                        cleanSpeaker.c_str());
                    WriteRechatStatus("skipped", cleanSpeaker, "request", "chain_closed_cooldown", cleanTarget);
                    return 0;
                }

                g_rechatChainClosed = false;
                g_rechatChainId.clear();
                g_lastRechatter.clear();
            }
            if (std::chrono::steady_clock::now() < g_rechatCooldownUntil) {
                Log("SpeakManager: Rechat skipped for %s because cooldown is active", cleanSpeaker.c_str());
                WriteRechatStatus("skipped", cleanSpeaker, "request", "cooldown_active", cleanTarget);
                return 0;
            }
        }

        const std::string chainId = EnsureRechatChainId(cleanSpeaker, cleanListener, cleanTarget);
        const std::string resolvedTarget = cleanTarget.empty() ? cleanListener : cleanTarget;
        const std::vector<std::string> audienceNames =
            BuildRechatAudienceNames(cleanSpeaker, cleanListener, cleanTarget, speakerFormId);
        const std::string audienceJson = JsonStringArray(audienceNames);
        const std::string audiencePipe = AudiencePeoplePipe(audienceNames);

        std::ostringstream payload;
        payload << "{"
                << "\"speaker\":\"" << HTTPManager::EscapeJson(cleanSpeaker) << "\","
                << "\"listener_hint\":\"" << HTTPManager::EscapeJson(cleanListener) << "\","
                << "\"rechat_target_hint\":\"" << HTTPManager::EscapeJson(cleanTarget) << "\","
                << "\"resolved_rechat_target\":\"" << HTTPManager::EscapeJson(resolvedTarget) << "\","
                << "\"origin_line\":\"" << HTTPManager::EscapeJson(originLine) << "\","
                << "\"rechat_depth\":" << rechatDepth << ","
                << "\"audience\":" << audienceJson << ","
                << "\"chain_members\":" << audienceJson << ","
                << "\"audience_snapshot\":{\"people\":\"" << HTTPManager::EscapeJson(audiencePipe) << "\"},"
                << "\"chain_id\":\"" << HTTPManager::EscapeJson(chainId) << "\","
                << "\"scene_key\":\"" << HTTPManager::EscapeJson(CurrentPlayerSceneKey()) << "\"";

        if (speakerFormId != 0) {
            payload << ",\"speaker_formid\":\"" << HexFormId(speakerFormId) << "\"";
        }
        if (listenerFormId != 0) {
            payload << ",\"listener_formid\":\"" << HexFormId(listenerFormId) << "\"";
        }
        if (targetFormId != 0) {
            payload << ",\"target_formid\":\"" << HexFormId(targetFormId) << "\"";
        }
        payload << "}";

        Log("SpeakManager: Rechat request speaker=%s listener=%s target=%s depth=%d chain=%s speakerForm=%s listenerForm=%s targetForm=%s audience=%s",
            cleanSpeaker.c_str(),
            cleanListener.c_str(),
            cleanTarget.c_str(),
            rechatDepth,
            chainId.c_str(),
            HexFormId(speakerFormId).c_str(),
            HexFormId(listenerFormId).c_str(),
            HexFormId(targetFormId).c_str(),
            audiencePipe.c_str());
        WriteRechatStatus("sent", cleanSpeaker, "request", "sent_to_server", cleanTarget);

        BeginRechatAttempt(cleanSpeaker);
        if (speakerFormId != 0) {
            GuardActorForPendingDialogue(speakerFormId, cleanSpeaker);
        }
        HTTPManager::SendEvent("rechat", payload.str());
        return 1;
    }

    static bool HasPendingAudioReady() {
        std::lock_guard<std::mutex> pendingLock(g_pendingMutex);
        return !g_pendingAudioQueue.empty();
    }

    static int PendingAudioWorkCountLocked() {
        return static_cast<int>(g_pendingAudioQueue.size()) + g_downloadsInProgress;
    }

    static int PendingAudioWorkCount() {
        std::lock_guard<std::mutex> pendingLock(g_pendingMutex);
        return PendingAudioWorkCountLocked();
    }

    static void CompleteAudioDownload(PendingAudio audio, uint64_t generation) {
        std::lock_guard<std::mutex> pendingLock(g_pendingMutex);
        if (g_downloadsInProgress > 0) {
            --g_downloadsInProgress;
        }
        if (generation != g_audioGeneration.load()) {
            return;
        }
        if (audio.ready && !audio.audioData.empty()) {
            g_audioReady.fetch_add(1, std::memory_order_relaxed);
            Log("SpeakManager: utterance state=audio_ready speaker='%s' utterance='%s' bytes=%zu",
                audio.line.actor.c_str(),
                audio.line.utteranceId.c_str(),
                audio.audioData.size());
            g_pendingAudioQueue.push_back(std::move(audio));
        } else if (audio.line.sequence != 0) {
            g_pendingLineSequences.erase(audio.line.sequence);
        }
    }

    static void CompleteFailedAudioDownload(const ScriptLine& item, uint64_t generation) {
        bool queuedTextFallback = false;
        if (generation == g_audioGeneration.load() && !item.text.empty() && !item.actor.empty()) {
            g_audioFailed.fetch_add(1, std::memory_order_relaxed);
        }
        {
            std::lock_guard<std::mutex> pendingLock(g_pendingMutex);
            if (g_downloadsInProgress > 0) {
                --g_downloadsInProgress;
            }
            if (generation == g_audioGeneration.load() && item.sequence != 0 &&
                !item.text.empty() && !item.actor.empty()) {
                PendingAudio fallback;
                fallback.speaker = item.actor;
                fallback.actorFormId = item.actorFormId;
                fallback.line = item;
                fallback.line.textOnlyFallback = true;
                fallback.ready = true;
                fallback.textOnlyFallback = true;
                g_pendingAudioQueue.push_back(std::move(fallback));
                queuedTextFallback = true;
            } else if (generation == g_audioGeneration.load() && item.sequence != 0) {
                g_pendingLineSequences.erase(item.sequence);
            }
        }
        if (!queuedTextFallback && !AudioManager::IsPlaying() && !g_currentPlaybackLineActive) {
            ClearDialogueGuardBridgeIfIdle();
        }
    }

    static bool PopNextReadyAudioLocked(PendingAudio& readyAudio) {
        if (g_pendingAudioQueue.empty() || g_pendingLineSequences.empty()) {
            return false;
        }

        const uint64_t nextSequence = *g_pendingLineSequences.begin();
        for (auto it = g_pendingAudioQueue.begin(); it != g_pendingAudioQueue.end(); ++it) {
            if (it->ready && (!it->audioData.empty() || it->textOnlyFallback) &&
                it->line.sequence == nextSequence) {
                readyAudio = std::move(*it);
                g_pendingAudioQueue.erase(it);
                g_pendingLineSequences.erase(nextSequence);
                return true;
            }
        }

        return false;
    }

    QueueStatus GetQueueStatus() {
        QueueStatus status;
        status.audioGeneration = g_audioGeneration.load();
        status.isProcessing = g_isProcessing;
        status.isPlaying = AudioManager::IsPlaying();
        status.currentPlaybackLineActive = g_currentPlaybackLineActive;
        status.currentSpeaker = g_currentSpeaker;
        status.currentTextPreview = PreviewText(g_currentPlaybackLine.text);
        {
            std::lock_guard<std::mutex> lock(g_queueMutex);
            status.dialogueLinesQueued = static_cast<int>(g_scriptQueue.size());
        }
        {
            std::lock_guard<std::mutex> pendingLock(g_pendingMutex);
            status.ttsDownloadsInProgress = g_downloadsInProgress;
            status.preparedAudioCount = static_cast<int>(g_pendingAudioQueue.size());
        }
        const TaskManager::Snapshot tasks = TaskManager::GetSnapshot();
        for (const auto& type : tasks.types) {
            if (type.type != "audio_prepare") continue;
            status.ttsTasksPending = static_cast<int>(type.pending);
            status.ttsTasksActive = static_cast<int>(type.active);
            status.totalTtsTasksQueued = type.queued;
            status.totalTtsTasksCompleted = type.completed;
            break;
        }
        return status;
    }

    SpeechDiagnostics GetDiagnostics() {
        SpeechDiagnostics diagnostics;
        diagnostics.dialogueLinesReceived = g_dialogueLinesReceived.load(std::memory_order_relaxed);
        diagnostics.playerAudioLinesReceived = g_playerAudioLinesReceived.load(std::memory_order_relaxed);
        diagnostics.playerTextOnlyLinesReceived = g_playerTextOnlyLinesReceived.load(std::memory_order_relaxed);
        diagnostics.audioPrepareStarted = g_audioPrepareStarted.load(std::memory_order_relaxed);
        diagnostics.audioReady = g_audioReady.load(std::memory_order_relaxed);
        diagnostics.audioFailed = g_audioFailed.load(std::memory_order_relaxed);
        diagnostics.playbackStarted = g_playbackStarted.load(std::memory_order_relaxed);
        diagnostics.playbackCompleted = g_playbackCompleted.load(std::memory_order_relaxed);
        diagnostics.playbackFailed = g_playbackFailed.load(std::memory_order_relaxed);
        diagnostics.lipSyncCommandsRequested = g_lipSyncCommandsRequested.load(std::memory_order_relaxed);
        diagnostics.nativeMfgApplied = g_nativeMfgApplied.load(std::memory_order_relaxed);
        diagnostics.scriptMfgFallbacks = g_scriptMfgFallbacks.load(std::memory_order_relaxed);
        diagnostics.subtitleScriptFallbacks = g_subtitleScriptFallbacks.load(std::memory_order_relaxed);
        diagnostics.nativeDialogueGuards = g_nativeDialogueGuards.load(std::memory_order_relaxed);
        diagnostics.scriptDialogueGuards = g_scriptDialogueGuards.load(std::memory_order_relaxed);
        diagnostics.dialogueTurnCancellations = g_dialogueTurnCancellations.load(std::memory_order_relaxed);
        return diagnostics;
    }

    bool IsRecentAISubtitleText(const std::string& text) {
        const std::string key = NormalizeSubtitleCompareKey(text);
        if (key.empty()) {
            return false;
        }

        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(g_recentSubtitleMutex);
        PruneRecentAiSubtitleKeysLocked(now);
        for (const auto& item : g_recentAiSubtitleKeys) {
            if (item.first == key) {
                return true;
            }
        }
        return false;
    }

    static bool HasSceneGuardedDialogueWork() {
        QueueStatus speech = GetQueueStatus();
        HTTPManager::QueueStatus http = HTTPManager::GetQueueStatus();
        bool rechatActive = false;
        {
            std::lock_guard<std::mutex> lock(g_rechatMutex);
            rechatActive = g_rechatInFlight || g_pendingRechatRetry.active || !g_rechatChainId.empty();
        }

        return speech.isPlaying ||
            speech.currentPlaybackLineActive ||
            speech.dialogueLinesQueued > 0 ||
            speech.ttsDownloadsInProgress > 0 ||
            speech.ttsTasksPending > 0 ||
            speech.ttsTasksActive > 0 ||
            speech.preparedAudioCount > 0 ||
            http.streamInProgress ||
            http.activeStreamTasks > 0 ||
            http.pendingHttpTasks > 0 ||
            http.activeHttpTasks > 0 ||
            http.httpResponsesQueued > 0 ||
            rechatActive;
    }

    static void ClearSpeechForSceneChange(const std::string& oldSceneKey,
                                          const std::string& newSceneKey) {
        Log("SpeakManager: Player scene changed from %s to %s; cancelling stale dialogue/rechat",
            oldSceneKey.c_str(),
            newSceneKey.c_str());

        g_audioGeneration.fetch_add(1);
        HTTPManager::CancelPendingResponses();
        if (g_currentPlaybackLineActive) {
            SendAbortDeliveryStateIfTracked(g_currentPlaybackLine, "scene_change");
        }
        AudioManager::Stop();
        AudioManager::Set3DPlaybackEnabled(false);
        AudioManager::SetVolume(GetBaseVoiceVolume());
        FinalizeLipSync("scene_change");
        ClearFaceTargetBridge();
        ClearSubtitleBridge();
        ClearDialogueGuardBridge();
        ActionManager::ClearPostDialogueActions();

        std::vector<ScriptLine> abortedLines;
        {
            std::lock_guard<std::mutex> lock(g_queueMutex);
            while (!g_scriptQueue.empty()) {
                abortedLines.push_back(g_scriptQueue.front());
                g_scriptQueue.pop();
            }
        }
        {
            std::lock_guard<std::mutex> pendingLock(g_pendingMutex);
            for (const auto& pendingAudio : g_pendingAudioQueue) {
                abortedLines.push_back(pendingAudio.line);
            }
            g_pendingAudioQueue.clear();
            g_pendingLineSequences.clear();
        }
        for (const auto& line : abortedLines) {
            SendAbortDeliveryStateIfTracked(line, "scene_change");
        }
        {
            std::lock_guard<std::mutex> lock(g_rechatMutex);
            g_rechatInFlight = false;
            g_rechatInFlightSpeaker.clear();
            g_rechatChainClosed = false;
            g_rechatChainAutonomous = false;
            g_rechatChainId.clear();
            g_lastRechatter.clear();
            g_pendingRechatRetry = PendingRechatRetry{};
        }

        g_isProcessing = false;
        g_currentSpeakerFormId = 0;
        g_currentSpeaker.clear();
        g_currentPlaybackLine = ScriptLine{};
        g_currentPlaybackLineActive = false;
        g_currentPlaybackRechatLaunched = false;
        g_currentPlaybackIsHeadVoice = false;
        g_playbackPausedForMenu = false;
        ClearPlayerInputTtsGate("scene_change");
    }

    static bool CancelDialogueIfPlayerSceneChanged() {
        const std::string currentSceneKey = CurrentPlayerSceneKey();
        if (currentSceneKey.empty()) {
            return false;
        }

        if (g_lastPlayerSceneKey.empty()) {
            g_lastPlayerSceneKey = currentSceneKey;
            return false;
        }

        if (currentSceneKey == g_lastPlayerSceneKey) {
            return false;
        }

        const std::string oldSceneKey = g_lastPlayerSceneKey;
        g_lastPlayerSceneKey = currentSceneKey;

        if (!IsHardSceneBoundaryChange(oldSceneKey, currentSceneKey)) {
            Log("SpeakManager: Player scene key changed from %s to %s; keeping dialogue because this is not a hard boundary",
                oldSceneKey.c_str(),
                currentSceneKey.c_str());
            return false;
        }

        if (!HasSceneGuardedDialogueWork()) {
            return false;
        }

        ClearSpeechForSceneChange(oldSceneKey, currentSceneKey);
        return true;
    }

    static void LogQueueStatusIfNeeded() {
        const auto now = std::chrono::steady_clock::now();

        QueueStatus speech = GetQueueStatus();
        HTTPManager::QueueStatus http = HTTPManager::GetQueueStatus();
        const auto& state = GameLoop::GetGameState();
        const bool heldForMenu = Config::pauseDialogueOnMenu && state.isPaused;
        const bool active = speech.isPlaying ||
            speech.dialogueLinesQueued > 0 ||
            speech.ttsDownloadsInProgress > 0 ||
            speech.ttsTasksPending > 0 ||
            speech.ttsTasksActive > 0 ||
            speech.preparedAudioCount > 0 ||
            http.streamInProgress ||
            http.pendingHttpTasks > 0 ||
            http.activeHttpTasks > 0 ||
            http.httpResponsesQueued > 0;

        if (g_lastQueueStatusWriteTime.time_since_epoch().count() == 0 ||
            now - g_lastQueueStatusWriteTime >= std::chrono::seconds(2)) {
            std::ostringstream status;
            status << "updated_tick_ms=" << StatusTimestampMs() << "\n"
                   << "active=" << (active ? 1 : 0) << "\n"
                   << "http_stream=" << (http.streamInProgress ? 1 : 0) << "\n"
                   << "http_active=" << http.activeStreamTasks << "\n"
                   << "http_queued=" << http.httpResponsesQueued << "\n"
                   << "http_generation=" << static_cast<unsigned long long>(http.generation) << "\n"
                   << "response_generation=" << static_cast<unsigned long long>(http.responseQueueGeneration) << "\n"
                   << "response_dialogue_queued=" << http.responseDialogueQueued << "\n"
                   << "response_actions_queued=" << http.responseActionsQueued << "\n"
                   << "response_dispatched_total=" << static_cast<unsigned long long>(http.totalResponsesDispatched) << "\n"
                   << "response_stale_dropped_total=" << static_cast<unsigned long long>(http.totalResponsesDroppedStale) << "\n"
                   << "http_tasks_pending=" << http.pendingHttpTasks << "\n"
                   << "http_tasks_active=" << http.activeHttpTasks << "\n"
                   << "http_tasks_cancelled_total=" << static_cast<unsigned long long>(http.totalHttpTasksCancelled) << "\n"
                   << "http_task_current=" << http.activeHttpTaskSummary << "\n"
                   << "dialogue_queued=" << speech.dialogueLinesQueued << "\n"
                   << "tts_downloads=" << speech.ttsDownloadsInProgress << "\n"
                   << "tts_tasks_pending=" << speech.ttsTasksPending << "\n"
                   << "tts_tasks_active=" << speech.ttsTasksActive << "\n"
                   << "tts_tasks_completed_total=" << static_cast<unsigned long long>(speech.totalTtsTasksCompleted) << "\n"
                   << "prepared_audio=" << speech.preparedAudioCount << "\n"
                   << "playing=" << (speech.isPlaying ? 1 : 0) << "\n"
                   << "current_line_active=" << (speech.currentPlaybackLineActive ? 1 : 0) << "\n"
                   << "audio_paused=" << (AudioManager::IsPaused() ? 1 : 0) << "\n"
                   << "held_for_menu=" << (heldForMenu ? 1 : 0) << "\n"
                   << "game_paused=" << (state.isPaused ? 1 : 0) << "\n"
                   << "game_in_menu=" << (state.isInMenu ? 1 : 0) << "\n"
                   << "game_in_dialogue=" << (state.isInDialogue ? 1 : 0) << "\n"
                   << "speaker=" << speech.currentSpeaker << "\n"
                   << "text=" << speech.currentTextPreview << "\n";
            WriteTextFile(kQueueStatusPath, status.str());
            g_lastQueueStatusWriteTime = now;
        }

        if (active &&
            (g_lastQueueStatusLogTime.time_since_epoch().count() == 0 ||
             now - g_lastQueueStatusLogTime >= std::chrono::seconds(5))) {
            g_lastQueueStatusLogTime = now;
            Log("QueueStatus: http_stream=%d http_active=%d http_queued=%zu http_tasks=%zu/%zu http_gen=%llu resp_gen=%llu stale=%llu "
                "dialogue=%d tts_downloads=%d tts_tasks=%d/%d prepared=%d playing=%d paused=%d in_menu=%d held=%d speaker='%s' text='%s'",
                http.streamInProgress ? 1 : 0,
                http.activeStreamTasks,
                http.httpResponsesQueued,
                http.activeHttpTasks,
                http.pendingHttpTasks,
                static_cast<unsigned long long>(http.generation),
                static_cast<unsigned long long>(http.responseQueueGeneration),
                static_cast<unsigned long long>(http.totalResponsesDroppedStale),
                speech.dialogueLinesQueued,
                speech.ttsDownloadsInProgress,
                speech.ttsTasksActive,
                speech.ttsTasksPending,
                speech.preparedAudioCount,
                speech.isPlaying ? 1 : 0,
                state.isPaused ? 1 : 0,
                state.isInMenu ? 1 : 0,
                heldForMenu ? 1 : 0,
                speech.currentSpeaker.c_str(),
                speech.currentTextPreview.c_str());
        }
    }

    static bool IsValidWavData(const std::vector<uint8_t>& audioData) {
        return audioData.size() >= 12 &&
            audioData[0] == 'R' &&
            audioData[1] == 'I' &&
            audioData[2] == 'F' &&
            audioData[3] == 'F';
    }

    static std::wstring Utf8ToWide(const std::string& value) {
        std::wstring wide;
        if (value.empty()) {
            return wide;
        }
        int sizeNeeded = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), NULL, 0);
        if (sizeNeeded <= 0) {
            return wide;
        }
        wide.resize(sizeNeeded);
        MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), &wide[0], sizeNeeded);
        return wide;
    }

    enum class TtsGenerationStatus {
        Unknown,
        Pending,
        Ready,
        Failed
    };

    static TtsGenerationStatus QueryTtsGenerationStatus(
        HINTERNET hConnect,
        const std::string& cacheKey,
        uint64_t generation,
        const TaskManager::CancellationToken& token) {
        if (!hConnect || cacheKey.empty() ||
            generation != g_audioGeneration.load() || token.IsCancellationRequested()) {
            return TtsGenerationStatus::Unknown;
        }

        const std::string path = "/DialecticServer/tts_status.php?cache_key=" + cacheKey;
        HINTERNET hRequest = WinHttpOpenRequest(hConnect,
                                                L"GET",
                                                Utf8ToWide(path).c_str(),
                                                NULL,
                                                WINHTTP_NO_REFERER,
                                                WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                0);
        if (!hRequest) {
            return TtsGenerationStatus::Unknown;
        }

        auto interruptibleRequest = std::make_shared<std::atomic<HINTERNET>>(hRequest);
        token.SetInterrupt([interruptibleRequest]() {
            HINTERNET request = interruptibleRequest->exchange(nullptr);
            if (request) WinHttpCloseHandle(request);
        });

        DWORD timeoutMs = 1000;
        WinHttpSetTimeouts(hRequest, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
        const bool requestOk = WinHttpSendRequest(hRequest,
                                                  WINHTTP_NO_ADDITIONAL_HEADERS,
                                                  0,
                                                  WINHTTP_NO_REQUEST_DATA,
                                                  0,
                                                  0,
                                                  0) &&
            WinHttpReceiveResponse(hRequest, NULL);

        DWORD statusCode = 0;
        DWORD statusCodeSize = sizeof(statusCode);
        if (requestOk) {
            WinHttpQueryHeaders(hRequest,
                                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX,
                                &statusCode,
                                &statusCodeSize,
                                WINHTTP_NO_HEADER_INDEX);
        }

        std::string response;
        if (requestOk && statusCode == 200) {
            DWORD available = 0;
            DWORD downloaded = 0;
            do {
                available = 0;
                if (!WinHttpQueryDataAvailable(hRequest, &available) || available == 0) {
                    break;
                }
                std::vector<char> buffer((std::min)(available, static_cast<DWORD>(4096)));
                if (!WinHttpReadData(hRequest, buffer.data(), static_cast<DWORD>(buffer.size()), &downloaded)) {
                    break;
                }
                response.append(buffer.data(), downloaded);
                if (response.size() >= 8192) {
                    break;
                }
            } while (available > 0);
        }

        token.ClearInterrupt();
        HINTERNET requestToClose = interruptibleRequest->exchange(nullptr);
        if (requestToClose) WinHttpCloseHandle(requestToClose);

        if (response.find("\"status\":\"failed\"") != std::string::npos ||
            response.find("\"status\": \"failed\"") != std::string::npos) {
            return TtsGenerationStatus::Failed;
        }
        if (response.find("\"status\":\"ready\"") != std::string::npos ||
            response.find("\"status\": \"ready\"") != std::string::npos) {
            return TtsGenerationStatus::Ready;
        }
        if (response.find("\"status\":\"pending\"") != std::string::npos ||
            response.find("\"status\": \"pending\"") != std::string::npos) {
            return TtsGenerationStatus::Pending;
        }
        return TtsGenerationStatus::Unknown;
    }

    static std::vector<uint8_t> DownloadSoundcacheWavWithRetry(
        const std::string& host,
        int port,
        const std::string& path,
        const std::string& cacheKey,
        bool queryGenerationStatus,
        uint64_t generation,
        const TaskManager::CancellationToken& token,
        int maxAttempts = 30) {
        maxAttempts = std::max(1, maxAttempts);
        std::vector<uint8_t> audioData;
        const auto startTime = std::chrono::steady_clock::now();

        HINTERNET hSession = WinHttpOpen(L"FalloutNV AIAgent/1.0",
                                         WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                         WINHTTP_NO_PROXY_NAME,
                                         WINHTTP_NO_PROXY_BYPASS,
                                         0);
        if (!hSession) {
            Log("SpeakManager: [Thread] Failed to open WinHTTP session (Error: %d)", GetLastError());
            return {};
        }

        HINTERNET hConnect = WinHttpConnect(hSession, Utf8ToWide(host).c_str(), port, 0);
        if (!hConnect) {
            Log("SpeakManager: [Thread] Failed to connect to server (Error: %d)", GetLastError());
            WinHttpCloseHandle(hSession);
            return {};
        }

        auto closeHandles = [&]() {
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
        };

        int attempt = 1;
        while (attempt <= maxAttempts) {
            if (generation != g_audioGeneration.load() || token.IsCancellationRequested()) {
                Log("SpeakManager: [Thread] TTS download cancelled before attempt %d for %s",
                    attempt, path.c_str());
                closeHandles();
                return {};
            }

            if (queryGenerationStatus) {
                const TtsGenerationStatus status =
                    QueryTtsGenerationStatus(hConnect, cacheKey, generation, token);
                if (status == TtsGenerationStatus::Failed) {
                    Log("SpeakManager: [Thread] Server reported permanent TTS failure for %s",
                        cacheKey.c_str());
                    closeHandles();
                    return {};
                }
                if (status == TtsGenerationStatus::Pending) {
                    if (!token.WaitFor(std::chrono::milliseconds(300))) {
                        closeHandles();
                        return {};
                    }
                    // A confirmed server-side job should remain eligible until
                    // the task deadline. Download attempts still stay bounded
                    // for unknown status or transient cache-read failures.
                    continue;
                }
            }

            HINTERNET hRequest = WinHttpOpenRequest(hConnect,
                                                    L"GET",
                                                    Utf8ToWide(path).c_str(),
                                                    NULL,
                                                    WINHTTP_NO_REFERER,
                                                    WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                    0);
            if (!hRequest) {
                Log("SpeakManager: [Thread] Failed to open HTTP request (Error: %d)", GetLastError());
                closeHandles();
                return {};
            }
            auto interruptibleRequest = std::make_shared<std::atomic<HINTERNET>>(hRequest);
            token.SetInterrupt([interruptibleRequest]() {
                HINTERNET request = interruptibleRequest->exchange(nullptr);
                if (request) WinHttpCloseHandle(request);
            });

            DWORD retryTimeout = 1500;
            WinHttpSetTimeouts(hRequest, retryTimeout, retryTimeout, retryTimeout, retryTimeout);

            bool requestOk = WinHttpSendRequest(hRequest,
                                                WINHTTP_NO_ADDITIONAL_HEADERS,
                                                0,
                                                WINHTTP_NO_REQUEST_DATA,
                                                0,
                                                0,
                                                0) &&
                WinHttpReceiveResponse(hRequest, NULL);

            DWORD statusCode = 0;
            DWORD statusCodeSize = sizeof(statusCode);
            if (requestOk) {
                WinHttpQueryHeaders(hRequest,
                                    WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX,
                                    &statusCode,
                                    &statusCodeSize,
                                    WINHTTP_NO_HEADER_INDEX);
            }

            audioData.clear();
            if (requestOk && statusCode == 200) {
                DWORD dwSize = 0;
                DWORD dwDownloaded = 0;
                do {
                    if (generation != g_audioGeneration.load() || token.IsCancellationRequested()) {
                        audioData.clear();
                        break;
                    }

                    dwSize = 0;
                    if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) {
                        Log("SpeakManager: [Thread] Error querying data available (Error: %d)", GetLastError());
                        break;
                    }
                    if (dwSize == 0) {
                        break;
                    }

                    std::vector<uint8_t> buffer(dwSize);
                    if (!WinHttpReadData(hRequest, buffer.data(), dwSize, &dwDownloaded)) {
                        Log("SpeakManager: [Thread] Error reading data (Error: %d)", GetLastError());
                        break;
                    }
                    audioData.insert(audioData.end(), buffer.begin(), buffer.begin() + dwDownloaded);
                } while (dwSize > 0);
            }

            token.ClearInterrupt();
            HINTERNET requestToClose = interruptibleRequest->exchange(nullptr);
            if (requestToClose) WinHttpCloseHandle(requestToClose);

            if (IsValidWavData(audioData)) {
                const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - startTime).count();
                if (attempt > 1) {
                    Log("SpeakManager: [Thread] TTS file became ready after %d attempts (%lld ms): %s",
                        attempt,
                        static_cast<long long>(elapsedMs),
                        path.c_str());
                } else {
                    Log("SpeakManager: [Thread] TTS file ready on first HTTP attempt (%lld ms): %s",
                        static_cast<long long>(elapsedMs),
                        path.c_str());
                }
                closeHandles();
                return audioData;
            }

            if (!audioData.empty()) {
                Log("SpeakManager: [Thread] TTS attempt %d returned non-WAV data (%zu bytes, status=%lu)",
                    attempt, audioData.size(), statusCode);
            } else {
                Log("SpeakManager: [Thread] TTS attempt %d not ready (status=%lu): %s",
                    attempt, statusCode, path.c_str());
            }

            const int sleepMs = std::min(400, 50 + (attempt * 50));
            if (!token.WaitFor(std::chrono::milliseconds(sleepMs))) {
                closeHandles();
                return {};
            }
            ++attempt;
        }

        closeHandles();
        return {};
    }

    static std::vector<uint8_t> ReadLocalSoundcacheWavWithRetry(
        const std::string& soundcachePath,
        const std::string& hash,
        uint64_t generation,
        const TaskManager::CancellationToken& token) {
        if (soundcachePath.empty() || hash.empty()) {
            return {};
        }

        std::string filePath = soundcachePath;
        const char lastChar = filePath.empty() ? '\0' : filePath[filePath.size() - 1];
        if (lastChar != '\\' && lastChar != '/') {
            filePath += "\\";
        }
        filePath += hash + ".wav";

        if (generation != g_audioGeneration.load() || token.IsCancellationRequested()) {
            Log("SpeakManager: [Thread] Local TTS read cancelled before open for %s",
                filePath.c_str());
            return {};
        }

        std::ifstream input(filePath, std::ios::binary);
        if (input) {
            std::vector<uint8_t> audioData(
                (std::istreambuf_iterator<char>(input)),
                std::istreambuf_iterator<char>());
            WavInfo wavInfo;
            if (IsValidWavData(audioData) && ParseWavInfo(audioData, wavInfo)) {
                Log("SpeakManager: [Thread] Loaded local TTS file: %s", filePath.c_str());
                return audioData;
            }
        }

        Log("SpeakManager: [Thread] Local TTS file unavailable, falling back to HTTP: %s",
            filePath.c_str());
        return {};
    }

    static uint32_t ResolveSpeakerFormId(const ScriptLine& line) {
        bool actorFormIdNameMismatch = false;
        if (line.actorFormId != 0) {
            const std::string registeredName = AgentManager::GetAgentName(line.actorFormId);
            if (registeredName.empty() || EqualsIgnoreCase(registeredName, line.actor)) {
                return line.actorFormId;
            }
            actorFormIdNameMismatch = true;
        }

        uint32_t formId = AgentManager::FindAgentFormIdByName(line.actor);
        if (formId != 0) {
            return formId;
        }

        const auto& target = TargetManager::GetCurrentTarget();
        if (target.formId != 0 && EqualsIgnoreCase(target.name, line.actor)) {
            return target.formId;
        }

        return actorFormIdNameMismatch ? 0 : line.actorFormId;
    }

    // Require a current-scene actor before binding queued dialogue to a reference.
    static bool IsLivePlaybackSpeaker(uint32_t formId,
                                      const std::string& expectedName,
                                      std::string* reason = nullptr) {
        if (formId == 0) {
            if (reason) {
                *reason = "speaker form id is unresolved";
            }
            return false;
        }

        const auto position = ActorPositionResolverFNV::ResolveActor(formId);
        if (!position.resolved) {
            if (reason) {
                *reason = "speaker position is unresolved";
            }
            return false;
        }
        if (!position.actorName.empty() && !EqualsIgnoreCase(position.actorName, expectedName)) {
            if (reason) {
                *reason = "speaker form id belongs to a different actor";
            }
            return false;
        }
        if (!ActorPositionResolverFNV::IsPositionInPlayerScene(position)) {
            if (reason) {
                *reason = "speaker is not in the player's current scene";
            }
            return false;
        }
        if (!ActorPositionResolverFNV::IsActorPositionFresh(formId, kDialogueActorFreshnessMs)) {
            if (reason) {
                *reason = "speaker position is stale";
            }
            return false;
        }
        if (!ActorPositionResolverFNV::IsActorInLatestScan(formId, kDialogueActorLatestScanMs)) {
            if (reason) {
                *reason = "speaker is not present in the latest spatial scan";
            }
            return false;
        }
        if (position.disabledKnown && position.isDisabled) {
            if (reason) {
                *reason = "speaker is disabled";
            }
            return false;
        }
        if (position.deadKnown && position.isDead) {
            if (reason) {
                *reason = "speaker is dead";
            }
            return false;
        }

        return true;
    }

    // Rebind once from live conversation state when a queued dynamic reference expires.
    static bool TryRebindSpeakerForPlayback(ScriptLine& line,
                                            uint32_t rejectedFormId,
                                            const std::string& rejectionReason) {
        struct Candidate {
            uint32_t formId = 0;
            const char* source = "";
        };

        std::vector<Candidate> candidates;
        if (GameLoop::IsConversationActive() &&
            EqualsIgnoreCase(GameLoop::GetConversationPartner(), line.actor)) {
            candidates.push_back({GameLoop::GetConversationPartnerFormId(), "active conversation"});
        }

        const auto& target = TargetManager::GetCurrentTarget();
        if (EqualsIgnoreCase(target.name, line.actor)) {
            candidates.push_back({target.formId, "current target"});
        }

        for (const auto& position : ActorPositionResolverFNV::GetRecentActorPositions()) {
            if (position.resolved && EqualsIgnoreCase(position.actorName, line.actor)) {
                candidates.push_back({position.formId, "spatial scan"});
            }
        }

        for (const Candidate& candidate : candidates) {
            if (candidate.formId == 0 || candidate.formId == rejectedFormId) {
                continue;
            }
            if (!IsLivePlaybackSpeaker(candidate.formId, line.actor)) {
                continue;
            }

            line.actorFormId = candidate.formId;
            Log("SpeakManager: Rebound queued speaker '%s' old=0x%08X new=0x%08X reason='%s' source='%s'",
                line.actor.c_str(),
                rejectedFormId,
                candidate.formId,
                rejectionReason.c_str(),
                candidate.source);
            return true;
        }

        return false;
    }

    static bool ShouldDropLineBeforePlayback(ScriptLine& line, std::string* reason = nullptr) {
        if (line.runtimeGeneration != 0 && !RuntimeGeneration::IsCurrent(line.runtimeGeneration)) {
            if (reason) {
                *reason = "runtime generation changed before playback";
            }
            return true;
        }
        if (!IsActorDialogueLine(line)) {
            return false;
        }

        const std::string currentSceneKey = CurrentPlayerSceneKey();
        if (!line.sceneKey.empty() &&
            !currentSceneKey.empty() &&
            line.sceneKey != currentSceneKey &&
            IsHardSceneBoundaryChange(line.sceneKey, currentSceneKey)) {
            if (reason) {
                *reason = "player scene changed before playback";
            }
            return true;
        }

        const uint32_t speakerFormId = ResolveSpeakerFormId(line);
        std::string speakerReason;
        if (!IsLivePlaybackSpeaker(speakerFormId, line.actor, &speakerReason)) {
            if (TryRebindSpeakerForPlayback(line, speakerFormId, speakerReason)) {
                return false;
            }
            if (reason) {
                *reason = speakerReason;
            }
            return true;
        }

        line.actorFormId = speakerFormId;
        return false;
    }

    struct RechatTarget {
        std::string listenerHint;
        std::string targetHint;
        uint32_t listenerFormId = 0;
        uint32_t targetFormId = 0;
    };

    static uint32_t ResolveRechatHintFormId(const std::string& hint) {
        const std::string cleanHint = Trim(hint);
        if (cleanHint.empty()) {
            return 0;
        }
        if (IsPlayerHint(cleanHint)) {
            return 0x00000014;
        }

        const uint32_t agentFormId = AgentManager::FindAgentFormIdByName(cleanHint);
        return agentFormId;
    }

    static uint32_t ResolveValidatedRechatFormId(const std::string& hint,
                                                 uint32_t suppliedFormId,
                                                 uint32_t speakerFormId) {
        const std::string cleanHint = Trim(hint);
        if (cleanHint.empty()) {
            return 0;
        }

        if (suppliedFormId == 0 || suppliedFormId == speakerFormId) {
            return ResolveRechatHintFormId(cleanHint);
        }

        if (IsPlayerHint(cleanHint)) {
            return suppliedFormId == 0x00000014 ? suppliedFormId : 0x00000014;
        }

        const std::string suppliedActorName = AgentManager::GetAgentName(suppliedFormId);
        if (!suppliedActorName.empty() && !EqualsIgnoreCase(suppliedActorName, cleanHint)) {
            Log("SpeakManager: Ignoring mismatched rechat form id 0x%08X for hint %s; resolved actor is %s",
                suppliedFormId, cleanHint.c_str(), suppliedActorName.c_str());
            return ResolveRechatHintFormId(cleanHint);
        }

        return suppliedFormId;
    }

    static std::vector<std::string> BuildRechatAudienceNames(const std::string& speaker,
                                                             const std::string& listenerHint,
                                                             const std::string& targetHint,
                                                             uint32_t speakerFormId) {
        std::vector<std::string> names;
        std::set<std::string> seen;

        auto speakerPosition = ActorPositionResolverFNV::ResolveActor(speakerFormId);
        if (!speakerPosition.resolved) {
            Log("SpeakManager: Rechat audience limited to speaker only because speaker 0x%08X position is unresolved",
                speakerFormId);
            AppendUniqueAudienceName(names, seen, speaker);
            return names;
        }

        struct AudienceCandidate {
            std::string name;
            float distance = 0.0f;
        };

        std::vector<AudienceCandidate> candidates;
        auto appendHintedActor = [&](const std::string& hint) {
            const uint32_t formId = ResolveRechatHintFormId(hint);
            if (formId == 0 || formId == 0x00000014 || formId == speakerFormId) {
                return;
            }

            const auto position = ActorPositionResolverFNV::ResolveActor(formId);
            if (!IsRechatPositionEligible(position)) {
                return;
            }

            const auto spatial = SpatialAwarenessFNV::Evaluate(speakerPosition, position);
            if (!spatial.canCommunicate) {
                return;
            }

            const std::string registeredName = AgentManager::GetAgentName(formId);
            AppendUniqueAudienceName(names, seen,
                registeredName.empty() ? Trim(hint) : registeredName);
        };

        // Preserve the response's intended listener before adding ambient candidates.
        appendHintedActor(targetHint);
        appendHintedActor(listenerHint);

        const auto playerPosition = ActorPositionResolverFNV::ResolvePlayer();
        if (playerPosition.resolved && playerPosition.formId != speakerFormId) {
            const auto playerSpatial = SpatialAwarenessFNV::Evaluate(speakerPosition, playerPosition);
            if (playerSpatial.canCommunicate) {
                const std::string playerName = Config::playerName.empty() ? "Player" : Config::playerName;
                candidates.push_back({ playerName, playerSpatial.airDistance });
            }
        }

        const auto positions = ActorPositionResolverFNV::GetRecentActorPositions();
        for (const auto& position : positions) {
            if (!position.resolved ||
                position.formId == 0 ||
                position.formId == speakerFormId ||
                position.formId == 0x00000014 ||
                position.actorName.empty() ||
                position.actorName == "<no name>") {
                continue;
            }

            const auto spatial = SpatialAwarenessFNV::Evaluate(speakerPosition, position);
            if (!spatial.canCommunicate) {
                continue;
            }

            if (!IsRechatPositionEligible(position)) {
                continue;
            }

            candidates.push_back({ position.actorName, spatial.airDistance });
        }

        std::sort(candidates.begin(), candidates.end(), [](const AudienceCandidate& left, const AudienceCandidate& right) {
            return left.distance < right.distance;
        });

        constexpr size_t kMaxAudienceNames = 12;
        for (const auto& candidate : candidates) {
            if (names.size() >= kMaxAudienceNames) {
                break;
            }
            AppendUniqueAudienceName(names, seen, candidate.name);
        }

        AppendUniqueAudienceName(names, seen, speaker);
        return names;
    }

    static RechatTarget ResolveRechatTarget(const ScriptLine& line, uint32_t speakerFormId) {
        RechatTarget target;
        target.listenerHint = Trim(line.listenerHint);
        target.targetHint = Trim(line.rechatTargetHint);

        if (target.listenerHint.empty()) {
            target.listenerHint = Config::playerName.empty() ? "Player" : Config::playerName;
        }
        // Match CHIM: an absent explicit rechat target falls back to the
        // response line's listener, never the crosshair or nearest actor.
        if (target.targetHint.empty()) {
            target.targetHint = target.listenerHint;
        }

        target.listenerFormId = ResolveValidatedRechatFormId(
            target.listenerHint, line.listenerFormId, speakerFormId);
        target.targetFormId = ResolveValidatedRechatFormId(
            target.targetHint, line.rechatTargetFormId, speakerFormId);

        return target;
    }

    static bool RechatEnvironmentAllowed(const std::string& speaker) {
        const auto& state = GameLoop::GetGameState();
        const uint32_t speakerFormId = AgentManager::FindAgentFormIdByName(speaker);
        if (!GameLoop::IsCombatDialogueAllowed(speakerFormId)) {
            Log("SpeakManager: Rechat launch skipped for %s because combat dialogue is disabled",
                speaker.c_str());
            WriteRechatStatus("skipped", speaker, "environment", "combat_dialogue_disabled");
            return false;
        }
        if (state.isLoading) {
            Log("SpeakManager: Rechat launch skipped for %s because loading gate is active "
                "(inGame=%d paused=%d inMenu=%d inDialogue=%d loading=%d)",
                speaker.c_str(),
                state.isInGame ? 1 : 0,
                state.isPaused ? 1 : 0,
                state.isInMenu ? 1 : 0,
                state.isInDialogue ? 1 : 0,
                state.isLoading ? 1 : 0);
            WriteRechatStatus("skipped", speaker, "environment", "loading");
            return false;
        }

        if (Config::rechatAvoidInMenu && state.isInDialogue) {
            Log("SpeakManager: Rechat launch skipped for %s because dialogue/menu-topic gate is active "
                "(inGame=%d paused=%d inMenu=%d inDialogue=%d loading=%d)",
                speaker.c_str(),
                state.isInGame ? 1 : 0,
                state.isPaused ? 1 : 0,
                state.isInMenu ? 1 : 0,
                state.isInDialogue ? 1 : 0,
                state.isLoading ? 1 : 0);
            WriteRechatStatus("skipped", speaker, "environment", "dialogue_menu");
            return false;
        }

        if (Config::rechatAvoidInMenu && !state.isInGame) {
            Log("SpeakManager: Rechat launch skipped for %s because game-state bridge has not marked in-game "
                "(inGame=%d paused=%d inMenu=%d inDialogue=%d loading=%d)",
                speaker.c_str(),
                state.isInGame ? 1 : 0,
                state.isPaused ? 1 : 0,
                state.isInMenu ? 1 : 0,
                state.isInDialogue ? 1 : 0,
                state.isLoading ? 1 : 0);
            WriteRechatStatus("skipped", speaker, "environment", "not_in_game");
            return false;
        }

        if (Config::rechatAvoidInMenu && (state.isPaused || state.isInMenu)) {
            Log("SpeakManager: Rechat environment has menu/pause flag for %s, but launch is allowed because dialogue playback is already complete "
                "(inGame=%d paused=%d inMenu=%d inDialogue=%d loading=%d)",
                speaker.c_str(),
                state.isInGame ? 1 : 0,
                state.isPaused ? 1 : 0,
                state.isInMenu ? 1 : 0,
                state.isInDialogue ? 1 : 0,
                state.isLoading ? 1 : 0);
        }

        if (Config::rechatAvoidWhenSneaking && ActorPositionResolverFNV::IsPlayerSneaking()) {
            Log("SpeakManager: Rechat launch skipped for %s because player is sneaking", speaker.c_str());
            WriteRechatStatus("skipped", speaker, "environment", "player_sneaking");
            return false;
        }

        return true;
    }

    static void DeferRechatLaunch(const ScriptLine& line,
                                  const char* trigger,
                                  const HTTPManager::QueueStatus& httpStatus,
                                  int queuedLines,
                                  int pendingAudioWork) {
        if (!line.isFinalResponseLine || Trim(line.actor).empty() || Trim(line.text).empty()) {
            return;
        }

        std::lock_guard<std::mutex> lock(g_rechatMutex);
        g_pendingRechatLaunchLine = line;
        g_pendingRechatLaunchActive = true;
        g_pendingRechatLaunchUntil = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        g_pendingRechatNextCheck = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        Log("SpeakManager: Deferred rechat launch for %s trigger=%s queueItems=%d pendingAudio=%d stream=%d httpQueued=%zu",
            line.actor.c_str(),
            trigger ? trigger : "unknown",
            queuedLines,
            pendingAudioWork,
            httpStatus.streamInProgress ? 1 : 0,
            httpStatus.httpResponsesQueued);
        WriteRechatStatus("deferred", line.actor, trigger ? trigger : "unknown", "launch_window_waiting_for_queue", line.rechatTargetHint);
    }

    static bool IsSamePlaybackLine(const ScriptLine& a, const ScriptLine& b) {
        if (!a.utteranceId.empty() && !b.utteranceId.empty()) {
            return a.utteranceId == b.utteranceId;
        }
        return EqualsIgnoreCase(a.actor, b.actor) && Trim(a.text) == Trim(b.text);
    }

    static void ClearDeferredRechatLaunch() {
        std::lock_guard<std::mutex> lock(g_rechatMutex);
        g_pendingRechatLaunchLine = ScriptLine{};
        g_pendingRechatLaunchActive = false;
        g_pendingRechatLaunchUntil = {};
        g_pendingRechatNextCheck = {};
    }

    static bool MaybeLaunchRechatForLine(const ScriptLine& finishedLine,
                                         const char* trigger,
                                         bool requireFinalLine,
                                         bool allowCurrentPlayback) {
        const std::string speaker = Trim(finishedLine.actor);
        const std::string originLine = Trim(finishedLine.text);
        if (!Config::rechatEnabled || !Config::rechatSmartLaunch) {
            WriteRechatStatus("skipped", speaker, trigger, "disabled_or_smart_launch_off");
            return false;
        }
        if (speaker.empty() || originLine.empty() ||
            IsPlayerSpeakerName(speaker) ||
            EqualsIgnoreCase(speaker, "The Narrator")) {
            WriteRechatStatus("skipped", speaker, trigger, "invalid_speaker_or_origin");
            return false;
        }
        const uint32_t speakerFormId = ResolveSpeakerFormId(finishedLine);
        if (ActionManager::HasPendingPostDialogueActionForSpeaker(speaker, speakerFormId)) {
            Log("SpeakManager: Rechat skipped for %s trigger=%s because a post-dialogue action is pending",
                speaker.c_str(), trigger);
            WriteRechatStatus("skipped", speaker, trigger, "post_dialogue_action_pending");
            return false;
        }

        const HTTPManager::QueueStatus httpStatus = HTTPManager::GetQueueStatus();
        const bool httpQueueDrained = !httpStatus.streamInProgress && httpStatus.httpResponsesQueued == 0;
        const int queuedLines = CountItems();
        const int pendingAudioWork = PendingAudioWorkCount();
        const bool speechQueueDrained = queuedLines == 0 && pendingAudioWork == 0;
        const bool playbackDrained =
            !AudioManager::IsPlaying() &&
            !AudioManager::IsPaused() &&
            !g_currentPlaybackLineActive;
        const bool currentPlaybackEligible =
            allowCurrentPlayback &&
            g_currentPlaybackLineActive &&
            IsSamePlaybackLine(finishedLine, g_currentPlaybackLine);
        const bool playbackReady = playbackDrained || currentPlaybackEligible;
        // CHIM launches rechat before DownloadAndPlay for the final queued line.
        // Dialectic can prefetch audio while a previous line is still playing, so
        // allow the final-line audio_prepare path to hide the next LLM request
        // behind the current playback once HTTP and speech queues are drained.
        const bool queuedFinalLinePrepareReady =
            trigger != nullptr &&
            std::strcmp(trigger, "audio_prepare") == 0 &&
            httpQueueDrained &&
            speechQueueDrained;
        const bool runtimeFinalLine = httpQueueDrained &&
            speechQueueDrained &&
            (playbackReady || queuedFinalLinePrepareReady);
        const bool finalLineReady = !requireFinalLine || finishedLine.isFinalResponseLine || runtimeFinalLine;
        const bool launchWindow =
            finalLineReady && runtimeFinalLine;
        if (!launchWindow) {
            const char* reason = requireFinalLine && !finalLineReady
                ? "not_final_line"
                : "launch_window_not_ready";
            Log("SpeakManager: Rechat not evaluated for %s trigger=%s; reason=%s queueItems=%d pendingAudio=%d httpDrained=%d playbackDrained=%d currentPlaybackEligible=%d queuedFinalPrepare=%d stream=%d httpQueued=%zu finalLine=%d runtimeFinal=%d",
                speaker.c_str(),
                trigger,
                reason,
                queuedLines,
                pendingAudioWork,
                httpQueueDrained ? 1 : 0,
                playbackDrained ? 1 : 0,
                currentPlaybackEligible ? 1 : 0,
                queuedFinalLinePrepareReady ? 1 : 0,
                httpStatus.streamInProgress ? 1 : 0,
                httpStatus.httpResponsesQueued,
                finishedLine.isFinalResponseLine ? 1 : 0,
                runtimeFinalLine ? 1 : 0);
            WriteRechatStatus("skipped", speaker, trigger, reason);
            DeferRechatLaunch(finishedLine, trigger, httpStatus, queuedLines, pendingAudioWork);
            return false;
        }

        if (!RechatEnvironmentAllowed(speaker)) {
            return false;
        }

        const bool sameSpeakerAsLastRechatter = EqualsIgnoreCase(speaker, GetLastRechatter());
        if (sameSpeakerAsLastRechatter) {
            Log("SpeakManager: Rechat avoided for %s trigger=%s because this speaker launched the last rechat",
                speaker.c_str(), trigger);
            WriteRechatStatus("skipped", speaker, trigger, "same_speaker_as_last_rechatter");
            return false;
        }

        if (speakerFormId == 0) {
            Log("SpeakManager: Rechat skipped for %s trigger=%s because speaker form id could not be resolved",
                speaker.c_str(), trigger);
            WriteRechatStatus("skipped", speaker, trigger, "speaker_formid_unresolved");
            return false;
        }
        const auto speakerPosition = ActorPositionResolverFNV::ResolveActor(speakerFormId);
        std::string speakerEligibilityReason;
        if (!IsRechatPositionEligible(speakerPosition, &speakerEligibilityReason)) {
            Log("SpeakManager: Rechat skipped for %s trigger=%s because speaker is not eligible in the current scene: %s",
                speaker.c_str(), trigger, speakerEligibilityReason.c_str());
            WriteRechatStatus("skipped", speaker, trigger, speakerEligibilityReason);
            return false;
        }

        if (IsRechatInFlightFor(speaker)) {
            RechatTarget target = ResolveRechatTarget(finishedLine, speakerFormId);
            QueueRechatRetry(speaker, target.listenerHint, target.targetHint, originLine,
                             finishedLine.rechatDepth, speakerFormId,
                             target.listenerFormId, target.targetFormId);
            Log("SpeakManager: Rechat retry queued for %s trigger=%s because a request is already in flight",
                speaker.c_str(), trigger);
            WriteRechatStatus("retry_queued", speaker, trigger, "request_in_flight", target.targetHint);
            return true;
        }

        RechatTarget target = ResolveRechatTarget(finishedLine, speakerFormId);
        Log("SpeakManager: Launching rechat trigger=%s speaker=%s queueItems=%d pendingAudio=%d httpDrained=%d playbackDrained=%d currentPlaybackEligible=%d queuedFinalPrepare=%d finalLine=%d runtimeFinal=%d target=%s",
            trigger, speaker.c_str(), queuedLines, pendingAudioWork, httpQueueDrained ? 1 : 0,
            playbackDrained ? 1 : 0,
            currentPlaybackEligible ? 1 : 0,
            queuedFinalLinePrepareReady ? 1 : 0,
            finishedLine.isFinalResponseLine ? 1 : 0,
            runtimeFinalLine ? 1 : 0,
            target.targetHint.c_str());
        WriteRechatStatus("launching", speaker, trigger, "launch_window_ready", target.targetHint);
        return Rechat(speaker,
                      target.listenerHint,
                      finishedLine.rechatDepth,
                      originLine,
                      target.targetHint,
                      speakerFormId,
                      target.listenerFormId,
                      target.targetFormId) > 0;
    }

    static bool MaybeLaunchRechatOnPlaybackStart(const ScriptLine& line) {
        if (line.rechatLaunched) {
            Log("SpeakManager: Rechat playback-start skipped for %s because this line already launched rechat",
                line.actor.c_str());
            return true;
        }
        return MaybeLaunchRechatForLine(line, "playback_start", true, true);
    }

    static bool MaybeLaunchRechatAfterPlayback(const ScriptLine& finishedLine) {
        if (finishedLine.rechatLaunched) {
            Log("SpeakManager: Rechat playback-finished skipped for %s because this line already launched rechat",
                finishedLine.actor.c_str());
            return true;
        }
        return MaybeLaunchRechatForLine(finishedLine, "playback_finished", false, false);
    }

    static void MaybeProcessDeferredRechatLaunch() {
        ScriptLine pendingLine;
        {
            std::lock_guard<std::mutex> lock(g_rechatMutex);
            if (!g_pendingRechatLaunchActive) {
                return;
            }

            const auto now = std::chrono::steady_clock::now();
            if (now < g_pendingRechatNextCheck) {
                return;
            }

            const auto& gameState = GameLoop::GetGameState();
            const bool waitingOnMenuPause =
                Config::pauseDialogueOnMenu &&
                (gameState.isPaused || gameState.isInMenu || AudioManager::IsPaused());
            if (waitingOnMenuPause) {
                g_pendingRechatLaunchUntil = now + std::chrono::seconds(8);
            } else if (now > g_pendingRechatLaunchUntil) {
                Log("SpeakManager: Deferred rechat launch expired for %s",
                    g_pendingRechatLaunchLine.actor.c_str());
                WriteRechatStatus("skipped",
                                  g_pendingRechatLaunchLine.actor,
                                  "deferred_playback_finished",
                                  "deferred_launch_expired",
                                  g_pendingRechatLaunchLine.rechatTargetHint);
                g_pendingRechatLaunchLine = ScriptLine{};
                g_pendingRechatLaunchActive = false;
                g_pendingRechatLaunchUntil = {};
                g_pendingRechatNextCheck = {};
                return;
            }

            g_pendingRechatNextCheck = now + std::chrono::milliseconds(250);
            pendingLine = g_pendingRechatLaunchLine;
        }

        if (IsPlayerSpeakerName(pendingLine.actor) || IsNarratorLine(pendingLine)) {
            ClearDeferredRechatLaunch();
            return;
        }
        if (Config::rechatAvoidWhenSneaking && ActorPositionResolverFNV::IsPlayerSneaking()) {
            Log("SpeakManager: Deferred rechat launch cleared for %s because player is sneaking",
                pendingLine.actor.c_str());
            WriteRechatStatus("skipped", pendingLine.actor, "deferred_playback_window",
                              "player_sneaking", pendingLine.rechatTargetHint);
            ClearDeferredRechatLaunch();
            return;
        }

        if (MaybeLaunchRechatForLine(pendingLine, "deferred_playback_window", false, true)) {
            if (g_currentPlaybackLineActive && IsSamePlaybackLine(pendingLine, g_currentPlaybackLine)) {
                g_currentPlaybackRechatLaunched = true;
            }
            ClearDeferredRechatLaunch();
        }
    }

    bool beginRechatAttempt(const std::string& speaker) {
        return BeginRechatAttempt(speaker);
    }

    void queueRechatRetry(const std::string& speaker,
                          const std::string& listenerHint,
                          const std::string& explicitTarget,
                          const std::string& originLine,
                          int rechatDepth,
                          uint32_t speakerFormId,
                          uint32_t listenerFormId,
                          uint32_t targetFormId) {
        QueueRechatRetry(speaker, listenerHint, explicitTarget, originLine, rechatDepth,
                         speakerFormId, listenerFormId, targetFormId);
    }

    void completeRechatAttempt(const std::string& speaker, bool success) {
        CompleteRechatAttempt(speaker, success);
    }

    bool isRechatInFlightFor(const std::string& speaker) {
        return IsRechatInFlightFor(speaker);
    }

    void resetRechatChainState() {
        ResetRechatChainState();
    }

    bool isRechatChainClosed() {
        return IsRechatChainClosed();
    }

    std::string getLastRechatter() {
        return GetLastRechatter();
    }

    void setLastRechatter(const std::string& speaker) {
        SetLastRechatter(speaker);
    }

    std::string ensureRechatChainId(const std::string& speaker,
                                    const std::string& listenerHint,
                                    const std::string& explicitTarget) {
        return EnsureRechatChainId(speaker, listenerHint, explicitTarget);
    }

    int rechat(const std::string& speaker,
               const std::string& listenerHint,
               int rechatDepth,
               const std::string& originLine,
               const std::string& explicitTarget,
               uint32_t speakerFormId,
               uint32_t listenerFormId,
               uint32_t targetFormId) {
        return Rechat(speaker, listenerHint, rechatDepth, originLine, explicitTarget,
                      speakerFormId, listenerFormId, targetFormId);
    }

    void Process(const std::string& actorName) {
        if (g_isProcessing) return;
        if (!HasItems()) return;

        SetProcessing(true);
        DequeueFirstItem();
        SetProcessing(false);
    }

    void ProcessPlayer() {
        Process("Player");
    }

    static bool ShouldPauseDialogueForMenu() {
        const auto& state = GameLoop::GetGameState();
        return Config::pauseDialogueOnMenu &&
            (state.isPaused || GameLoop::IsTextInputMenuActiveOrRecentlyClosed());
    }

    // Functions required by GameLoop
    void UpdatePlaybackFrame() {
        ProcessPendingLipSyncResets();
        UpdatePlayerTextOnlySubtitle();
        if (CancelDialogueIfPlayerSceneChanged()) {
            return;
        }

        if (AudioManager::IsPlaying()) {
            const auto& state = GameLoop::GetGameState();
            const bool shouldPauseForMenu = ShouldPauseDialogueForMenu();

            if (shouldPauseForMenu) {
                if (!g_playbackPausedForMenu) {
                    AudioManager::Pause();
                    g_playbackPausedForMenu = true;
                    Log("SpeakManager: Paused AI dialogue because a blocking menu/chatbox is open "
                        "(paused=%d inMenu=%d)",
                        state.isPaused ? 1 : 0,
                        state.isInMenu ? 1 : 0);
                }
                return;
            }

            if (g_playbackPausedForMenu) {
                AudioManager::Resume();
                g_playbackPausedForMenu = false;
                Log("SpeakManager: Resumed AI dialogue after blocking menu/chatbox closed");
            }

            UpdateCurrentSpatialPlayback();
            UpdateLipSyncBridge();
            UpdateSubtitleBridge();
            LogDialogueGuardStatusIfChanged();
        } else if (g_playbackPausedForMenu && !AudioManager::IsPaused() &&
                   !ShouldPauseDialogueForMenu()) {
            g_playbackPausedForMenu = false;
        }
    }

    void ProcessQueue() {
        UpdatePlayerTextOnlySubtitle();
        if (UpdateNpcTextOnlyFallback()) {
            return;
        }
        LogQueueStatusIfNeeded();
        LogDialogueGuardStatusIfChanged();
        if (CancelDialogueIfPlayerSceneChanged()) {
            return;
        }
        MaybeProcessDeferredRechatLaunch();

        const bool audioPlaying = AudioManager::IsPlaying();
        const bool audioPaused = AudioManager::IsPaused();
        if (!audioPlaying && !audioPaused && g_currentPlaybackLineActive) {
            ScriptLine finishedLine = g_currentPlaybackLine;
            g_playbackCompleted.fetch_add(1, std::memory_order_relaxed);
            const bool rechatAlreadyLaunched = g_currentPlaybackRechatLaunched;
            if (!finishedLine.textOnlyFallback) {
                SendDeliveryState(finishedLine, "spoken");
            }
            g_dialogueGuardHoldUntil = std::chrono::steady_clock::now() + kVanillaDialogueGuardTail;
            FinalizeLipSync("playback_finished");
            ClearFaceTargetBridge();
            g_currentPlaybackLine = ScriptLine{};
            g_currentPlaybackLineActive = false;
            g_currentPlaybackRechatLaunched = false;
            g_currentPlaybackIsHeadVoice = false;
            g_currentSpeakerFormId = 0;
            g_currentSpeaker.clear();
            AudioManager::Set3DPlaybackEnabled(false);
            AudioManager::SetVolume(GetBaseVoiceVolume());
            ClearSubtitleBridge();
            ClearDialogueGuardBridge();
            const HTTPManager::QueueStatus httpStatusForActions = HTTPManager::GetQueueStatus();
            const bool responseStreamDrainedForActions =
                !httpStatusForActions.streamInProgress &&
                httpStatusForActions.httpResponsesQueued == 0 &&
                httpStatusForActions.pendingHttpTasks == 0 &&
                httpStatusForActions.activeHttpTasks == 0;
            const int queuedLinesForActions = CountItems();
            const int pendingAudioForActions = PendingAudioWorkCount();
            const bool speechQueueDrainedForActions =
                queuedLinesForActions == 0 &&
                pendingAudioForActions == 0;
            const bool flushedPostDialogueActions =
                finishedLine.isFinalResponseLine &&
                responseStreamDrainedForActions &&
                speechQueueDrainedForActions &&
                ActionManager::FlushPostDialogueActionsForSpeaker(finishedLine.actor,
                                                                  finishedLine.actorFormId,
                                                                  "SpeakManager");
            if (finishedLine.isFinalResponseLine &&
                (!responseStreamDrainedForActions || !speechQueueDrainedForActions) &&
                ActionManager::HasPendingPostDialogueActionForSpeaker(finishedLine.actor,
                                                                      finishedLine.actorFormId)) {
                Log("SpeakManager: Holding post-dialogue action for %s because response/speech queue is not drained "
                    "(http_stream=%d http_tasks=%zu/%zu http_queued=%zu dialogue=%d pendingAudio=%d)",
                    finishedLine.actor.c_str(),
                    httpStatusForActions.streamInProgress ? 1 : 0,
                    httpStatusForActions.activeHttpTasks,
                    httpStatusForActions.pendingHttpTasks,
                    httpStatusForActions.httpResponsesQueued,
                    queuedLinesForActions,
                    pendingAudioForActions);
            }
            if (!finishedLine.textOnlyFallback &&
                !rechatAlreadyLaunched &&
                !flushedPostDialogueActions) {
                MaybeLaunchRechatAfterPlayback(finishedLine);
            }
        }

        const auto& state = GameLoop::GetGameState();
        const bool shouldPauseForMenu = ShouldPauseDialogueForMenu();
        if (shouldPauseForMenu) {
            if (audioPlaying || audioPaused) {
                UpdatePlaybackFrame();
                return;
            }
            if (!g_playbackPausedForMenu) {
                g_playbackPausedForMenu = true;
                Log("SpeakManager: Holding AI dialogue because a blocking menu/chatbox is open "
                    "(paused=%d inMenu=%d)",
                    state.isPaused ? 1 : 0,
                    state.isInMenu ? 1 : 0);
            }
            return;
        }
        if (g_playbackPausedForMenu && !audioPlaying && !audioPaused) {
            g_playbackPausedForMenu = false;
            Log("SpeakManager: Releasing held AI dialogue after blocking menu/chatbox closed");
        }

        PendingAudio readyAudio;
        bool hasReadyAudio = false;
        {
            std::lock_guard<std::mutex> pendingLock(g_pendingMutex);
            if (!g_pendingAudioQueue.empty() && !g_isProcessing && !audioPlaying && !audioPaused) {
                hasReadyAudio = PopNextReadyAudioLocked(readyAudio);
            }
        }

        if (hasReadyAudio) {
            g_isProcessing = true;

            std::string dropReason;
            if (ShouldDropLineBeforePlayback(readyAudio.line, &dropReason)) {
                Log("SpeakManager: Dropping stale queued audio for speaker '%s' (0x%08X): %s",
                    readyAudio.line.actor.c_str(),
                    readyAudio.line.actorFormId,
                    dropReason.c_str());
                SendDeliveryState(readyAudio.line, "aborted");
                ClearDialogueGuardBridgeIfIdle();
                g_isProcessing = false;
                UpdatePlaybackFrame();
                return;
            }
            readyAudio.actorFormId = readyAudio.line.actorFormId;

            if (readyAudio.textOnlyFallback) {
                g_currentSpeaker = readyAudio.speaker;
                g_currentSpeakerFormId = readyAudio.actorFormId;
                g_currentPlaybackLine = readyAudio.line;
                g_currentPlaybackLineActive = true;
                g_currentPlaybackRechatLaunched = false;
                StartDialogueGuardBridge(g_currentPlaybackLine);
                StartNpcTextOnlyFallback(g_currentPlaybackLine);
                g_isProcessing = false;
                return;
            }

            Log("SpeakManager: Playing audio for speaker '%s' (%zu bytes)",
                readyAudio.speaker.c_str(), readyAudio.audioData.size());

            if (AudioManager::LoadWAV(readyAudio.audioData.data(),
                                     static_cast<int>(readyAudio.audioData.size()))) {
                g_currentSpeaker = readyAudio.speaker;
                g_currentSpeakerFormId = readyAudio.actorFormId;
                g_currentPlaybackLine = readyAudio.line;
                g_currentPlaybackLineActive = true;
                g_currentPlaybackRechatLaunched = false;
                g_currentPlaybackIsHeadVoice = HeadVoiceVolumeUtils::IsHeadVoice(
                    IsNarratorLine(g_currentPlaybackLine),
                    IsPlayerTtsLine(g_currentPlaybackLine));
                g_lastSpatialPlaybackUpdateTime = {};
                g_lastSpatialPlaybackSpeakerFormId = 0;
                AudioManager::Set3DPlaybackEnabled(false);
                AudioManager::SetVolume(GetCurrentLineVoiceVolume());
                if (g_currentPlaybackIsHeadVoice) {
                    Log("SpeakManager: Applying narrator/player TTS volume %.0f%% to speaker '%s'",
                        Config::headVoiceVolume,
                        g_currentSpeaker.c_str());
                }
                StartDialogueGuardBridge(g_currentPlaybackLine);
                StartFaceTargetBridge(g_currentPlaybackLine);
                if (AudioManager::Play()) {
                    g_playbackStarted.fetch_add(1, std::memory_order_relaxed);
                    Log("SpeakManager: Audio playback started for speaker '%s'",
                        g_currentSpeaker.c_str());
                    SendDeliveryState(g_currentPlaybackLine, "playing");
                    StartLipSync(g_currentPlaybackLine, readyAudio.audioData);
                    StartSubtitleBridge(g_currentPlaybackLine, readyAudio.audioData);
                    UpdateCurrentSpatialPlayback();
                    UpdateLipSyncBridge();
                    UpdateSubtitleBridge();
                    g_currentPlaybackRechatLaunched = MaybeLaunchRechatOnPlaybackStart(g_currentPlaybackLine);
                } else {
                    g_playbackFailed.fetch_add(1, std::memory_order_relaxed);
                    Log("SpeakManager: AudioManager::Play failed for speaker '%s'",
                        g_currentSpeaker.c_str());
                    SendDeliveryState(g_currentPlaybackLine, "failed");
                    FinalizeLipSync("audio_play_failed");
                    ClearFaceTargetBridge();
                    g_currentPlaybackLine = ScriptLine{};
                    g_currentPlaybackLineActive = false;
                    g_currentPlaybackRechatLaunched = false;
                    g_currentPlaybackIsHeadVoice = false;
                    AudioManager::Set3DPlaybackEnabled(false);
                    AudioManager::SetVolume(GetBaseVoiceVolume());
                    ClearSubtitleBridge();
                    ClearDialogueGuardBridgeIfIdle();
                }
            } else {
                g_playbackFailed.fetch_add(1, std::memory_order_relaxed);
                Log("SpeakManager: Failed to load WAV data");
                SendDeliveryState(readyAudio.line, "failed");
                AudioManager::Set3DPlaybackEnabled(false);
                AudioManager::SetVolume(GetBaseVoiceVolume());
                ClearFaceTargetBridge();
                ClearSubtitleBridge();
                ClearDialogueGuardBridgeIfIdle();
            }

            g_isProcessing = false;
        }

        UpdatePlaybackFrame();

        {
            std::lock_guard<std::mutex> pendingLock(g_pendingMutex);
            if (PendingAudioWorkCountLocked() >= MAX_PREFETCHED_AUDIO) {
                return;
            }
        }

        if (IsPlayerInputTtsGateActive()) {
            return;
        }

        ScriptLine item;
        {
            std::lock_guard<std::mutex> lock(g_queueMutex);
            if (g_scriptQueue.empty()) {
                return;
            }
            item = g_scriptQueue.front();
            g_scriptQueue.pop();
        }

        std::string dropReason;
        if (ShouldDropLineBeforePlayback(item, &dropReason)) {
            Log("SpeakManager: Dropping stale queued dialogue for speaker '%s' (0x%08X): %s",
                item.actor.c_str(),
                item.actorFormId,
                dropReason.c_str());
            SendDeliveryState(item, "aborted");
            if (item.sequence != 0) {
                std::lock_guard<std::mutex> pendingLock(g_pendingMutex);
                g_pendingLineSequences.erase(item.sequence);
            }
            ClearDialogueGuardBridgeIfIdle();
            return;
        }

        {
            std::lock_guard<std::mutex> pendingLock(g_pendingMutex);
            ++g_downloadsInProgress;
        }
        g_audioPrepareStarted.fetch_add(1, std::memory_order_relaxed);

        // Capture config values on main thread to avoid threading issues
        std::string serverHost = Config::serverHost;
        int serverPort = Config::serverPort;
        std::string localSoundcachePath = Config::localSoundcachePath;
        
        // Server responses include a voice-aware cache key. Fall back to the
        // legacy text hash for locally queued/debug lines.
        std::string textHash = item.ttsCacheKey.empty() ? Misc::MD5Hash(item.text) : item.ttsCacheKey;
        const uint64_t audioGeneration = g_audioGeneration.load();
        const bool isPlayerTts = IsPlayerTtsLine(item);
        // Local voice-clone generation can legitimately take longer than ten
        // seconds. Keep polling while the server reports a pending job so the
        // generated cache file is not discarded just before it becomes ready.
        const int maxHttpAttempts = isPlayerTts ? 4 : kNpcTtsMaxDownloadAttempts;
        Log("SpeakManager: utterance state=preparing_audio speaker='%s' utterance='%s' cache='%s'",
            item.actor.c_str(),
            item.utteranceId.c_str(),
            textHash.c_str());

        TaskManager::Options audioTask;
        audioTask.type = "audio_prepare";
        audioTask.key = item.utteranceId.empty() ? textHash : item.utteranceId;
        audioTask.generation = item.runtimeGeneration;
        audioTask.scope.actorFormId = item.actorFormId;
        audioTask.scope.utteranceId = item.utteranceId;
        audioTask.lane = TaskManager::Lane::Audio;
        audioTask.priority = IsPlayerTtsLine(item);
        audioTask.deadlineFromEnqueue = true;
        audioTask.timeout = isPlayerTts ? std::chrono::seconds(15) : kNpcTtsPreparationTimeout;
        audioTask.coalescing = TaskManager::CoalescingPolicy::RejectIfPendingOrActive;
        audioTask.concurrencyLimit = 2;
        const TaskManager::TaskHandle audioTaskHandle = TaskManager::Submit(std::move(audioTask), [item,
                        host = serverHost,
                        port = serverPort,
                        hash = textHash,
                        localSoundcachePath,
                        generation = audioGeneration,
                        maxHttpAttempts,
                        queryGenerationStatus = !isPlayerTts](const TaskManager::CancellationToken& token) {
            // Create a temporary config-like structure for the download
            Log("SpeakManager: [Thread] Starting TTS download for '%s'", item.actor.c_str());
            std::string path = "/DialecticServer/soundcache/" + hash + ".wav";
            std::vector<uint8_t> audioData = ReadLocalSoundcacheWavWithRetry(localSoundcachePath, hash, generation, token);
            if (audioData.empty()) {
                Log("SpeakManager: [Thread] Requesting TTS file: %s", path.c_str());
                audioData = DownloadSoundcacheWavWithRetry(
                    host,
                    port,
                    path,
                    hash,
                    queryGenerationStatus,
                    generation,
                    token,
                    maxHttpAttempts);
            }

            if (audioData.empty()) {
                CompleteFailedAudioDownload(item, generation);
                return;
            }

            PendingAudio completed;
            completed.audioData = std::move(audioData);
            completed.speaker = item.actor;
            completed.actorFormId = item.actorFormId;
            completed.line = item;
            completed.ready = true;
            CompleteAudioDownload(std::move(completed), generation);
            
        });
        if (!audioTaskHandle) {
            Log("SpeakManager: audio preparation task rejected speaker='%s' utterance='%s'",
                item.actor.c_str(), item.utteranceId.c_str());
            CompleteFailedAudioDownload(item, audioGeneration);
        }
    }

    void QueueDialogue(const std::string& text,
                       const std::string& speaker,
                       uint32_t actorFormId,
                       bool isFinalResponseLine,
                       const std::string& listenerHint,
                       const std::string& rechatTargetHint,
                       int rechatDepth,
                       const std::string& ttsCacheKey,
                       const std::string& utteranceId,
                       const std::string& requestId,
                       uint64_t runtimeGeneration,
                       uint32_t listenerFormId,
                       uint32_t rechatTargetFormId,
                       const std::string& displayName) {
        ScriptLine line;
        line.text = text;
        line.actor = speaker;
        line.displayName = displayName;
        line.action = "";
        line.actorFormId = actorFormId;
        line.isFinalResponseLine = isFinalResponseLine;
        line.listenerHint = listenerHint;
        line.rechatTargetHint = rechatTargetHint;
        line.listenerFormId = listenerFormId;
        line.rechatTargetFormId = rechatTargetFormId;
        line.rechatDepth = rechatDepth;
        line.ttsCacheKey = ttsCacheKey;
        line.utteranceId = utteranceId;
        line.requestId = requestId;
        line.sceneKey = CurrentPlayerSceneKey();
        line.runtimeGeneration = runtimeGeneration != 0
            ? runtimeGeneration
            : RuntimeGeneration::Current();
        Log("SpeakManager: QueueDialogue received speaker='%s' final=%d rechatDepth=%d utterance='%s' tts_key='%s' text='%s'",
            line.actor.c_str(),
            line.isFinalResponseLine ? 1 : 0,
            line.rechatDepth,
            line.utteranceId.c_str(),
            line.ttsCacheKey.c_str(),
            PreviewText(line.text).c_str());
        g_dialogueLinesReceived.fetch_add(1, std::memory_order_relaxed);

        if (IsPlayerTtsLine(line)) {
            g_playerAudioLinesReceived.fetch_add(1, std::memory_order_relaxed);
            ClearPlayerInputTtsGate("player_tts_arrived");
            Log("SpeakManager: Queueing player TTS at front after interrupt: '%s'",
                PreviewText(line.text).c_str());
            InsertInQueueFront(line);
            return;
        }

        if (IsPlayerTextOnlyLine(line)) {
            g_playerTextOnlyLinesReceived.fetch_add(1, std::memory_order_relaxed);
            ClearPlayerInputTtsGate("player_text_only_arrived");
            Log("SpeakManager: Displaying text-only player subtitle without audio: '%s'",
                PreviewText(line.text).c_str());
            StartPlayerTextOnlySubtitle(line);
            return;
        }

        bool autonomousResponse = false;
        {
            std::lock_guard<std::mutex> lock(g_rechatMutex);
            autonomousResponse = g_rechatChainAutonomous;
        }
        if (autonomousResponse && line.actorFormId != 0) {
            std::string activityReason;
            if (!ActivityStatusFNV::IsAutomaticDialogueAllowed(line.actorFormId, &activityReason)) {
                Log("SpeakManager: Dropping stale autonomous response for %s (0x%08X): %s",
                    line.actor.c_str(), line.actorFormId, activityReason.c_str());
                std::lock_guard<std::mutex> lock(g_rechatMutex);
                g_rechatChainClosed = true;
                g_pendingRechatRetry = PendingRechatRetry{};
                return;
            }
        }

        InsertInQueue(line);
    }

    void GuardActorForPendingDialogue(uint32_t actorFormId, const std::string& actorName) {
        ScriptLine line;
        line.actor = actorName;
        line.actorFormId = actorFormId;
        StartDialogueGuardBridge(line);
    }

    void BeginPlayerInputTtsGate() {
        std::lock_guard<std::mutex> lock(g_playerTtsGateMutex);
        g_playerInputTtsGateActive = true;
        g_playerInputTtsGateUntil = std::chrono::steady_clock::now() + kPlayerInputTtsGateTimeout;
        Log("SpeakManager: Player TTS interrupt gate active for %lld ms",
            static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(kPlayerInputTtsGateTimeout).count()));
    }

    static void StopSpeakingInternal(bool preservePlayerLines, const char* reason) {
        Log("SpeakManager: Stopping speech (%s)", reason ? reason : "stop");
        if (g_playbackPausedForMenu || AudioManager::IsPaused()) {
            Log("SpeakManager: Clearing menu playback pause because stop path was explicitly requested");
        }
        g_aborted = true;
        g_audioGeneration.fetch_add(1);
        TaskManager::CancelByType("audio_prepare");
        HTTPManager::CancelPendingResponses();
        if (g_currentPlaybackLineActive) {
            SendAbortDeliveryStateIfTracked(g_currentPlaybackLine, reason ? reason : "stop_speaking");
        }
        AudioManager::Stop();
        g_playbackPausedForMenu = false;
        FinalizeLipSync(reason ? reason : "stop_speaking");
        ClearFaceTargetBridge();
        ClearDialogueGuardBridge();
        g_currentPlaybackRechatLaunched = false;
        
        // Normal player interruption preserves queued Player TTS. The hard
        // killswitch clears everything immediately.
        std::vector<ScriptLine> abortedLines;
        std::queue<ScriptLine> tempQueue;
        {
            std::lock_guard<std::mutex> lock(g_queueMutex);
            while (!g_scriptQueue.empty()) {
                const auto& item = g_scriptQueue.front();
                if (preservePlayerLines && IsPlayerTtsLine(item)) {
                    tempQueue.push(item);
                } else {
                    abortedLines.push_back(item);
                }
                g_scriptQueue.pop();
            }
            g_scriptQueue = tempQueue;
        }
        std::queue<ScriptLine> preserved = tempQueue;

        {
            std::lock_guard<std::mutex> pendingLock(g_pendingMutex);
            for (const auto& pendingAudio : g_pendingAudioQueue) {
                if (!(preservePlayerLines && IsPlayerTtsLine(pendingAudio.line))) {
                    abortedLines.push_back(pendingAudio.line);
                }
            }
            g_pendingAudioQueue.clear();
            g_pendingLineSequences.clear();
            while (!preserved.empty()) {
                if (preserved.front().sequence != 0) {
                    g_pendingLineSequences.insert(preserved.front().sequence);
                }
                preserved.pop();
            }
        }
        for (const auto& line : abortedLines) {
            SendAbortDeliveryStateIfTracked(line, reason ? reason : "stop_speaking");
        }
        
        g_isProcessing = false;
        g_currentSpeakerFormId = 0;
        g_currentSpeaker.clear();
        g_currentPlaybackLine = ScriptLine{};
        g_currentPlaybackLineActive = false;
        g_currentPlaybackIsHeadVoice = false;
        AudioManager::Set3DPlaybackEnabled(false);
        AudioManager::SetVolume(GetBaseVoiceVolume());
        ClearSubtitleBridge();
        ClearPlayerInputTtsGate("stop_speaking");
        ActionManager::ClearPostDialogueActions();

        {
            std::lock_guard<std::mutex> rechatLock(g_rechatMutex);
            g_rechatInFlight = false;
            g_rechatInFlightSpeaker.clear();
            g_rechatChainClosed = false;
            g_rechatChainAutonomous = false;
            g_rechatChainId.clear();
            g_lastRechatter.clear();
            g_pendingRechatRetry = PendingRechatRetry{};
            g_pendingRechatLaunchLine = ScriptLine{};
            g_pendingRechatLaunchActive = false;
            g_pendingRechatLaunchUntil = {};
            g_pendingRechatNextCheck = {};
        }
    }

    void CancelDialogueTurn(const char* reason, bool preservePlayerLines, bool suppressRechatBriefly);

    void StopSpeaking() {
        CancelDialogueTurn("stop_speaking", true, false);
    }

    void HaltAllSpeech() {
        CancelDialogueTurn("halt_ai_actions", false, false);
    }

    void ClearAllSpeech(const char* reason) {
        CancelDialogueTurn(reason ? reason : "clear_speech", false, false);
    }

    void InterruptForPlayerInput() {
        CancelDialogueTurn("player_interrupt", true, true);
    }

    void CancelDialogueTurn(const char* reason, bool preservePlayerLines, bool suppressRechatBriefly) {
        const char* cleanReason = reason ? reason : "dialogue_turn_cancel";
        g_dialogueTurnCancellations.fetch_add(1, std::memory_order_relaxed);
        Log("SpeakManager: CancelDialogueTurn reason=%s preserve_player=%d suppress_rechat=%d",
            cleanReason,
            preservePlayerLines ? 1 : 0,
            suppressRechatBriefly ? 1 : 0);
        StopSpeakingInternal(preservePlayerLines, cleanReason);
        {
            std::lock_guard<std::mutex> lock(g_rechatMutex);
            g_rechatInFlight = false;
            g_rechatInFlightSpeaker.clear();
            g_rechatChainClosed = false;
            g_rechatChainId.clear();
            g_lastRechatter.clear();
            g_pendingRechatRetry = PendingRechatRetry{};
            if (suppressRechatBriefly) {
                g_rechatCooldownUntil = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            }
        }
    }

    bool IsSpeaking() {
        return g_isProcessing || AudioManager::IsPlaying();
    }
}
