// GameLoop.cpp - Main game loop integration for Dialectic

#include "GameLoop.h"
#include "PlaythroughNotices.h"
#include "IngameNotifier.h"
#include "ActionManager.h"
#include "InputManager.h"
#include "TargetManager.h"
#include "AgentManager.h"
#include "ActivationManager.h"
#include "ActorEligibilityFNV.h"
#include "Config.h"
#include "Console.h"
#include "HTTPManager.h"
#include "SpeakManager.h"
#include "AudioManager.h"
#include "Logger.h"
#include "NPCDetector.h"
#include "ActorPositionResolverFNV.h"
#include "SpatialAwarenessFNV.h"
#include "SpatialSnapshotManagerFNV.h"
#include "VoiceRecorder.h"
#include "Misc.h"
#include "WorldContextFNV.h"
#include "NearbyActorsFNV.h"
#include "ActivityStatusFNV.h"
#include "AutoGreetingFNV.h"
#include "NearbyItemsFNV.h"
#include "NearbyPoiFNV.h"
#include "QuestJournalFNV.h"
#include "PlayerInventoryManagerFNV.h"
#include "PlayerSurvivalManagerFNV.h"
#include "FalloutStatsManagerFNV.h"
#include "PipVisionManager.h"
#include "ResponseQueueFNV.h"
#include "LoadedPluginsFNV.h"
#include "WorldDataSyncFNV.h"
#include "DialecticInitialization.h"
#include "TradeManager.h"
#include "RuntimeSnapshot.h"
#include "RuntimeGeneration.h"
#include "RuntimeEventBus.h"
#include "NativeComparisonTelemetry.h"
#include "GameThreadDispatcher.h"
#include "XNVSEAdapter.h"
#include "TaskManager.h"
#include "VersionCheck.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <chrono>
#include <atomic>
#include <vector>
#include <deque>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <cstdio>
#include <cctype>
#include <climits>
#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <set>
#include <unordered_map>

#ifndef DIALECTIC_VERSION
#define DIALECTIC_VERSION "1.0.0"
#endif

// Forward declarations
void Log(const char* fmt, ...);
bool Dialectic_RequestVoiceSampleBatch(const char* source);

namespace GameLoop {

// Forward declarations for internal functions
void Update(float deltaTime);  // From header, needs forward decl for thread func
static void UpdateOpenMicMonitoringState();
static void StartVoiceInputInternal(bool openMicTriggered);
void HaltAIActionsNow();
static void CancelDialogueForCombatEntry();
static void CancelDialogueForVanillaDialogueEntry();
static bool IsConversationTargetOnCooldown(uint32_t formId, const std::string& name, bool notify);
static void TriggerDynamicProfileForCurrentTarget();
static void TriggerDynamicProfilesForNearbyAgents();
static void TriggerDynamicProfileForNarrator();
static void UpdateBoredEventTimer();
static int ResetRuntimeForAIActions(const char* reason, bool notifyServer,
    bool haltActorActions, bool clearCapturedDialogue = true);
static void MaybeSendLoadedSaveInit();
static void PrepareTextInputTargetHint();
static void ProcessNativeRuntimeEvents();
static bool EnforceCombatDialogueGate(uint32_t actorFormId, const char* source, bool notify);

// Current game state
static GameState g_gameState;

// Conversation state
static std::atomic<bool> g_conversationActive{false};
static bool g_conversationIsNarrator = false;
static std::string g_conversationPartner;
static std::atomic<uint32_t> g_conversationPartnerFormId{0};
struct ConversationCooldownEntry {
    std::string name;
    std::chrono::steady_clock::time_point until;
};
static std::mutex g_conversationCooldownMutex;
static std::unordered_map<uint32_t, ConversationCooldownEntry> g_conversationCooldownByFormId;
static std::unordered_map<std::string, ConversationCooldownEntry> g_conversationCooldownByName;
static constexpr const char* kNarratorName = "The Narrator";
static constexpr float kNarratorLookUpPitchDegrees = -85.0f;
static bool g_loadedSaveInitSent = false;
static long long g_lastSeenGamets = 0;
static bool g_loadedSaveInitBlocked = false;

// Voice input state
static std::atomic<bool> g_voiceInputActive(false);

// Update timing
static std::chrono::steady_clock::time_point g_lastUpdateTime;
static float g_updateAccumulator = 0.0f;
static const float UPDATE_INTERVAL = 0.1f;  // 100ms update rate
static std::chrono::steady_clock::time_point g_lastDynamicProfileTimerUpdate;
static std::chrono::steady_clock::time_point g_dynamicProfileBlockedAt;
static std::chrono::steady_clock::time_point g_dynamicProfileResumeNotBefore;
static std::chrono::steady_clock::time_point g_lastDynamicProfileLoadDelayAt;
static std::chrono::steady_clock::time_point g_lastBoredEventTimerUpdate;
static std::chrono::steady_clock::time_point g_lastBoredBlockingActivityTime;
static std::chrono::steady_clock::time_point g_lastVoiceSampleToolPoll;
static std::chrono::steady_clock::time_point g_lastModeSelectionPoll;
static std::chrono::steady_clock::time_point g_lastDynamicProfileSelectionPoll;
static std::chrono::steady_clock::time_point g_lastLegacyToolPoll;
static std::chrono::steady_clock::time_point g_lastTextInputPoll;
static std::chrono::steady_clock::time_point g_lastRuntimeConfigFallbackPoll;
static std::chrono::steady_clock::time_point g_lastRpgEventPoll;
static std::atomic<bool> g_runtimeConfigDirty(false);
static std::atomic<DWORD> g_runtimeConfigDirtyTick(0);
static uint32_t g_dialecticControlTargetFormId = 0;
static std::string g_dialecticControlTargetName;

static std::mutex g_runtimeStatusMutex;
static bool g_runtimeStatusPending = false;
static int g_pendingRuntimeModeIndex = 0;
static int g_pendingRuntimeModelSlot = 1;
static std::mutex g_nativeDialogueCaptureMutex;
static std::deque<std::string> g_nativeDialogueCaptures;

struct PerfAggregate {
    long long calls = 0;
    long long totalUs = 0;
    long long maxUs = 0;
    long long slowCalls = 0;
};

static std::unordered_map<std::string, PerfAggregate> g_updatePerfAggregates;
static std::chrono::steady_clock::time_point g_lastUpdatePerfSummaryTime;
static uint64_t g_updatePerfTickCount = 0;
static constexpr long long kPerfSlowSubsystemUs = 8000;
static constexpr long long kPerfSlowFrameUs = 25000;
static constexpr auto kPerfSummaryInterval = std::chrono::seconds(5);

static bool ShouldPoll(std::chrono::steady_clock::time_point& lastPoll,
                       std::chrono::milliseconds interval) {
    const auto now = std::chrono::steady_clock::now();
    if (lastPoll.time_since_epoch().count() != 0 && now - lastPoll < interval) {
        return false;
    }
    lastPoll = now;
    return true;
}

static long long ElapsedUsSince(const std::chrono::steady_clock::time_point& start) {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count();
}

static void RecordUpdatePerf(const char* name, long long elapsedUs) {
    PerfAggregate& aggregate = g_updatePerfAggregates[name ? name : "unknown"];
    aggregate.calls += 1;
    aggregate.totalUs += elapsedUs;
    aggregate.maxUs = std::max(aggregate.maxUs, elapsedUs);
    if (elapsedUs >= kPerfSlowSubsystemUs) {
        aggregate.slowCalls += 1;
        Logger::LogInfo("[PERF] GameLoop subsystem slow name=%s elapsed_ms=%.3f",
            name ? name : "unknown",
            static_cast<double>(elapsedUs) / 1000.0);
    }
}

template <typename Fn>
static void ProfileUpdateSubsystem(const char* name, Fn&& fn) {
    const auto start = std::chrono::steady_clock::now();
    fn();
    RecordUpdatePerf(name, ElapsedUsSince(start));
}

static void MaybeLogUpdatePerfSummary(long long frameElapsedUs) {
    g_updatePerfTickCount += 1;
    const auto now = std::chrono::steady_clock::now();
    if (g_lastUpdatePerfSummaryTime.time_since_epoch().count() != 0 &&
        now - g_lastUpdatePerfSummaryTime < kPerfSummaryInterval) {
        return;
    }

    const long long windowMs = g_lastUpdatePerfSummaryTime.time_since_epoch().count() == 0
        ? 0
        : std::chrono::duration_cast<std::chrono::milliseconds>(now - g_lastUpdatePerfSummaryTime).count();
    g_lastUpdatePerfSummaryTime = now;

    Logger::LogInfo("[PERF] GameLoop summary window_ms=%lld ticks=%llu last_frame_ms=%.3f subsystems=%zu",
        windowMs,
        static_cast<unsigned long long>(g_updatePerfTickCount),
        static_cast<double>(frameElapsedUs) / 1000.0,
        g_updatePerfAggregates.size());

    for (const auto& entry : g_updatePerfAggregates) {
        const PerfAggregate& aggregate = entry.second;
        const double avgMs = aggregate.calls > 0
            ? static_cast<double>(aggregate.totalUs) / static_cast<double>(aggregate.calls) / 1000.0
            : 0.0;
        Logger::LogInfo("[PERF]   %s calls=%lld total_ms=%.3f avg_ms=%.3f max_ms=%.3f slow_calls=%lld",
            entry.first.c_str(),
            aggregate.calls,
            static_cast<double>(aggregate.totalUs) / 1000.0,
            avgMs,
            static_cast<double>(aggregate.maxUs) / 1000.0,
            aggregate.slowCalls);
    }

    g_updatePerfAggregates.clear();
    g_updatePerfTickCount = 0;
}

static std::string TrimInput(std::string value) {
    const char* whitespace = " \t\r\n";
    const size_t start = value.find_first_not_of(whitespace);
    if (start == std::string::npos) {
        return "";
    }
    const size_t end = value.find_last_not_of(whitespace);
    return value.substr(start, end - start + 1);
}

static std::string ToLowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

static std::string ToUpperCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return value;
}

static uint32_t ParseFormIdString(const std::string& rawValue) {
    std::string value = TrimInput(rawValue);
    if (value.empty()) {
        return 0;
    }
    if (value.size() > 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) {
        value = value.substr(2);
    }
    try {
        return static_cast<uint32_t>(std::stoul(value, nullptr, 16));
    } catch (...) {
        return 0;
    }
}

static std::string FormatFormIdJsonValue(uint32_t formId) {
    if (formId == 0) {
        return "";
    }
    std::ostringstream hex;
    hex << "0x" << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << formId;
    return hex.str();
}

static bool LooksLikeMetadataToken(const std::string& value) {
    const std::string token = TrimInput(value);
    if (token.size() > 80) {
        return false;
    }

    for (char ch : token) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (!std::isalnum(c) && ch != ' ' && ch != '_' && ch != '-' && ch != '.' && ch != '#') {
            return false;
        }
    }

    return true;
}

static void AppendUniqueAudienceName(
    std::vector<std::string>& names,
    std::set<std::string>& seen,
    const std::string& rawName) {
    const std::string name = TrimInput(rawName);
    if (name.empty() || name == "<no name>") {
        return;
    }

    std::string key = name;
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

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

static bool IsConversationTargetEligible(
    uint32_t formId,
    const std::string& rawName,
    bool notify,
    bool creatureKnown = false,
    bool isCreature = false) {
    const std::string name = TrimInput(rawName);
    if (formId == 0 || name.empty()) {
        return false;
    }

    ActorEligibilityFNV::Metadata identityMetadata;
    identityMetadata.name = name;
    std::string identityReason;
    if (!ActorEligibilityFNV::IsTargetableActorIdentity(identityMetadata, &identityReason)) {
        Logger::LogInfo("GameLoop: Conversation target rejected: %s (0x%08X): %s",
            name.c_str(), formId, identityReason.c_str());
        return false;
    }

    // Creatures remain available through explicit Manual Activate, matching CHIM's policy.
    if (Config::autoAddCreatures ||
        AgentManager::IsManuallyActivated(formId)) {
        return true;
    }

    ActorEligibilityFNV::Metadata metadata;
    metadata.name = name;
    metadata.isCreatureKnown = creatureKnown;
    metadata.isCreature = isCreature;

    const auto position = ActorPositionResolverFNV::ResolveActor(formId);
    if (position.resolved) {
        metadata = EligibilityMetadataFromPosition(position);
        if (metadata.name.empty()) {
            metadata.name = name;
        }
    }

    RuntimeSnapshot::ActorState actor;
    if (RuntimeSnapshot::TryGetActor(formId, actor)) {
        if (metadata.name.empty()) metadata.name = actor.name;
        if (metadata.race.empty()) metadata.race = actor.raceName;
        if (metadata.baseId.empty() && actor.baseFormId != 0) {
            std::ostringstream baseId;
            baseId << "0x" << std::hex << std::setw(8) << std::setfill('0') << actor.baseFormId;
            metadata.baseId = baseId.str();
        }
        if (!metadata.baseTypeKnown && actor.baseType != 0) {
            metadata.baseType = actor.baseType;
            metadata.baseTypeKnown = true;
        }
        metadata.isCreature = actor.creature;
        metadata.isCreatureKnown = true;
    }

    std::string reason;
    const bool hasCategoryMetadata = metadata.baseTypeKnown ||
        metadata.isCreatureKnown ||
        !metadata.race.empty() ||
        !metadata.voiceId.empty() ||
        !metadata.voiceName.empty();
    const bool allowed = hasCategoryMetadata
        ? ActorEligibilityFNV::IsAutoActivationAllowed(metadata, &reason)
        : !ActorEligibilityFNV::IsClearlyDisallowedCreature(metadata, &reason);
    if (allowed) {
        return true;
    }

    Logger::LogInfo("GameLoop: Conversation target rejected by creature policy: %s (0x%08X): %s",
        name.c_str(), formId, reason.empty() ? "not eligible" : reason.c_str());
    if (notify) {
        Console::Print("[DIALECTIC] Manually activate %s before talking.", name.c_str());
    }
    return false;
}

static bool AudiencePositionEligible(const ActorPositionResolverFNV::PositionResult& position) {
    if (!position.resolved || position.formId == 0) {
        return false;
    }
    if (!ActorPositionResolverFNV::IsPositionInPlayerScene(position)) {
        return false;
    }
    if (position.disabledKnown && position.isDisabled) {
        return false;
    }
    if (position.deadKnown && position.isDead) {
        return false;
    }

    return ActorEligibilityFNV::IsRechatAllowed(
        EligibilityMetadataFromPosition(position),
        AgentManager::IsManuallyActivated(position.formId));
}

static bool AudienceNameHintAllowed(const std::string& rawName) {
    const std::string name = TrimInput(rawName);
    if (name.empty() || name == "<no name>") {
        return false;
    }

    const uint32_t formId = AgentManager::FindAgentFormIdByName(name);
    if (formId != 0 && AgentManager::IsManuallyActivated(formId)) {
        return true;
    }

    ActorEligibilityFNV::Metadata metadata;
    metadata.name = name;
    return !ActorEligibilityFNV::IsClearlyDisallowedCreature(metadata);
}

static void AppendEligibleAudienceHintName(
    std::vector<std::string>& names,
    std::set<std::string>& seen,
    const std::string& rawName) {
    if (!AudienceNameHintAllowed(rawName)) {
        return;
    }
    AppendUniqueAudienceName(names, seen, rawName);
}

static void AppendConversationPartnerAudienceName(
    std::vector<std::string>& names,
    std::set<std::string>& seen) {
    if (g_conversationPartnerFormId != 0 &&
        !ActorPositionResolverFNV::IsActorInPlayerScene(g_conversationPartnerFormId)) {
        return;
    }

    AppendEligibleAudienceHintName(names, seen, g_conversationPartner);
}

static bool ConversationPartnerClearlyOutsideCurrentScene() {
    if (!g_conversationActive || g_conversationIsNarrator || g_conversationPartnerFormId == 0) {
        return false;
    }

    const auto position = ActorPositionResolverFNV::ResolveActor(g_conversationPartnerFormId);
    return position.resolved && !ActorPositionResolverFNV::IsPositionInPlayerScene(position);
}

static void ClearConversationIfPartnerLeftScene(const char* reason) {
    if (!ConversationPartnerClearlyOutsideCurrentScene()) {
        return;
    }

    const uint32_t oldPartnerFormId = g_conversationPartnerFormId;
    const std::string oldPartner = g_conversationPartner;
    Log("GameLoop: Clearing conversation with %s (0x%08X), partner left current scene: %s",
        oldPartner.c_str(),
        oldPartnerFormId,
        reason ? reason : "scene validation");
    Console::Print("[DIALECTIC] %s left the scene", oldPartner.empty() ? "Conversation target" : oldPartner.c_str());
    StopConversation();

    const auto& currentTarget = TargetManager::GetCurrentTarget();
    if (currentTarget.formId == oldPartnerFormId) {
        TargetManager::ClearCurrentTarget();
    }
}

static float GetPlayerSpeechDistanceMultiplier();
static float GetConversationTargetRadius();
static bool IsStealthPlayerInputActive();
static bool EqualsIgnoreCase(const std::string& left, const std::string& right);

static bool IsPrivateConversationMode() {
    return EqualsIgnoreCase(Config::currentMode, "WHISPER") ||
        EqualsIgnoreCase(Config::currentMode, "CLOSE");
}

static std::string BuildAudienceSnapshotJson(const std::string& source = "") {
    std::vector<std::string> names;
    std::set<std::string> seen;
    const float playerSpeechDistanceMultiplier = GetPlayerSpeechDistanceMultiplier();
    const bool closeMode = EqualsIgnoreCase(Config::currentMode, "CLOSE");
    const float closeRadius = closeMode ? GetConversationTargetRadius() : 0.0f;

    AppendConversationPartnerAudienceName(names, seen);
    AppendUniqueAudienceName(names, seen, Config::playerName.empty() ? "Player" : Config::playerName);

    const auto player = ActorPositionResolverFNV::ResolvePlayer();
    if (player.resolved) {
        struct AudienceCandidate {
            std::string name;
            float distance = 0.0f;
        };

        std::vector<AudienceCandidate> candidates;
        const auto positions = ActorPositionResolverFNV::GetRecentActorPositions();
        const float nearbyMaxDistance = Config::nearbyActorsMaxDistance > 0.0f
            ? Config::nearbyActorsMaxDistance
            : std::max(Config::distanceActivatingNpcInterior, Config::distanceActivatingNpcExterior);
        for (const auto& position : positions) {
            if (!position.resolved ||
                position.formId == 0 ||
                position.formId == 0x00000014 ||
                position.actorName.empty() ||
                position.actorName == "<no name>") {
                continue;
            }

            const auto spatial = SpatialAwarenessFNV::Evaluate(player, position);
            const float effectiveMaxDistance = closeMode
                ? closeRadius
                : (spatial.maxDistance > 0.0f
                    ? spatial.maxDistance * playerSpeechDistanceMultiplier
                    : 0.0f);
            bool canHearPlayer = spatial.canCommunicate;
            if (!canHearPlayer &&
                playerSpeechDistanceMultiplier > 1.0f &&
                (spatial.reason == "too_far" || spatial.reason == "too_far_shout") &&
                effectiveMaxDistance > 0.0f &&
                spatial.airDistance <= effectiveMaxDistance) {
                canHearPlayer = true;
            }

            const bool withinSpeechRange = canHearPlayer &&
                (effectiveMaxDistance <= 0.0f || spatial.airDistance <= effectiveMaxDistance);
            const bool withinNearbyRange = std::isfinite(spatial.airDistance) &&
                nearbyMaxDistance > 0.0f &&
                spatial.airDistance <= nearbyMaxDistance;
            const bool managedActor = AgentManager::IsManuallyActivated(position.formId) ||
                AgentManager::IsAutoManaged(position.formId);
            const bool includeInAudience = closeMode
                ? withinSpeechRange
                : (withinSpeechRange || withinNearbyRange || managedActor);
            if (!includeInAudience) {
                continue;
            }

            if (!AudiencePositionEligible(position)) {
                continue;
            }

            candidates.push_back({ position.actorName, spatial.airDistance });
        }

        std::sort(candidates.begin(), candidates.end(), [](const AudienceCandidate& a, const AudienceCandidate& b) {
            return a.distance < b.distance;
        });

        constexpr size_t kMaxAudienceNames = 12;
        for (const auto& candidate : candidates) {
            if (names.size() >= kMaxAudienceNames) {
                break;
            }
            AppendUniqueAudienceName(names, seen, candidate.name);
        }
    }

    std::ostringstream people;
    people << "|";
    for (const auto& name : names) {
        people << name << "|";
    }

    std::ostringstream json;
    json << "{\"people\":\"" << HTTPManager::EscapeJson(people.str()) << "\"";
    if (!source.empty()) {
        json << ",\"source\":\"" << HTTPManager::EscapeJson(source) << "\"";
    }
    json << "}";
    return json.str();
}

static std::string ExtractPeopleFromAudienceSnapshotJson(const std::string& audienceSnapshotJson) {
    const std::string marker = "\"people\":\"";
    const size_t start = audienceSnapshotJson.find(marker);
    if (start == std::string::npos) {
        return "";
    }

    size_t valueStart = start + marker.size();
    std::string people;
    bool escaping = false;
    for (size_t i = valueStart; i < audienceSnapshotJson.size(); ++i) {
        const char ch = audienceSnapshotJson[i];
        if (escaping) {
            people.push_back(ch);
            escaping = false;
            continue;
        }
        if (ch == '\\') {
            escaping = true;
            continue;
        }
        if (ch == '"') {
            break;
        }
        people.push_back(ch);
    }
    return people;
}

static bool IsUsableDetectedPlayerName(const std::string& value) {
    const std::string name = TrimInput(value);
    if (name.empty() || name.size() > 80) {
        return false;
    }

    const std::string lower = Misc::ToLower(name);
    return lower != "player" &&
           lower != "courier" &&
           lower != "prisoner" &&
           lower != "unknown" &&
           lower != "unknown player" &&
           lower != "the narrator";
}

static void RefreshPlayerNameFromGame() {
    const std::string detectedName = TrimInput(Misc::GetPlayerName());
    if (!IsUsableDetectedPlayerName(detectedName)) {
        return;
    }

    if (Config::playerName != detectedName) {
        Log("GameLoop: Detected player name [%s]", detectedName.c_str());
        Config::playerName = detectedName;
    }
}

static std::string StripDialogueMetadata(std::string message) {
    message = TrimInput(message);

    std::vector<std::string> parts;
    std::string part;
    std::istringstream stream(message);
    while (std::getline(stream, part, '/')) {
        parts.push_back(TrimInput(part));
    }

    if (parts.size() >= 4 && !parts[0].empty()) {
        bool metadataTail = true;
        for (size_t i = 1; i < parts.size(); ++i) {
            if (parts[i].empty()) {
                continue;
            }
            if (!LooksLikeMetadataToken(parts[i])) {
                metadataTail = false;
                break;
            }
        }
        if (metadataTail) {
            message = parts[0];
        }
    }

    const char* labels[] = { "mood:", "emotion:", "action:", "animation:", "speaker:", "listener:", "target:" };
    bool changed = true;
    while (changed) {
        changed = false;
        const size_t open = message.find('[');
        const size_t close = open == std::string::npos ? std::string::npos : message.find(']', open + 1);
        if (open == std::string::npos || close == std::string::npos) {
            break;
        }

        std::string tag = message.substr(open + 1, close - open - 1);
        std::transform(tag.begin(), tag.end(), tag.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

        for (const char* label : labels) {
            if (tag.rfind(label, 0) == 0) {
                message.erase(open, close - open + 1);
                changed = true;
                break;
            }
        }
    }

    return TrimInput(message);
}

static bool EqualsIgnoreCase(const std::string& left, const std::string& right) {
    if (left.size() != right.size()) {
        return false;
    }

    for (size_t i = 0; i < left.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(left[i])) !=
            std::tolower(static_cast<unsigned char>(right[i]))) {
            return false;
        }
    }
    return true;
}

static bool IsPlayerSpeakerName(const std::string& speaker) {
    return EqualsIgnoreCase(speaker, "Player") ||
        (!Config::playerName.empty() && EqualsIgnoreCase(speaker, Config::playerName));
}

static std::string PlayerDisplayName() {
    std::string name = TrimInput(Config::playerName);
    if (name.empty() || EqualsIgnoreCase(name, "Player")) {
        name = TrimInput(Misc::GetPlayerName());
    }
    return name.empty() ? "Player" : name;
}

static std::string NormalizePlayerSpeakerForDisplay(const std::string& speaker) {
    if (!IsPlayerSpeakerName(speaker)) {
        return speaker;
    }

    return PlayerDisplayName();
}

static std::string BuildPrivateNarratorAudienceSnapshotJson() {
    std::ostringstream people;
    people << "|" << (Config::playerName.empty() ? "Player" : Config::playerName) << "|" << kNarratorName << "|";

    std::ostringstream json;
    json << "{\"people\":\"" << HTTPManager::EscapeJson(people.str()) << "\",\"private\":true}";
    return json.str();
}

static std::string BuildTargetOnlyAudienceSnapshotJson(bool privateConversation = false) {
    const std::string playerName = Config::playerName.empty() ? "Player" : Config::playerName;
    std::ostringstream people;
    people << "|" << playerName << "|";
    if (!g_conversationPartner.empty()) {
        people << g_conversationPartner << "|";
    }

    std::ostringstream json;
    json << "{\"people\":\"" << HTTPManager::EscapeJson(people.str()) << "\",\"target_only\":true";
    if (privateConversation) {
        json << ",\"private\":true,\"privacy_scope\":\"target_only\"";
    }
    if (g_conversationPartnerFormId != 0) {
        json << ",\"target_form_id\":\"0x"
             << std::hex << std::setw(8) << std::setfill('0') << g_conversationPartnerFormId << std::dec
             << "\"";
    }
    json << "}";
    return json.str();
}

static float GetPlayerSpeechDistanceMultiplier() {
    std::string mode = Config::currentMode;
    std::transform(mode.begin(), mode.end(), mode.begin(),
        [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });

    float multiplier = 1.0f;
    if (mode == "WHISPER") {
        multiplier = 0.35f;
    } else if (mode == "SHOUT") {
        multiplier = 2.0f;
    }

    if (ActorPositionResolverFNV::IsPlayerSneaking()) {
        multiplier *= 0.5f;
    }

    return std::clamp(multiplier, 0.05f, 4.0f);
}

static float GetConversationTargetRadius() {
    constexpr float kDefaultTargetRadius = 500.0f;
    constexpr float kCloseRadius = 200.0f;
    if (EqualsIgnoreCase(Config::currentMode, "CLOSE")) {
        return ActorPositionResolverFNV::IsPlayerSneaking()
            ? kCloseRadius * 0.5f
            : kCloseRadius;
    }
    return kDefaultTargetRadius * GetPlayerSpeechDistanceMultiplier();
}

// Whisper and Close require the selected listener to remain within their request-local radius.
static bool IsConversationTargetWithinModeRadius(uint32_t formId, const std::string& name, bool notify) {
    if (!IsPrivateConversationMode() || formId == 0) {
        return true;
    }

    const auto player = ActorPositionResolverFNV::ResolvePlayer();
    const auto target = ActorPositionResolverFNV::ResolveActor(formId);
    if (!player.resolved || !target.resolved) {
        Logger::LogDebug("GameLoop: Private mode range unavailable for %s (0x%08X); preserving explicit target",
            name.c_str(), formId);
        return true;
    }

    const auto spatial = SpatialAwarenessFNV::Evaluate(player, target);
    const float radius = GetConversationTargetRadius();
    if (std::isfinite(spatial.airDistance) && spatial.airDistance <= radius) {
        return true;
    }

    Logger::LogInfo("GameLoop: %s mode rejected %s (0x%08X), distance=%.1f radius=%.1f",
        Config::currentMode.c_str(), name.c_str(), formId, spatial.airDistance, radius);
    if (notify) {
        Console::Print("[DIALECTIC] Move closer to %s", name.empty() ? "the target" : name.c_str());
    }
    return false;
}

static bool IsStealthPlayerInputActive() {
    return ActorPositionResolverFNV::IsPlayerSneaking();
}

static bool StartsWithHeyNarrator(const std::string& rawMessage) {
    std::string message = TrimInput(rawMessage);
    message = ToLowerCopy(message);
    return message == "hey narrator" ||
           message.rfind("hey narrator ", 0) == 0 ||
           message.rfind("hey, narrator", 0) == 0 ||
           message.rfind("narrator ", 0) == 0;
}

static bool IsLookingUpForNarrator() {
    float pitchDegrees = 0.0f;
    if (!Misc::GetPlayerPitchDegrees(pitchDegrees)) {
        Logger::LogDebug("GameLoop: Narrator pitch check unavailable");
        return false;
    }

    Logger::LogInfo("GameLoop: Player pitch for narrator route %.2f degrees", pitchDegrees);
    return pitchDegrees < kNarratorLookUpPitchDegrees;
}

static bool ShouldRouteToNarrator(const std::string& message, bool allowPitchRoute) {
    if (Config::narratorModeEnabled || EqualsIgnoreCase(Config::currentMode, "NARRATOR")) {
        Logger::LogInfo("GameLoop: Narrator route selected by Narrator Mode");
        return true;
    }

    if (StartsWithHeyNarrator(message)) {
        Logger::LogInfo("GameLoop: Narrator route selected by explicit phrase");
        return true;
    }

    if (allowPitchRoute && IsLookingUpForNarrator()) {
        Logger::LogInfo("GameLoop: Narrator route selected by sky pitch");
        return true;
    }

    return false;
}

static void StartNarratorConversation(const char* reason) {
    g_conversationActive = true;
    g_conversationIsNarrator = true;
    g_conversationPartner = kNarratorName;
    g_conversationPartnerFormId = 0;
    RefreshPlayerNameFromGame();
    Logger::LogInfo("GameLoop: Started narrator conversation (%s)", reason ? reason : "unknown");

    const std::string playerName = Config::playerName.empty() ? "Player" : Config::playerName;
    std::ostringstream payload;
    payload << "{"
            << "\"schema\":\"dialectic.conversation.v1\","
            << "\"action\":\"conversation_start\","
            << "\"npc\":\"" << HTTPManager::EscapeJson(g_conversationPartner) << "\","
            << "\"npc_id\":\"0x00000000\","
            << "\"player\":\"" << HTTPManager::EscapeJson(playerName) << "\","
            << "\"target\":{\"name\":\"" << HTTPManager::EscapeJson(g_conversationPartner) << "\",\"refid\":\"0x00000000\"},"
            << "\"player_actor\":{\"name\":\"" << HTTPManager::EscapeJson(playerName) << "\"},"
            << "\"private\":true,"
            << "\"game\":\"fnv\""
            << "}";

    HTTPManager::SendEvent("conversation_start", payload.str(), BuildPrivateNarratorAudienceSnapshotJson());
}

static constexpr const char* kOpenTextInputMenuPath = "Data\\NVSE\\Plugins\\dialectic_open_text_input.tmp";
static constexpr const char* kTextInputTargetPath = "Data\\NVSE\\Plugins\\dialectic_textinput_target.tmp";
static constexpr const char* kTextInputStatusPath = "Data\\NVSE\\Plugins\\dialectic_textinput_status.tmp";
static constexpr const char* kModeSelectPath = "Data\\NVSE\\Plugins\\dialectic_mode_select.tmp";
static constexpr const char* kDynamicProfileSelectPath = "Data\\NVSE\\Plugins\\dialectic_dynamic_profile_select.tmp";
static constexpr const char* kRuntimeConfigReloadPath = "Data\\NVSE\\Plugins\\dialectic_reload_runtime.tmp";
static std::atomic<bool> g_textInputMenuPending(false);
static std::atomic<DWORD> g_textInputMenuRequestTick(0);
static std::atomic<DWORD> g_textInputMenuBlockUntilTick(0);

struct PreparedTextInputTarget {
    uint32_t formId = 0;
    std::string name;
    std::uint64_t runtimeGeneration = 0;
};

static PreparedTextInputTarget g_preparedTextInputTarget;

static bool WriteToolBridgeSignal(const char* path, const char* label) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        Logger::LogWarning("GameLoop: Failed to write tool bridge signal: %s", path ? path : "<null>");
        return false;
    }

    out << (label ? label : "1") << "\n";
    return true;
}

static void ClearToolBridgeFile(const char* path) {
    if (!path || !*path) {
        return;
    }

    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        return;
    }

    if (std::remove(path) != 0) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (out.is_open()) {
            out << "";
        }
    }
}

static bool IsTickBefore(DWORD now, DWORD until) {
    return until != 0 && static_cast<LONG>(until - now) > 0;
}

static void MarkTextInputMenuClosed(const char* reason) {
    g_textInputMenuPending.store(false);
    g_textInputMenuRequestTick.store(0);
    g_textInputMenuBlockUntilTick.store(GetTickCount() + 1500);
    ClearToolBridgeFile(kOpenTextInputMenuPath);
    ClearToolBridgeFile(kTextInputTargetPath);
    Logger::LogInfo("GameLoop: Text input chatbox closed (%s)", reason ? reason : "unknown");
}

bool IsTextInputMenuActiveOrRecentlyClosed() {
    if (g_textInputMenuPending.load()) {
        return true;
    }
    return IsTickBefore(GetTickCount(), g_textInputMenuBlockUntilTick.load());
}

static bool ReadToolBridgeLine(const char* path, std::string& line) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return false;
    }

    if (!std::getline(in, line)) {
        return false;
    }

    line = TrimInput(line);
    return !line.empty();
}

static bool PollToolBridgeInt(const char* path, int& value) {
    std::string raw;
    if (!ReadToolBridgeLine(path, raw)) {
        return false;
    }

    try {
        value = std::stoi(raw);
    } catch (...) {
        Logger::LogWarning("GameLoop: Invalid integer in tool bridge file %s: %s", path ? path : "<null>", raw.c_str());
        ClearToolBridgeFile(path);
        return false;
    }

    ClearToolBridgeFile(path);
    return true;
}

static void PollVoiceSampleToolRequest() {
    const auto now = std::chrono::steady_clock::now();
    if (g_lastVoiceSampleToolPoll.time_since_epoch().count() != 0 &&
        now - g_lastVoiceSampleToolPoll < std::chrono::seconds(1)) {
        return;
    }
    g_lastVoiceSampleToolPoll = now;

    const int sendVoiceSamples = Config::ReadINIInt("Tools", "SendVoiceSamples", 0);
    if (sendVoiceSamples <= 0) {
        return;
    }

    Config::WriteCustomINIValue("Tools", "SendVoiceSamples", "0");
    Logger::LogInfo("GameLoop: detected Tools:SendVoiceSamples=%d; launching voice sample batch",
        sendVoiceSamples);
    Dialectic_RequestVoiceSampleBatch("Tools INI flag");
}

static void PollDynamicProfileSelection() {
    int selectedProfileAction = -1;
    if (PollToolBridgeInt(kDynamicProfileSelectPath, selectedProfileAction)) {
        Logger::LogInfo("GameLoop: detected dynamic profile menu selection=%d", selectedProfileAction);
        if (selectedProfileAction == 0) {
            TriggerDynamicProfileForCurrentTarget();
        } else if (selectedProfileAction == 1) {
            TriggerDynamicProfilesForNearbyAgents();
        } else if (selectedProfileAction == 2) {
            TriggerDynamicProfileForNarrator();
        } else {
            Logger::LogWarning("GameLoop: Ignoring unknown dynamic profile menu selection=%d", selectedProfileAction);
        }
    }

}

static void PollLegacyDynamicProfileToolRequests() {
    const int updateTargetProfile = Config::ReadINIInt("Tools", "UpdateTargetProfile", 0);
    if (updateTargetProfile > 0) {
        Config::WriteCustomINIValue("Tools", "UpdateTargetProfile", "0");
        Logger::LogInfo("GameLoop: detected Tools:UpdateTargetProfile=%d", updateTargetProfile);
        TriggerDynamicProfileForCurrentTarget();
    }

    const int updateNearbyProfiles = Config::ReadINIInt("Tools", "UpdateNearbyProfiles", 0);
    if (updateNearbyProfiles > 0) {
        Config::WriteCustomINIValue("Tools", "UpdateNearbyProfiles", "0");
        Logger::LogInfo("GameLoop: detected Tools:UpdateNearbyProfiles=%d", updateNearbyProfiles);
        TriggerDynamicProfilesForNearbyAgents();
    }

    const int updateNarratorProfile = Config::ReadINIInt("Tools", "UpdateNarratorProfile", 0);
    if (updateNarratorProfile > 0) {
        Config::WriteCustomINIValue("Tools", "UpdateNarratorProfile", "0");
        Logger::LogInfo("GameLoop: detected Tools:UpdateNarratorProfile=%d", updateNarratorProfile);
        TriggerDynamicProfileForNarrator();
    }
}

static const char* ModeNameFromIndex(int modeIndex) {
    switch (modeIndex) {
        case 0: return "STANDARD";
        case 1: return "WHISPER";
        case 2: return "CLOSE";
        case 3: return "SHOUT";
        case 4: return "NARRATOR";
        case 5: return "DIRECTOR";
        case 6: return "INJECTION_LOG";
        case 7: return "INJECTION_CHAT";
        case 8: return "CHEATMODE";
        default: return "STANDARD";
    }
}

static const char* ModeLabelFromIndex(int modeIndex) {
    switch (modeIndex) {
        case 0: return "Standard";
        case 1: return "Whisper";
        case 2: return "Close";
        case 3: return "Shout";
        case 4: return "Narrator";
        case 5: return "Director";
        case 6: return "Inject Event";
        case 7: return "Inject & Chat";
        case 8: return "Cheat Mode";
        default: return "Standard";
    }
}

static const char* ModeTitleNameFromIndex(int modeIndex) {
    // Keep titles aligned with the user-facing menu labels instead of internal mode names.
    switch (modeIndex) {
        case 6: return "INJECT EVENT";
        case 7: return "INJECT & CHAT";
        case 8: return "CHEAT";
        default: return ModeNameFromIndex(modeIndex);
    }
}

static int ModeIndexFromName(const std::string& rawMode) {
    const std::string mode = ToUpperCopy(TrimInput(rawMode));
    for (int i = 0; i <= 8; ++i) {
        if (mode == ModeNameFromIndex(i)) {
            return i;
        }
    }
    return 0;
}

static const char* ProfileModelLabelFromSlot(int slot) {
    switch (std::clamp(slot, 1, 4)) {
        case 1: return "Standard";
        case 2: return "Fast";
        case 3: return "Powerful";
        case 4: return "Experimental";
        default: return "Standard";
    }
}

static void MaybeSyncRuntimeStateFromServer(bool force = false);

static void ApplyModeIndex(int modeIndex, bool sendServerUpdate) {
    modeIndex = std::clamp(modeIndex, 0, 8);
    const char* modeName = ModeNameFromIndex(modeIndex);
    const char* modeLabel = ModeLabelFromIndex(modeIndex);

    Config::currentModeIndex = modeIndex;
    Config::currentMode = modeName;
    Config::narratorModeEnabled = modeIndex == 4;

    if (sendServerUpdate) {
        Console::Print("[DIALECTIC] Mode: %s", modeLabel);
    }
    Logger::LogInfo("GameLoop: Dialectic mode set to %s (%d)", modeName, modeIndex);

    if (sendServerUpdate) {
        std::ostringstream payload;
        payload << "{"
                << "\"schema\":\"dialectic.setconf.v1\","
                << "\"setting\":\"dialectic_mode\","
                << "\"value\":\"" << HTTPManager::EscapeJson(modeName) << "\""
                << "}";
        HTTPManager::SendEvent("setconf", payload.str());
    }
}

void NoteProfileModelSelection(int slot) {
    const int modelSlot = std::clamp(slot, 1, 4);
    if (modelSlot == Config::currentProfileModelSlot) {
        return;
    }
    Logger::LogInfo("GameLoop: LLM model slot selection %d -> %d (%s)",
        Config::currentProfileModelSlot,
        modelSlot,
        ProfileModelLabelFromSlot(modelSlot));
    Config::currentProfileModelSlot = modelSlot;
}

static void PollModeSelection() {
    int selectedModeIndex = 0;
    if (PollToolBridgeInt(kModeSelectPath, selectedModeIndex)) {
        Logger::LogInfo("GameLoop: detected mode menu selection=%d", selectedModeIndex);
        ApplyModeIndex(selectedModeIndex, true);
    }
}

void MarkRuntimeConfigDirty() {
    g_runtimeConfigDirty.store(true);
    g_runtimeConfigDirtyTick.store(GetTickCount());
}

static void PollRuntimeConfigReloadFallback() {
    std::string signal;
    if (!ReadToolBridgeLine(kRuntimeConfigReloadPath, signal)) {
        return;
    }
    ClearToolBridgeFile(kRuntimeConfigReloadPath);
    MarkRuntimeConfigDirty();
}

static void ApplyPendingRuntimeConfigReload() {
    if (!g_runtimeConfigDirty.load() || g_gameState.isInMenu || g_gameState.isLoading) {
        return;
    }
    const DWORD dirtyTick = g_runtimeConfigDirtyTick.load();
    if (dirtyTick != 0 && GetTickCount() - dirtyTick < 250) {
        return;
    }

    Config::LoadRuntimeSettings();
    InputManager::LoadConfig();
    g_runtimeConfigDirty.store(false);
    g_runtimeConfigDirtyTick.store(0);
    ClearToolBridgeFile(kRuntimeConfigReloadPath);
    Logger::LogInfo("GameLoop: Applied deferred runtime settings after menu close");
}

static bool IsToolMenuBlocked(const RuntimeSnapshot::GameState& state) {
    return !state.inGame || state.paused || state.pipboyOpen ||
        state.pauseMenuOpen || state.dialogueMenuOpen || state.barterMenuOpen ||
        state.containerMenuOpen || state.loadingMenuOpen;
}

void RequestDialecticControlMenuOpen() {
    const RuntimeSnapshot::GameState state = RuntimeSnapshot::GetGameState();
    if (IsToolMenuBlocked(state)) {
        Logger::LogInfo("GameLoop: Ignoring Dialectic Control while a blocking menu is open");
        return;
    }

    NPCDetector::NPCInfo waitTarget = NPCDetector::GetCrosshairNPC();
    if (!waitTarget.isValid || waitTarget.isDead || waitTarget.formId == 0x00000014) {
        waitTarget = NPCDetector::GetClosestNPC();
    }
    if (waitTarget.isValid && !waitTarget.isDead && waitTarget.formId != 0x00000014) {
        g_dialecticControlTargetFormId = waitTarget.formId;
        g_dialecticControlTargetName = waitTarget.name;
        Logger::LogInfo("GameLoop: DIALECTIC Control captured Wait Here target %s (0x%08X) at %.1f units",
            waitTarget.name.c_str(), waitTarget.formId, waitTarget.distance);
    } else {
        g_dialecticControlTargetFormId = 0;
        g_dialecticControlTargetName.clear();
        Logger::LogInfo("GameLoop: DIALECTIC Control found no living NPC for Wait Here");
    }

    // Re-read the live mode state on every open so the buttons advertise current values.
    XNVSEAdapter::NativeToolMenuStatus status;
    status.chatMode = ModeTitleNameFromIndex(std::clamp(Config::currentModeIndex, 0, 8));
    status.llmMode = ToUpperCopy(ProfileModelLabelFromSlot(Config::currentProfileModelSlot));

    if (XNVSEAdapter::OpenNativeToolMenu(
            XNVSEAdapter::NativeToolMenu::DialecticControl, nullptr, &status)) {
        Logger::LogInfo("GameLoop: Opened Dialectic Control through native UI adapter");
        return;
    }
    Logger::LogError("GameLoop: Failed to open Dialectic Control through native UI adapter");
}

void RequestControlMenuWaitHere() {
    const uint32_t actorFormId = g_dialecticControlTargetFormId;
    const std::string actorName = g_dialecticControlTargetName;
    g_dialecticControlTargetFormId = 0;
    g_dialecticControlTargetName.clear();

    if (actorFormId == 0) {
        Console::Print("[DIALECTIC] Look at an NPC or stand near one before choosing Wait Here");
        Logger::LogInfo("GameLoop: DIALECTIC Control Wait Here selected without an NPC target");
        return;
    }

    ActionManager::RequestWaitHere(actorFormId, actorName, "DialecticControl");
}

// Chat shortcuts deliberately omit the Control menu's nearest-NPC fallback.
static void RequestChatHotkeyWaitHere() {
    const NPCDetector::NPCInfo npc = NPCDetector::GetCrosshairNPC();
    if (!npc.isValid || npc.isDead || npc.formId == 0x00000014) {
        Console::Print("[DIALECTIC] Look at a living NPC to make them wait here");
        return;
    }
    ActionManager::RequestWaitHere(npc.formId, npc.name, "ChatHotkey");
}

void RequestModeMenuOpen() {
    const RuntimeSnapshot::GameState state = RuntimeSnapshot::GetGameState();
    if (IsToolMenuBlocked(state)) {
        Logger::LogInfo("GameLoop: Ignoring mode selector while a blocking menu is open");
        return;
    }
    const std::string modeMenuTitle = std::string("Mode: [") +
        ModeTitleNameFromIndex(std::clamp(Config::currentModeIndex, 0, 8)) + "]";
    if (XNVSEAdapter::OpenNativeToolMenu(XNVSEAdapter::NativeToolMenu::Mode, modeMenuTitle.c_str())) {
        Logger::LogInfo("GameLoop: Opened mode selector through native UI adapter");
        return;
    }
    Logger::LogError("GameLoop: Failed to open mode selector through native UI adapter");
}

void RequestTextInputMenuOpen() {
    static DWORD s_lastRequestTick = 0;
    const DWORD now = GetTickCount();
    const DWORD blockUntil = g_textInputMenuBlockUntilTick.load();
    if (IsTickBefore(now, blockUntil)) {
        Logger::LogInfo("GameLoop: Ignoring chatbox request during close debounce");
        return;
    }

    if (g_textInputMenuPending.load()) {
        const DWORD requestTick = g_textInputMenuRequestTick.load();
        if (requestTick != 0 && (now - requestTick) < 15000) {
            Logger::LogInfo("GameLoop: Ignoring chatbox request because text input is already pending/open");
            return;
        }

        Logger::LogWarning("GameLoop: Text input pending flag timed out; allowing new chatbox request");
        g_textInputMenuPending.store(false);
        g_textInputMenuRequestTick.store(0);
    }

    if ((now - s_lastRequestTick) < 250) {
        return;
    }
    s_lastRequestTick = now;

    uint32_t combatTargetFormId = g_conversationPartnerFormId;
    if (combatTargetFormId == 0) {
        const NPCDetector::NPCInfo crosshairNpc = NPCDetector::GetCrosshairNPC();
        if (crosshairNpc.isValid) {
            combatTargetFormId = crosshairNpc.formId;
        } else {
            combatTargetFormId = TargetManager::GetCurrentTarget().formId;
        }
    }
    if (!EnforceCombatDialogueGate(combatTargetFormId, "chatbox", true)) {
        return;
    }

    if (!g_conversationIsNarrator) {
        if (g_conversationActive &&
            IsConversationTargetOnCooldown(g_conversationPartnerFormId, g_conversationPartner, true)) {
            return;
        }

        const NPCDetector::NPCInfo npc = NPCDetector::GetCrosshairNPC();
        if (npc.isValid && IsConversationTargetOnCooldown(npc.formId, npc.name, true)) {
            return;
        }
    }

    PrepareTextInputTargetHint();
    WriteToolBridgeSignal(kOpenTextInputMenuPath, "text_input");
    g_textInputMenuPending.store(true);
    g_textInputMenuRequestTick.store(now);
    Logger::LogInfo("GameLoop: Requested text input chatbox");
}

void RequestLLMModelMenuOpen() {
    const RuntimeSnapshot::GameState state = RuntimeSnapshot::GetGameState();
    if (IsToolMenuBlocked(state)) {
        Logger::LogInfo("GameLoop: Ignoring LLM model selector while a blocking menu is open");
        return;
    }
    if (XNVSEAdapter::OpenNativeToolMenu(XNVSEAdapter::NativeToolMenu::LlmModel)) {
        Logger::LogInfo("GameLoop: Opened LLM model selector through native UI adapter");
        return;
    }
    Logger::LogError("GameLoop: Failed to open LLM model selector through native UI adapter");
}

void RequestDynamicProfileMenuOpen() {
    const RuntimeSnapshot::GameState state = RuntimeSnapshot::GetGameState();
    if (IsToolMenuBlocked(state)) {
        Logger::LogInfo("GameLoop: Ignoring dynamic profile selector while a blocking menu is open");
        return;
    }
    if (XNVSEAdapter::OpenNativeToolMenu(XNVSEAdapter::NativeToolMenu::DynamicProfile)) {
        Logger::LogInfo("GameLoop: Opened dynamic profile selector through native UI adapter");
        return;
    }
    Logger::LogError("GameLoop: Failed to open dynamic profile selector through native UI adapter");
}

static uint32_t ResolveResponseSpeakerFormId(const std::string& speaker) {
    const std::string playerName = Config::playerName.empty() ? "Player" : Config::playerName;
    if (!speaker.empty() && IsPlayerSpeakerName(speaker)) {
        const uint32_t playerFormId = Misc::GetPlayerFormId();
        if (playerFormId != 0) {
            Logger::LogInfo("GameLoop: Resolved response speaker [%s] as player form 0x%08X",
                speaker.c_str(),
                playerFormId);
            return playerFormId;
        }
        Logger::LogInfo("GameLoop: Could not resolve player form id for speaker [%s]", speaker.c_str());
        return 0;
    }

    if (!speaker.empty() && EqualsIgnoreCase(speaker, kNarratorName)) {
        return 0;
    }

    if (!speaker.empty() && EqualsIgnoreCase(speaker, g_conversationPartner)) {
        return g_conversationPartnerFormId;
    }

    uint32_t agentFormId = AgentManager::FindAgentFormIdByName(speaker);
    if (agentFormId != 0) {
        return agentFormId;
    }

    const auto& target = TargetManager::GetCurrentTarget();
    if (target.formId != 0 && EqualsIgnoreCase(target.name, speaker)) {
        return target.formId;
    }

    const std::vector<ActorPositionResolverFNV::PositionResult> positions =
        ActorPositionResolverFNV::GetRecentActorPositions();
    for (const auto& position : positions) {
        if (position.resolved &&
            position.formId != 0 &&
            EqualsIgnoreCase(position.actorName, speaker)) {
            Logger::LogInfo("GameLoop: Resolved response speaker [%s] from spatial cache as 0x%08X",
                speaker.c_str(),
                position.formId);
            return position.formId;
        }
    }

    return 0;
}

static std::string JoinCsv(const std::vector<std::string>& values) {
    std::ostringstream joined;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i > 0) {
            joined << ",";
        }
        joined << values[i];
    }
    return joined.str();
}

static void SendDynamicProfileBatchRequest(const std::vector<std::string>& npcNames) {
    std::vector<std::string> cleaned;
    std::set<std::string> seen;
    cleaned.reserve(npcNames.size());

    for (const auto& rawName : npcNames) {
        const std::string name = TrimInput(rawName);
        if (name.empty() || name == "<no name>") {
            continue;
        }

        std::string key = name;
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (seen.insert(key).second) {
            cleaned.push_back(name);
        }
    }

    if (cleaned.empty()) {
        Console::Print("[DIALECTIC] No AI NPCs found for dynamic profile update");
        Logger::LogInfo("GameLoop: Dynamic profile request skipped; empty NPC list");
        return;
    }

    const std::string npcList = JoinCsv(cleaned);
    Console::Print("[DIALECTIC] Updating %zu dynamic profile%s",
        cleaned.size(),
        cleaned.size() == 1 ? "" : "s");
    Logger::LogInfo("GameLoop: Sending updateprofiles_batch_async for [%s]", npcList.c_str());
    std::ostringstream payload;
    payload << "{"
            << "\"schema\":\"dialectic.dynamic_profile_batch.v1\","
            << "\"npcs\":[";
    for (size_t i = 0; i < cleaned.size(); ++i) {
        if (i > 0) {
            payload << ",";
        }
        payload << "\"" << HTTPManager::EscapeJson(cleaned[i]) << "\"";
    }
    payload << "]"
            << "}";
    HTTPManager::SendEvent("updateprofiles_batch_async", payload.str());
}

static void TriggerDynamicProfileForCurrentTarget() {
    const auto& target = TargetManager::GetCurrentTarget();
    if (!target.isActor || target.name.empty() || target.name == "<no name>") {
        Console::Print("[DIALECTIC] Target an AI NPC to update their dynamic profile");
        Logger::LogInfo("GameLoop: UpdateTargetProfile skipped; invalid target");
        return;
    }

    if (!AgentManager::IsAIAgent(target.formId)) {
        Console::Print("[DIALECTIC] %s is not an active DIALECTIC NPC", target.name.c_str());
        Logger::LogInfo("GameLoop: UpdateTargetProfile skipped; %s 0x%08X is not an AI agent",
            target.name.c_str(),
            target.formId);
        return;
    }

    SendDynamicProfileBatchRequest({ target.name });
}

static void TriggerDynamicProfilesForNearbyAgents() {
    const auto player = ActorPositionResolverFNV::ResolvePlayer();
    if (!player.resolved) {
        Console::Print("[DIALECTIC] Could not resolve player position for nearby profile update");
        Logger::LogInfo("GameLoop: UpdateNearbyProfiles skipped; player position unresolved");
        return;
    }

    struct Candidate {
        std::string name;
        float distance = 0.0f;
    };

    std::vector<Candidate> candidates;
    const auto positions = ActorPositionResolverFNV::GetRecentActorPositions();
    const float maxDistance = Config::nearbyActorsMaxDistance > 0.0f
        ? Config::nearbyActorsMaxDistance
        : std::max(Config::distanceActivatingNpcInterior, Config::distanceActivatingNpcExterior);

    for (const auto& position : positions) {
        if (!position.resolved ||
            position.formId == 0 ||
            position.actorName.empty() ||
            position.actorName == "<no name>" ||
            !AgentManager::IsAIAgent(position.formId)) {
            continue;
        }

        const auto spatial = SpatialAwarenessFNV::Evaluate(player, position);
        if (!std::isfinite(spatial.airDistance) || spatial.airDistance > maxDistance) {
            continue;
        }

        candidates.push_back({ position.actorName, spatial.airDistance });
    }

    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        return a.distance < b.distance;
    });

    std::vector<std::string> npcNames;
    constexpr size_t kMaxDynamicProfileBatch = 12;
    for (const auto& candidate : candidates) {
        if (npcNames.size() >= kMaxDynamicProfileBatch) {
            break;
        }
        npcNames.push_back(candidate.name);
    }

    if (npcNames.empty()) {
        const auto registered = AgentManager::GetRegisteredAgentSnapshot();
        for (const auto& [formId, name] : registered) {
            if (formId != 0 && !name.empty()) {
                npcNames.push_back(name);
            }
            if (npcNames.size() >= kMaxDynamicProfileBatch) {
                break;
            }
        }
    }

    SendDynamicProfileBatchRequest(npcNames);
}

static void TriggerDynamicProfileForNarrator() {
    Console::Print("[DIALECTIC] Updating The Narrator dynamic profile");
    Logger::LogInfo("GameLoop: Sending updateprofile_narrator");
    HTTPManager::SendEvent("updateprofile_narrator", "{\"schema\":\"dialectic.dynamic_profile.v1\",\"npc\":\"The Narrator\"}");
}

static void UpdateDynamicProfileTimer() {
    if (Config::dynamicProfileTimerMinutes <= 0) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (g_lastDynamicProfileTimerUpdate.time_since_epoch().count() == 0) {
        g_lastDynamicProfileTimerUpdate = now;
        Logger::LogInfo("GameLoop: Dynamic profile timer armed for %d minute(s)",
            Config::dynamicProfileTimerMinutes);
        return;
    }

    std::string blockReason;
    if (!g_gameState.isInGame) {
        blockReason = "not in game";
    } else if (g_gameState.isLoading) {
        blockReason = "loading";
    } else if (g_gameState.isPaused) {
        blockReason = "paused";
    } else if (g_gameState.isInMenu || g_gameState.isInDialogue) {
        blockReason = "menu open";
    }

    if (!blockReason.empty()) {
        if (g_dynamicProfileBlockedAt.time_since_epoch().count() == 0) {
            g_dynamicProfileBlockedAt = now;
            Logger::LogDebug("GameLoop: Dynamic profile timer suspended (%s)", blockReason.c_str());
        }
        return;
    }

    if (g_dynamicProfileBlockedAt.time_since_epoch().count() != 0) {
        const auto blockedDuration = now - g_dynamicProfileBlockedAt;
        g_lastDynamicProfileTimerUpdate += blockedDuration;
        Logger::LogDebug("GameLoop: Dynamic profile timer resumed after %lld ms",
            static_cast<long long>(
                std::chrono::duration_cast<std::chrono::milliseconds>(blockedDuration).count()));
        g_dynamicProfileBlockedAt = {};
    }

    if (g_dynamicProfileResumeNotBefore.time_since_epoch().count() != 0) {
        if (now < g_dynamicProfileResumeNotBefore) {
            return;
        }
        g_dynamicProfileResumeNotBefore = {};
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::minutes>(
        now - g_lastDynamicProfileTimerUpdate);
    if (elapsed.count() < Config::dynamicProfileTimerMinutes) {
        return;
    }

    g_lastDynamicProfileTimerUpdate = now;
    Logger::LogInfo("GameLoop: Dynamic profile timer fired after %lld minute(s)",
        static_cast<long long>(elapsed.count()));
    TriggerDynamicProfilesForNearbyAgents();
    if (Config::dynamicProfileTimerIncludeNarrator) {
        TriggerDynamicProfileForNarrator();
    }
}

static void BeginDynamicProfileTimerBlock(const char* reason) {
    if (g_dynamicProfileBlockedAt.time_since_epoch().count() == 0) {
        g_dynamicProfileBlockedAt = std::chrono::steady_clock::now();
        Logger::LogDebug("GameLoop: Dynamic profile timer suspended (%s)",
            reason && *reason ? reason : "runtime transition");
    }
}

static void DelayDynamicProfileTimerAfterLoad(const char* reason) {
    constexpr auto kLoadDelay = std::chrono::seconds(30);
    constexpr auto kLoadDelayCooldown = std::chrono::seconds(60);
    const auto now = std::chrono::steady_clock::now();

    if (g_lastDynamicProfileLoadDelayAt.time_since_epoch().count() != 0 &&
        now - g_lastDynamicProfileLoadDelayAt < kLoadDelayCooldown) {
        Logger::LogInfo("GameLoop: Dynamic profile load delay skipped; cooldown active (%s)",
            reason && *reason ? reason : "load");
        return;
    }

    if (g_lastDynamicProfileTimerUpdate.time_since_epoch().count() == 0) {
        g_lastDynamicProfileTimerUpdate = now;
    }
    g_lastDynamicProfileTimerUpdate += kLoadDelay;
    g_dynamicProfileResumeNotBefore = now + kLoadDelay;
    g_lastDynamicProfileLoadDelayAt = now;
    Logger::LogInfo("GameLoop: Added 30-second dynamic profile delay after %s",
        reason && *reason ? reason : "load");
}

static bool IsBoredEventBlocked(std::string& reason) {
    if (!g_gameState.isInGame) {
        reason = "not in game";
        return true;
    }
    if (g_gameState.isLoading) {
        reason = "loading";
        return true;
    }
    if (g_gameState.isPaused) {
        reason = "paused";
        return true;
    }
    if (Config::boredAvoidInMenu && IsTextInputMenuActiveOrRecentlyClosed()) {
        reason = "text input active";
        return true;
    }
    if (Config::boredAvoidInMenu && g_gameState.isInMenu) {
        reason = "menu open";
        return true;
    }
    if (Config::boredAvoidInDialogue && g_gameState.isInDialogue) {
        reason = "vanilla dialogue active";
        return true;
    }
    if (Config::boredAvoidInCombat && g_gameState.isInCombat) {
        reason = "combat active";
        return true;
    }
    if (Config::boredAvoidWhenSneaking && ActorPositionResolverFNV::IsPlayerSneaking()) {
        reason = "player sneaking";
        return true;
    }
    if (Config::boredAvoidWhenVoiceInputActive && (g_voiceInputActive.load() || VoiceRecorder::IsRecording())) {
        reason = "voice input active";
        return true;
    }

    const SpeakManager::QueueStatus speech = SpeakManager::GetQueueStatus();
    const HTTPManager::QueueStatus http = HTTPManager::GetQueueStatus();
    if (speech.isProcessing ||
        speech.isPlaying ||
        speech.currentPlaybackLineActive ||
        speech.dialogueLinesQueued > 0 ||
        speech.ttsDownloadsInProgress > 0 ||
        speech.ttsTasksPending > 0 ||
        speech.ttsTasksActive > 0 ||
        speech.preparedAudioCount > 0 ||
        http.streamInProgress ||
        http.pendingHttpTasks > 0 ||
        http.activeHttpTasks > 0 ||
        http.httpResponsesQueued > 0) {
        std::ostringstream details;
        details << "dialogue queue active"
                << " lines=" << speech.dialogueLinesQueued
                << " tts=" << speech.ttsDownloadsInProgress
                << " tts_tasks=" << speech.ttsTasksActive << "/" << speech.ttsTasksPending
                << " prepared=" << speech.preparedAudioCount
                << " playing=" << (speech.isPlaying ? 1 : 0)
                << " http_stream=" << (http.streamInProgress ? 1 : 0)
                << " http_tasks=" << http.activeHttpTasks << "/" << http.pendingHttpTasks
                << " http_queued=" << http.httpResponsesQueued;
        reason = details.str();
        return true;
    }

    return false;
}

static std::string BuildBoredEventPayload(
    uint32_t actorFormId,
    const std::string& actorName,
    const std::vector<std::pair<uint32_t, std::string>>& eligibleActors) {
    const std::string playerName = Config::playerName.empty() ? "Player" : Config::playerName;
    const std::string location = Misc::GetPlayerLocation();
    std::ostringstream people;
    people << "|";
    std::set<std::string> seenNames;
    for (const auto& candidate : eligibleActors) {
        if (candidate.second.empty() || !seenNames.insert(candidate.second).second) {
            continue;
        }
        people << candidate.second << "|";
    }
    if (seenNames.insert(actorName).second) {
        people << actorName << "|";
    }
    people << playerName << "|";

    std::ostringstream payload;
    payload << "{";
    payload << "\"schema\":\"dialectic.bored_event.v1\",";
    payload << "\"npc\":\"" << HTTPManager::EscapeJson(actorName) << "\",";
    payload << "\"npc_id\":\"" << HTTPManager::EscapeJson(FormatFormIdJsonValue(actorFormId)) << "\",";
    payload << "\"speaker\":\"" << HTTPManager::EscapeJson(actorName) << "\",";
    payload << "\"speaker_formid\":\"" << HTTPManager::EscapeJson(FormatFormIdJsonValue(actorFormId)) << "\",";
    payload << "\"actor_name\":\"" << HTTPManager::EscapeJson(actorName) << "\",";
    payload << "\"actor_formid\":\"" << HTTPManager::EscapeJson(FormatFormIdJsonValue(actorFormId)) << "\",";
    payload << "\"player\":\"" << HTTPManager::EscapeJson(playerName) << "\",";
    payload << "\"listener\":\"" << HTTPManager::EscapeJson(playerName) << "\",";
    payload << "\"location\":\"" << HTTPManager::EscapeJson(location) << "\",";
    payload << "\"reason\":\"idle_bored\",";
    payload << "\"people\":\"" << HTTPManager::EscapeJson(people.str()) << "\",";
    payload << "\"eligible_actors\":[";
    for (size_t index = 0; index < eligibleActors.size(); ++index) {
        if (index > 0) {
            payload << ",";
        }
        payload << "{\"name\":\"" << HTTPManager::EscapeJson(eligibleActors[index].second) << "\",";
        payload << "\"refid\":\""
            << HTTPManager::EscapeJson(FormatFormIdJsonValue(eligibleActors[index].first)) << "\"}";
    }
    payload << "],";
    payload << "\"audience_snapshot\":{";
    payload << "\"people\":\"" << HTTPManager::EscapeJson(people.str()) << "\",";
    payload << "\"target_only\":false,";
    payload << "\"target_form_id\":\""
        << HTTPManager::EscapeJson(FormatFormIdJsonValue(actorFormId)) << "\"}";
    payload << "}";
    return payload.str();
}

static float DistanceBetween(
    const ActorPositionResolverFNV::Vector3& a,
    const ActorPositionResolverFNV::Vector3& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

static std::vector<std::pair<uint32_t, std::string>> BuildFreshBoredCandidates(float maxDistance) {
    struct Candidate {
        uint32_t formID = 0;
        std::string name;
        float distance = 0.0f;
    };

    std::vector<Candidate> fresh;
    const auto player = ActorPositionResolverFNV::ResolvePlayer();
    if (!player.resolved) {
        Logger::LogDebug("GameLoop: Bored candidate scan skipped; player position unresolved (%s)",
            player.reason.c_str());
        return {};
    }

    const auto positions = ActorPositionResolverFNV::GetRecentActorPositions();
    for (const auto& position : positions) {
        if (!position.resolved ||
            position.formId == 0 ||
            position.formId == 0x00000014 ||
            position.actorName.empty() ||
            position.actorName == "<no name>" ||
            !AgentManager::IsAIAgent(position.formId)) {
            continue;
        }

        const auto spatial = SpatialAwarenessFNV::Evaluate(player, position);
        if (!spatial.canCommunicate) {
            continue;
        }

        const float distance = spatial.airDistance > 0.0f
            ? spatial.airDistance
            : DistanceBetween(player.position, position.position);
        if (maxDistance > 0.0f && distance > maxDistance) {
            continue;
        }

        std::string activityReason;
        if (!ActivityStatusFNV::IsAutomaticDialogueAllowed(position.formId, &activityReason)) {
            Logger::LogDebug("GameLoop: Bored candidate %s (0x%08X) skipped: %s",
                position.actorName.c_str(), position.formId, activityReason.c_str());
            continue;
        }

        AgentManager::MarkAgentSeen(position.formId, distance);
        fresh.push_back({ position.formId, position.actorName, distance });
    }

    std::sort(fresh.begin(), fresh.end(), [](const Candidate& left, const Candidate& right) {
        return left.distance < right.distance;
    });

    std::vector<std::pair<uint32_t, std::string>> result;
    result.reserve(fresh.size());
    for (const auto& candidate : fresh) {
        result.emplace_back(candidate.formID, candidate.name);
    }
    return result;
}

static void ResetBoredEventTimer(const char* reason) {
    const auto now = std::chrono::steady_clock::now();
    g_lastBoredEventTimerUpdate = now;
    g_lastBoredBlockingActivityTime = now;
    if (reason && *reason) {
        Logger::LogDebug("GameLoop: Bored event timer reset (%s)", reason);
    }
}

static void UpdateBoredEventTimer() {
    if (!Config::boredEventsEnabled || Config::boredEventTimerSeconds <= 0) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (g_lastBoredEventTimerUpdate.time_since_epoch().count() == 0) {
        g_lastBoredEventTimerUpdate = now;
        g_lastBoredBlockingActivityTime = now;
        Logger::LogInfo("GameLoop: Bored event timer armed for %d second(s)",
            Config::boredEventTimerSeconds);
        return;
    }

    std::string blockReason;
    if (IsBoredEventBlocked(blockReason)) {
        g_lastBoredEventTimerUpdate = now;
        g_lastBoredBlockingActivityTime = now;
        return;
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        now - g_lastBoredEventTimerUpdate);
    if (elapsed.count() < Config::boredEventTimerSeconds) {
        return;
    }

    if (Config::boredRecentSpeechCooldownSeconds > 0 &&
        g_lastBoredBlockingActivityTime.time_since_epoch().count() != 0) {
        const auto idleElapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - g_lastBoredBlockingActivityTime);
        if (idleElapsed.count() < Config::boredRecentSpeechCooldownSeconds) {
            return;
        }
    }

    g_lastBoredEventTimerUpdate = now;

    uint32_t selectedFormId = 0;
    std::string selectedName;
    const float maxDistance = std::max(Config::distanceActivatingNpcInterior, Config::distanceActivatingNpcExterior);
    const auto freshCandidates = BuildFreshBoredCandidates(maxDistance);
    if (!AgentManager::SelectLeastBoredNearbyAgent(freshCandidates, selectedFormId, selectedName)) {
        Logger::LogInfo("GameLoop: Bored event skipped; no managed NPC within %.1f units", maxDistance);
        return;
    }

    RefreshPlayerNameFromGame();
    const std::string payload = BuildBoredEventPayload(selectedFormId, selectedName, freshCandidates);
    AgentManager::MarkBoredEventFired(selectedFormId);
    Logger::LogInfo("GameLoop: Sending bored event for %s (0x%08X)", selectedName.c_str(), selectedFormId);
    SpeakManager::StartRechatChainForAutonomousEvent();
    HTTPManager::SendEvent("bored", payload);
}

static std::string ExtractJsonStringValue(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    size_t keyPos = json.find(needle);
    if (keyPos == std::string::npos) return "";
    size_t colonPos = json.find(':', keyPos + needle.size());
    if (colonPos == std::string::npos) return "";
    size_t quotePos = json.find('"', colonPos + 1);
    if (quotePos == std::string::npos) return "";

    std::string value;
    bool escaped = false;
    for (size_t i = quotePos + 1; i < json.size(); ++i) {
        char c = json[i];
        if (escaped) {
            switch (c) {
                case '"': value.push_back('"'); break;
                case '\\': value.push_back('\\'); break;
                case 'n': value.push_back('\n'); break;
                case 'r': value.push_back('\r'); break;
                case 't': value.push_back('\t'); break;
                default: value.push_back(c); break;
            }
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        if (c == '"') break;
        value.push_back(c);
    }
    return value;
}

static bool ExtractJsonBoolValue(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    size_t keyPos = json.find(needle);
    if (keyPos == std::string::npos) return false;
    size_t colonPos = json.find(':', keyPos + needle.size());
    if (colonPos == std::string::npos) return false;
    size_t valueStart = json.find_first_not_of(" \t\r\n", colonPos + 1);
    return valueStart != std::string::npos && json.compare(valueStart, 4, "true") == 0;
}

static int ExtractJsonIntValue(const std::string& json, const std::string& key, int fallback = 0) {
    const std::string needle = "\"" + key + "\"";
    size_t keyPos = json.find(needle);
    if (keyPos == std::string::npos) return fallback;
    size_t colonPos = json.find(':', keyPos + needle.size());
    if (colonPos == std::string::npos) return fallback;
    size_t valueStart = json.find_first_of("-0123456789", colonPos + 1);
    if (valueStart == std::string::npos) return fallback;
    size_t valueEnd = json.find_first_not_of("-0123456789", valueStart);
    try {
        return std::stoi(json.substr(valueStart, valueEnd - valueStart));
    } catch (...) {
        return fallback;
    }
}

static void CaptureRuntimeStatusFromServer(const std::string& response) {
    const std::string mode = ExtractJsonStringValue(response, "dialectic_mode");
    const int modeIndex = !mode.empty()
        ? ModeIndexFromName(mode)
        : ExtractJsonIntValue(response, "mode_index", Config::currentModeIndex);
    const int modelSlot = std::clamp(
        ExtractJsonIntValue(response, "dialectic_profile_model",
            ExtractJsonIntValue(response, "active_model_slot", Config::currentProfileModelSlot)),
        1,
        4);

    {
        std::lock_guard<std::mutex> lock(g_runtimeStatusMutex);
        g_pendingRuntimeModeIndex = std::clamp(modeIndex, 0, 8);
        g_pendingRuntimeModelSlot = modelSlot;
        g_runtimeStatusPending = true;
    }
}

static void ApplyPendingRuntimeStatusFromServer() {
    int modeIndex = Config::currentModeIndex;
    int modelSlot = Config::currentProfileModelSlot;
    {
        std::lock_guard<std::mutex> lock(g_runtimeStatusMutex);
        if (!g_runtimeStatusPending) {
            return;
        }
        modeIndex = g_pendingRuntimeModeIndex;
        modelSlot = g_pendingRuntimeModelSlot;
        g_runtimeStatusPending = false;
    }

    const bool modeChanged = modeIndex != Config::currentModeIndex;
    const bool modelChanged = modelSlot != Config::currentProfileModelSlot;
    if (modeChanged) {
        Logger::LogInfo("GameLoop: Server runtime mode sync %s/%d -> %s/%d",
            Config::currentMode.c_str(),
            Config::currentModeIndex,
            ModeNameFromIndex(modeIndex),
            modeIndex);
        ApplyModeIndex(modeIndex, false);
    }

    if (modelChanged) {
        Logger::LogInfo("GameLoop: Server LLM model slot sync %d -> %d (%s)",
            Config::currentProfileModelSlot,
            modelSlot,
            ProfileModelLabelFromSlot(modelSlot));
        Config::currentProfileModelSlot = modelSlot;
    }
}

static void MaybeSyncRuntimeStateFromServer(bool force) {
    static std::atomic<bool> syncInFlight(false);
    static auto lastSync = std::chrono::steady_clock::time_point{};

    ApplyPendingRuntimeStatusFromServer();

    const auto now = std::chrono::steady_clock::now();
    if (!force &&
        lastSync.time_since_epoch().count() != 0 &&
        now - lastSync < std::chrono::seconds(3)) {
        return;
    }

    bool expected = false;
    if (!syncInFlight.compare_exchange_strong(expected, true)) {
        return;
    }
    lastSync = now;

    if (TaskManager::Enqueue("runtime_status", "server_runtime_status",
        RuntimeGeneration::Current(), false, std::chrono::seconds(30),
        [](const TaskManager::CancellationToken& token) {
        if (token.IsCancellationRequested()) {
            syncInFlight.store(false);
            return;
        }
        const std::string request = "{\"schema\":\"dialectic.runtime_status.request.v1\"}";
        const std::string response = HTTPManager::SendJson("processor/runtime_status.php", request);
        if (!token.IsCancellationRequested() && !response.empty()) {
            CaptureRuntimeStatusFromServer(response);
        } else {
            Logger::LogDebug("GameLoop: Runtime status sync returned empty response");
        }
        syncInFlight.store(false);
    }) == 0) {
        syncInFlight.store(false);
    }
}

static std::vector<std::string> ExtractJsonArrayObjects(const std::string& json, const std::string& key) {
    std::vector<std::string> objects;
    const std::string needle = "\"" + key + "\"";
    size_t keyPos = json.find(needle);
    if (keyPos == std::string::npos) return objects;
    size_t arrayStart = json.find('[', keyPos + needle.size());
    if (arrayStart == std::string::npos) return objects;

    bool inString = false;
    bool escaped = false;
    int depth = 0;
    size_t objectStart = std::string::npos;
    for (size_t i = arrayStart + 1; i < json.size(); ++i) {
        const char c = json[i];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (c == '\\' && inString) {
            escaped = true;
            continue;
        }
        if (c == '"') {
            inString = !inString;
            continue;
        }
        if (inString) continue;
        if (c == '{') {
            if (depth == 0) objectStart = i;
            depth++;
            continue;
        }
        if (c == '}') {
            depth--;
            if (depth == 0 && objectStart != std::string::npos) {
                objects.push_back(json.substr(objectStart, i - objectStart + 1));
                objectStart = std::string::npos;
            }
            continue;
        }
        if (c == ']' && depth == 0) {
            break;
        }
    }
    return objects;
}

static std::string ReadAndDeleteTextInputFile(const char* path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        return "";
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    input.close();
    std::remove(path);
    return buffer.str();
}

static std::string ReadFileIfExists(const char* path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        return "";
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

static void DeleteFileIfExists(const char* path) {
    std::remove(path);
}

static bool GetFileModifiedAgeMs(const char* path, uint64_t& ageMs) {
    WIN32_FILE_ATTRIBUTE_DATA attributes = {};
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &attributes)) {
        return false;
    }

    FILETIME currentFileTime = {};
    GetSystemTimeAsFileTime(&currentFileTime);

    ULARGE_INTEGER current = {};
    current.LowPart = currentFileTime.dwLowDateTime;
    current.HighPart = currentFileTime.dwHighDateTime;

    ULARGE_INTEGER modified = {};
    modified.LowPart = attributes.ftLastWriteTime.dwLowDateTime;
    modified.HighPart = attributes.ftLastWriteTime.dwHighDateTime;

    ageMs = current.QuadPart <= modified.QuadPart
        ? 0
        : (current.QuadPart - modified.QuadPart) / 10000;
    return true;
}

static std::unordered_map<std::string, std::string> ParseBridgeKeyValueData(const std::string& data) {
    std::unordered_map<std::string, std::string> values;
    std::string normalizedData = data;
    const std::string trimmedData = TrimInput(normalizedData);

    if (!trimmedData.empty() && trimmedData.front() == '{') {
        const char* stringKeys[] = {
            "source",
            "speaker",
            "speaker_refid",
            "target",
            "target_refid",
            "text",
            "tile_name",
            "topic_refid",
            "parent_topic_refid",
            "prompt_source",
            "capture_id",
            "location",
        };
        for (const char* key : stringKeys) {
            const std::string value = ExtractJsonStringValue(trimmedData, key);
            if (!value.empty()) {
                values[key] = value;
            }
        }

        const char* boolKeys[] = {
            "is_player_line",
            "menu_mode",
            "trade_menu_mode",
            "pipboy_open",
            "pause_menu_open",
            "dialogue_menu_open",
            "paused",
            "combat",
            "in_combat",
        };
        for (const char* key : boolKeys) {
            if (ExtractJsonBoolValue(trimmedData, key)) {
                values[key] = "1";
            }
        }

        const char* intKeys[] = {
            "menu_id",
            "tile_id",
            "end",
        };
        for (const char* key : intKeys) {
            const int value = ExtractJsonIntValue(trimmedData, key, INT_MIN);
            if (value != INT_MIN) {
                values[key] = std::to_string(value);
            }
        }

        return values;
    }

    size_t escapedNewlinePos = 0;
    while ((escapedNewlinePos = normalizedData.find("\\n", escapedNewlinePos)) != std::string::npos) {
        normalizedData.replace(escapedNewlinePos, 2, "\n");
        escapedNewlinePos += 1;
    }

    std::istringstream stream(normalizedData);
    std::string line;
    while (std::getline(stream, line)) {
        line = TrimInput(line);
        if (line.empty()) {
            continue;
        }
        const size_t equalsPos = line.find('=');
        if (equalsPos == std::string::npos) {
            continue;
        }
        const std::string key = ToLowerCopy(TrimInput(line.substr(0, equalsPos)));
        const std::string value = TrimInput(line.substr(equalsPos + 1));
        if (!key.empty()) {
            values[key] = value;
        }
    }
    return values;
}

static bool ParseBridgeFlag(const std::unordered_map<std::string, std::string>& values,
    const char* key,
    bool fallback = false) {
    auto it = values.find(key);
    if (it == values.end()) {
        return fallback;
    }

    const std::string value = ToLowerCopy(TrimInput(it->second));
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

static void CancelDialogueForCombatEntry() {
    Logger::LogInfo("GameLoop: Player entered combat - cancelling active AI dialogue");
    Console::Print("[DIALECTIC] Combat started - clearing AI dialogue");
    SpeakManager::CancelDialogueTurn("combat_entry", false, false);
    ResetBoredEventTimer("combat entry");
}

static void CancelDialogueForVanillaDialogueEntry() {
    Logger::LogInfo("GameLoop: Vanilla dialogue menu opened - cancelling active AI dialogue");
    ResetRuntimeForAIActions("vanilla_dialogue_menu_open", true, false, false);
}

static bool HasDialoguePlaybackWorkForMenuPause() {
    if (!Config::pauseDialogueOnMenu) {
        return false;
    }

    if (SpeakManager::IsSpeaking()) {
        return true;
    }

    const SpeakManager::QueueStatus status = SpeakManager::GetQueueStatus();
    return status.dialogueLinesQueued > 0
        || status.preparedAudioCount > 0
        || status.ttsDownloadsInProgress > 0
        || status.ttsTasksPending > 0
        || status.ttsTasksActive > 0
        || status.currentPlaybackLineActive;
}

static void RefreshGameState() {
    static bool s_hadState = false;
    static bool s_lastInMenu = false;
    static bool s_lastPaused = false;
    static bool s_lastInDialogue = false;
    static bool s_lastInCombat = false;

    RuntimeSnapshot::GameState nativeState;
    if (!RuntimeSnapshot::TryGetFreshGameState(nativeState, std::chrono::milliseconds(500))) {
        return;
    }

    const bool inMenu =
        nativeState.paused ||
        nativeState.pipboyOpen ||
        nativeState.pauseMenuOpen ||
        nativeState.dialogueMenuOpen ||
        nativeState.barterMenuOpen ||
        nativeState.containerMenuOpen ||
        nativeState.loadingMenuOpen;
    const bool paused = nativeState.paused;
    const bool inDialogue = nativeState.dialogueMenuOpen;
    const bool inCombat = nativeState.inCombat;

    if (!s_hadState || s_lastInMenu != inMenu || s_lastPaused != paused ||
        s_lastInDialogue != inDialogue ||
        s_lastInCombat != inCombat) {
        Log("GameLoop: Native runtime state inGame=%d inMenu=%d paused=%d dialogue=%d combat=%d loading=%d",
            nativeState.inGame ? 1 : 0,
            inMenu ? 1 : 0, paused ? 1 : 0, inDialogue ? 1 : 0,
            inCombat ? 1 : 0,
            nativeState.loadingMenuOpen ? 1 : 0);
        if (s_hadState && !s_lastInCombat && inCombat && Config::cancelDialogueOnCombat) {
            CancelDialogueForCombatEntry();
        }
        if (s_hadState && !s_lastInDialogue && inDialogue) {
            CancelDialogueForVanillaDialogueEntry();
        }
        s_hadState = true;
        s_lastInMenu = inMenu;
        s_lastPaused = paused;
        s_lastInDialogue = inDialogue;
        s_lastInCombat = inCombat;
    }

    g_gameState.isInMenu = inMenu;
    g_gameState.isPaused = paused;
    g_gameState.isInDialogue = inDialogue;
    g_gameState.isInCombat = inCombat;
    g_gameState.isInGame = nativeState.inGame;
    g_gameState.isLoading = nativeState.loadingMenuOpen;
}

enum class FreshTargetResult {
    NoTarget,
    Ready,
    Blocked
};

enum class ConversationStartResult {
    NoTarget,
    Started,
    Blocked
};

static std::string ConversationCooldownNameKey(const std::string& name) {
    return ToLowerCopy(TrimInput(name));
}

static void RegisterConversationCooldown(uint32_t formId, const std::string& name) {
    const int cooldownSeconds = std::max(0, Config::rechatEndConversationCooldown);
    if (cooldownSeconds <= 0) {
        return;
    }

    const std::string cleanName = TrimInput(name);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(cooldownSeconds);
    ConversationCooldownEntry entry{ cleanName, until };

    std::lock_guard<std::mutex> lock(g_conversationCooldownMutex);
    if (formId != 0) {
        g_conversationCooldownByFormId[formId] = entry;
    }

    const std::string nameKey = ConversationCooldownNameKey(cleanName);
    if (!nameKey.empty()) {
        g_conversationCooldownByName[nameKey] = entry;
    }

    Logger::LogInfo("GameLoop: EndConversation cooldown registered for %s (0x%08X) for %d seconds",
        cleanName.empty() ? "<unknown>" : cleanName.c_str(),
        formId,
        cooldownSeconds);
}

static bool IsConversationTargetOnCooldown(uint32_t formId, const std::string& name, bool notify) {
    const auto now = std::chrono::steady_clock::now();
    ConversationCooldownEntry entry;
    bool found = false;

    {
        std::lock_guard<std::mutex> lock(g_conversationCooldownMutex);
        if (formId != 0) {
            auto it = g_conversationCooldownByFormId.find(formId);
            if (it != g_conversationCooldownByFormId.end()) {
                if (now >= it->second.until) {
                    g_conversationCooldownByFormId.erase(it);
                } else {
                    entry = it->second;
                    found = true;
                }
            }
        }

        if (!found) {
            const std::string nameKey = ConversationCooldownNameKey(name);
            if (!nameKey.empty()) {
                auto it = g_conversationCooldownByName.find(nameKey);
                if (it != g_conversationCooldownByName.end()) {
                    if (now >= it->second.until) {
                        g_conversationCooldownByName.erase(it);
                    } else {
                        entry = it->second;
                        found = true;
                    }
                }
            }
        }
    }

    if (!found) {
        return false;
    }

    const std::string displayName = !entry.name.empty() ? entry.name : TrimInput(name);
    const auto remainingMs = std::chrono::duration_cast<std::chrono::milliseconds>(entry.until - now).count();
    Logger::LogInfo("GameLoop: Conversation start blocked; %s (0x%08X) is on EndConversation cooldown for %.1fs",
        displayName.empty() ? "NPC" : displayName.c_str(),
        formId,
        std::max<long long>(0, remainingMs) / 1000.0);
    if (notify) {
        Console::Print("[DIALECTIC] %s has finished talking with you for now.",
            displayName.empty() ? "This NPC" : displayName.c_str());
    }
    return true;
}

static FreshTargetResult TrySetCurrentTargetFromCrosshair(bool announceTarget) {
    (void)announceTarget;
    NPCDetector::NPCInfo npc = NPCDetector::GetCrosshairNPC();
    if (!npc.isValid) {
        return FreshTargetResult::NoTarget;
    }

    if (NPCDetector::IsExcluded(npc.formId, npc.name)) {
        Console::Print("[DIALECTIC] Cannot talk to %s (excluded)", npc.name.c_str());
        Logger::LogInfo("GameLoop: Crosshair NPC %s is excluded", npc.name.c_str());
        return FreshTargetResult::Blocked;
    }

    if (npc.isDead) {
        Console::Print("[DIALECTIC] Cannot talk to %s (dead)", npc.name.c_str());
        Logger::LogInfo("GameLoop: Crosshair NPC %s is dead", npc.name.c_str());
        return FreshTargetResult::Blocked;
    }

    if (!IsConversationTargetEligible(npc.formId, npc.name, true, true, npc.isCreature)) {
        return FreshTargetResult::Blocked;
    }

    if (IsConversationTargetOnCooldown(npc.formId, npc.name, true)) {
        return FreshTargetResult::Blocked;
    }

    TargetManager::SetCurrentTarget(npc.formId, npc.name, true);
    return FreshTargetResult::Ready;
}

static bool UpdateConversationPartnerFromCurrentTarget(const char* reason) {
    const auto& currentTarget = TargetManager::GetCurrentTarget();
    if (!currentTarget.isActor || !currentTarget.isAlive || currentTarget.formId == 0 || currentTarget.name.empty()) {
        return false;
    }

    if (NPCDetector::IsExcluded(currentTarget.formId, currentTarget.name)) {
        Console::Print("[DIALECTIC] Cannot talk to %s (excluded)", currentTarget.name.c_str());
        Logger::LogInfo("GameLoop: Current target %s is excluded", currentTarget.name.c_str());
        return false;
    }

    if (!IsConversationTargetEligible(currentTarget.formId, currentTarget.name, true)) {
        return false;
    }

    if (IsConversationTargetOnCooldown(currentTarget.formId, currentTarget.name, true)) {
        return false;
    }

    const bool changed = g_conversationPartnerFormId != currentTarget.formId ||
        !EqualsIgnoreCase(g_conversationPartner, currentTarget.name);

    g_conversationActive = true;
    g_conversationIsNarrator = false;
    g_conversationPartner = currentTarget.name;
    g_conversationPartnerFormId = currentTarget.formId;

    if (changed) {
        Logger::LogInfo("GameLoop: Retargeted conversation partner to %s (0x%08X) from %s",
            currentTarget.name.c_str(), currentTarget.formId, reason ? reason : "target refresh");
    }

    return true;
}

static ConversationStartResult TryStartConversationFromCurrentTarget() {
    Logger::LogInfo("GameLoop: Detecting target for conversation...");

    const FreshTargetResult crosshairResult = TrySetCurrentTargetFromCrosshair(true);
    if (crosshairResult == FreshTargetResult::Ready) {
        return StartConversation()
            ? ConversationStartResult::Started
            : ConversationStartResult::Blocked;
    }
    if (crosshairResult == FreshTargetResult::Blocked) {
        return ConversationStartResult::Blocked;
    }

    const auto& currentTarget = TargetManager::GetCurrentTarget();
    if (currentTarget.formId != 0 && !currentTarget.name.empty() &&
        (!currentTarget.isActor || !currentTarget.isAlive)) {
        Logger::LogInfo("GameLoop: Remembered target %s (0x%08X) has stale actor state; continuing target fallback",
            currentTarget.name.c_str(), currentTarget.formId);
    } else if (currentTarget.formId != 0 && !currentTarget.name.empty()) {
        if (NPCDetector::IsExcluded(currentTarget.formId, currentTarget.name)) {
            Console::Print("[DIALECTIC] Cannot talk to %s (excluded)", currentTarget.name.c_str());
            Logger::LogInfo("GameLoop: TargetManager NPC %s is excluded, cannot start conversation",
                currentTarget.name.c_str());
            return ConversationStartResult::Blocked;
        }

        if (!IsConversationTargetEligible(currentTarget.formId, currentTarget.name, true)) {
            return ConversationStartResult::Blocked;
        }

        if (IsConversationTargetOnCooldown(currentTarget.formId, currentTarget.name, true)) {
            return ConversationStartResult::Blocked;
        }

        return StartConversation()
            ? ConversationStartResult::Started
            : ConversationStartResult::Blocked;
    }

    NPCDetector::NPCInfo npc = NPCDetector::GetCrosshairNPC();
    Logger::LogInfo("GameLoop: No crosshair target, finding closest NPC...");
    const float targetRadius = GetConversationTargetRadius();
    npc = NPCDetector::GetClosestNPC(targetRadius);

    if (!npc.isValid) {
        Logger::LogInfo("GameLoop: No NPCDetector target, finding closest spatial actor...");
        const auto player = ActorPositionResolverFNV::ResolvePlayer();
        const auto positions = ActorPositionResolverFNV::GetRecentActorPositions();
        float bestDistanceSq = targetRadius * targetRadius;
        ActorPositionResolverFNV::PositionResult bestPosition;

        if (player.resolved) {
            for (const auto& position : positions) {
                if (!position.resolved || position.formId == 0 || position.formId == 0x00000014 ||
                    position.actorName.empty() || position.actorName == "<no name>") {
                    continue;
                }

                if (!IsConversationTargetEligible(position.formId, position.actorName, false)) {
                    continue;
                }

                const auto spatial = SpatialAwarenessFNV::Evaluate(player, position);
                if (!spatial.canCommunicate) {
                    continue;
                }

                const float distanceSq = spatial.airDistance * spatial.airDistance;
                if (distanceSq < bestDistanceSq) {
                    bestDistanceSq = distanceSq;
                    bestPosition = position;
                }
            }
        }

        if (!bestPosition.resolved) {
            Console::Print("[DIALECTIC] No NPC found nearby");
            Logger::LogInfo("GameLoop: No valid NPC found for conversation");
            return ConversationStartResult::NoTarget;
        }

        if (IsConversationTargetOnCooldown(bestPosition.formId, bestPosition.actorName, true)) {
            return ConversationStartResult::Blocked;
        }

        TargetManager::SetCurrentTarget(bestPosition.formId, bestPosition.actorName, true);
        return StartConversation()
            ? ConversationStartResult::Started
            : ConversationStartResult::Blocked;
    }

    if (NPCDetector::IsExcluded(npc.formId, npc.name)) {
        Console::Print("[DIALECTIC] Cannot talk to %s (excluded)", npc.name.c_str());
        Logger::LogInfo("GameLoop: NPC %s is excluded, cannot start conversation", npc.name.c_str());
        return ConversationStartResult::Blocked;
    }

    if (npc.isDead) {
        Console::Print("[DIALECTIC] Cannot talk to %s (dead)", npc.name.c_str());
        Logger::LogInfo("GameLoop: NPC %s is dead, cannot start conversation", npc.name.c_str());
        return ConversationStartResult::Blocked;
    }

    if (!IsConversationTargetEligible(npc.formId, npc.name, true, true, npc.isCreature)) {
        return ConversationStartResult::Blocked;
    }

    if (IsConversationTargetOnCooldown(npc.formId, npc.name, true)) {
        return ConversationStartResult::Blocked;
    }

    TargetManager::SetCurrentTarget(npc.formId, npc.name, true);
    return StartConversation()
        ? ConversationStartResult::Started
        : ConversationStartResult::Blocked;
}

static void ClearPreparedTextInputTarget() {
    g_preparedTextInputTarget = {};
}

static bool WriteTextInputTargetHint(uint32_t formId, const std::string& targetName) {
    if (formId == 0 || targetName.empty()) {
        ClearPreparedTextInputTarget();
        ClearToolBridgeFile(kTextInputTargetPath);
        return false;
    }

    std::ofstream out(kTextInputTargetPath, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        Logger::LogWarning("GameLoop: Failed to write text input target hint");
        ClearPreparedTextInputTarget();
        return false;
    }

    out << targetName << "\n";
    g_preparedTextInputTarget.formId = formId;
    g_preparedTextInputTarget.name = targetName;
    g_preparedTextInputTarget.runtimeGeneration = RuntimeGeneration::Current();
    Logger::LogInfo("[TEXT_INPUT_TARGET] retained form=0x%08X name=%s generation=%llu",
        formId,
        targetName.c_str(),
        static_cast<unsigned long long>(g_preparedTextInputTarget.runtimeGeneration));
    return true;
}

static bool PrepareTargetFromNpcInfo(const NPCDetector::NPCInfo& npc, const char* source) {
    if (!npc.isValid || npc.formId == 0 || npc.name.empty()) {
        return false;
    }
    if (NPCDetector::IsExcluded(npc.formId, npc.name) || npc.isDead) {
        Logger::LogInfo("GameLoop: Skipping %s chatbox target %s (excluded=%d dead=%d)",
            source ? source : "unknown",
            npc.name.c_str(),
            NPCDetector::IsExcluded(npc.formId, npc.name) ? 1 : 0,
            npc.isDead ? 1 : 0);
        return false;
    }
    if (!IsConversationTargetEligible(npc.formId, npc.name, false, true, npc.isCreature)) {
        Logger::LogInfo("GameLoop: Skipping %s chatbox target %s (creature policy)",
            source ? source : "unknown", npc.name.c_str());
        return false;
    }
    if (npc.distance > GetConversationTargetRadius()) {
        Logger::LogInfo("GameLoop: Skipping %s chatbox target %s outside mode radius distance=%.1f radius=%.1f",
            source ? source : "unknown", npc.name.c_str(), npc.distance, GetConversationTargetRadius());
        return false;
    }
    if (IsConversationTargetOnCooldown(npc.formId, npc.name, true)) {
        return false;
    }

    TargetManager::SetCurrentTarget(npc.formId, npc.name, true);
    WriteTextInputTargetHint(npc.formId, npc.name);
    Logger::LogInfo("GameLoop: Prepared %s chatbox target %s (0x%08X)",
        source ? source : "unknown",
        npc.name.c_str(),
        npc.formId);
    return true;
}

static bool PrepareNearestSpatialTextInputTarget(float maxDistance) {
    const auto player = ActorPositionResolverFNV::ResolvePlayer();
    if (!player.resolved) {
        return false;
    }

    const auto positions = ActorPositionResolverFNV::GetRecentActorPositions();
    float bestDistanceSq = maxDistance * maxDistance;
    ActorPositionResolverFNV::PositionResult bestPosition;

    for (const auto& position : positions) {
        if (!position.resolved ||
            position.formId == 0 ||
            position.formId == 0x00000014 ||
            position.actorName.empty() ||
            position.actorName == "<no name>") {
            continue;
        }
        if (NPCDetector::IsExcluded(position.formId, position.actorName)) {
            continue;
        }
        if (!IsConversationTargetEligible(position.formId, position.actorName, false)) {
            continue;
        }
        if (IsConversationTargetOnCooldown(position.formId, position.actorName, false)) {
            continue;
        }

        const auto spatial = SpatialAwarenessFNV::Evaluate(player, position);
        if (!spatial.canCommunicate) {
            continue;
        }

        const float distanceSq = spatial.airDistance * spatial.airDistance;
        if (distanceSq < bestDistanceSq) {
            bestDistanceSq = distanceSq;
            bestPosition = position;
        }
    }

    if (!bestPosition.resolved) {
        return false;
    }

    TargetManager::SetCurrentTarget(bestPosition.formId, bestPosition.actorName, true);
    WriteTextInputTargetHint(bestPosition.formId, bestPosition.actorName);
    Logger::LogInfo("GameLoop: Prepared nearest spatial chatbox target %s (0x%08X)",
        bestPosition.actorName.c_str(),
        bestPosition.formId);
    return true;
}

static void PrepareTextInputTargetHint() {
    ClearPreparedTextInputTarget();
    ClearToolBridgeFile(kTextInputTargetPath);

    const float targetRadius = GetConversationTargetRadius();
    const NPCDetector::NPCInfo crosshairTarget = NPCDetector::GetCrosshairNPC();
    const NPCDetector::NPCInfo closestTarget = NPCDetector::GetClosestNPC(targetRadius);
    const auto currentTarget = TargetManager::GetCurrentTarget();
    Logger::LogInfo(
        "[TEXT_INPUT_TARGET] open generation=%llu radius=%.1f "
        "crosshair(valid=%d form=0x%08X name=%s dead=%d creature=%d distance=%.1f) "
        "closest(valid=%d form=0x%08X name=%s dead=%d creature=%d distance=%.1f) "
        "current(form=0x%08X name=%s actor=%d alive=%d) "
        "conversation(active=%d narrator=%d form=0x%08X name=%s)",
        static_cast<unsigned long long>(RuntimeGeneration::Current()),
        targetRadius,
        crosshairTarget.isValid ? 1 : 0,
        crosshairTarget.formId,
        crosshairTarget.name.c_str(),
        crosshairTarget.isDead ? 1 : 0,
        crosshairTarget.isCreature ? 1 : 0,
        crosshairTarget.distance,
        closestTarget.isValid ? 1 : 0,
        closestTarget.formId,
        closestTarget.name.c_str(),
        closestTarget.isDead ? 1 : 0,
        closestTarget.isCreature ? 1 : 0,
        closestTarget.distance,
        currentTarget.formId,
        currentTarget.name.c_str(),
        currentTarget.isActor ? 1 : 0,
        currentTarget.isAlive ? 1 : 0,
        g_conversationActive ? 1 : 0,
        g_conversationIsNarrator ? 1 : 0,
        g_conversationPartnerFormId.load(),
        g_conversationPartner.c_str());

    if (PrepareTargetFromNpcInfo(crosshairTarget, "crosshair")) {
        return;
    }

    if (PrepareTargetFromNpcInfo(closestTarget, "nearest npc")) {
        return;
    }

    if (currentTarget.formId != 0 &&
        !currentTarget.name.empty() &&
        !NPCDetector::IsExcluded(currentTarget.formId, currentTarget.name) &&
        IsConversationTargetEligible(currentTarget.formId, currentTarget.name, false) &&
        !IsConversationTargetOnCooldown(currentTarget.formId, currentTarget.name, false)) {
        WriteTextInputTargetHint(currentTarget.formId, currentTarget.name);
        Logger::LogInfo("GameLoop: Prepared cached chatbox target %s (0x%08X)",
            currentTarget.name.c_str(),
            currentTarget.formId);
        return;
    }

    if (PrepareNearestSpatialTextInputTarget(targetRadius)) {
        return;
    }

    if (g_conversationActive && !g_conversationIsNarrator &&
        g_conversationPartnerFormId != 0 && !g_conversationPartner.empty() &&
        IsConversationTargetEligible(g_conversationPartnerFormId, g_conversationPartner, false)) {
        WriteTextInputTargetHint(g_conversationPartnerFormId, g_conversationPartner);
        Logger::LogInfo("GameLoop: Prepared active conversation chatbox target %s (0x%08X)",
            g_conversationPartner.c_str(),
            g_conversationPartnerFormId.load());
        return;
    }

    Logger::LogInfo("GameLoop: No chatbox target hint available; submit path will resolve again");
}

static void ProcessTextInputBridge() {
    const std::string status = ReadAndDeleteTextInputFile(kTextInputStatusPath);
    bool bridgeClosed = false;
    bool bridgeSubmitted = false;
    bool bridgeBlocked = false;
    if (!status.empty()) {
        const auto statusValues = ParseBridgeKeyValueData(status);
        bridgeClosed = ParseBridgeFlag(statusValues, "closed", false);
        bridgeSubmitted = ParseBridgeFlag(statusValues, "submitted", false);
        bridgeBlocked = ParseBridgeFlag(statusValues, "blocked", false);
        if (bridgeClosed) {
            MarkTextInputMenuClosed(bridgeSubmitted ? "submitted" : "closed");
        } else if (bridgeBlocked) {
            MarkTextInputMenuClosed("blocked");
        }
    }

    std::string message = TrimInput(ReadAndDeleteTextInputFile("Data\\NVSE\\Plugins\\dialectic_textinput.tmp"));

    if (message.empty()) {
        // entered/opening status files arrive while the menu is active; only terminal states may discard the target.
        if (bridgeClosed || bridgeBlocked) {
            Logger::LogInfo(
                "[TEXT_INPUT_TARGET] bridge completed without message; clearing retained form=0x%08X name=%s generation=%llu",
                g_preparedTextInputTarget.formId,
                g_preparedTextInputTarget.name.c_str(),
                static_cast<unsigned long long>(g_preparedTextInputTarget.runtimeGeneration));
            ClearPreparedTextInputTarget();
            Logger::LogInfo("GameLoop: Text input closed without a message; no request will be sent");
        } else if (!status.empty()) {
            Logger::LogInfo(
                "[TEXT_INPUT_TARGET] bridge status received while menu remains open; retaining form=0x%08X name=%s generation=%llu",
                g_preparedTextInputTarget.formId,
                g_preparedTextInputTarget.name.c_str(),
                static_cast<unsigned long long>(g_preparedTextInputTarget.runtimeGeneration));
        }
        return;
    }

    const PreparedTextInputTarget preparedTarget = g_preparedTextInputTarget;
    ClearPreparedTextInputTarget();
    MarkTextInputMenuClosed("message received");
    Logger::LogInfo("GameLoop: Received typed message from script bridge: %s", message.c_str());
    const std::uint64_t currentGeneration = RuntimeGeneration::Current();
    const bool hasPreparedTarget = preparedTarget.formId != 0 && !preparedTarget.name.empty();
    const bool preparedGenerationCurrent = hasPreparedTarget &&
        RuntimeGeneration::IsCurrent(preparedTarget.runtimeGeneration);
    const auto submitCurrentTarget = TargetManager::GetCurrentTarget();
    const NPCDetector::NPCInfo submitCrosshairTarget = NPCDetector::GetCrosshairNPC();
    Logger::LogInfo(
        "[TEXT_INPUT_TARGET] submit prepared(form=0x%08X name=%s generation=%llu current_generation=%llu valid_generation=%d) "
        "current(form=0x%08X name=%s actor=%d alive=%d) "
        "crosshair(valid=%d form=0x%08X name=%s dead=%d distance=%.1f) "
        "conversation(active=%d narrator=%d form=0x%08X name=%s)",
        preparedTarget.formId,
        preparedTarget.name.c_str(),
        static_cast<unsigned long long>(preparedTarget.runtimeGeneration),
        static_cast<unsigned long long>(currentGeneration),
        preparedGenerationCurrent ? 1 : 0,
        submitCurrentTarget.formId,
        submitCurrentTarget.name.c_str(),
        submitCurrentTarget.isActor ? 1 : 0,
        submitCurrentTarget.isAlive ? 1 : 0,
        submitCrosshairTarget.isValid ? 1 : 0,
        submitCrosshairTarget.formId,
        submitCrosshairTarget.name.c_str(),
        submitCrosshairTarget.isDead ? 1 : 0,
        submitCrosshairTarget.distance,
        g_conversationActive ? 1 : 0,
        g_conversationIsNarrator ? 1 : 0,
        g_conversationPartnerFormId.load(),
        g_conversationPartner.c_str());
    if (hasPreparedTarget && !preparedGenerationCurrent) {
        Logger::LogWarning(
            "[TEXT_INPUT_TARGET] discarded retained target because runtime generation changed form=0x%08X name=%s prepared_generation=%llu current_generation=%llu reason=%s",
            preparedTarget.formId,
            preparedTarget.name.c_str(),
            static_cast<unsigned long long>(preparedTarget.runtimeGeneration),
            static_cast<unsigned long long>(currentGeneration),
            RuntimeGeneration::LastReason());
    }
    if (ShouldRouteToNarrator(message, true)) {
        Logger::LogInfo("[TEXT_INPUT_TARGET] routing submitted text to narrator");
        if (!g_conversationActive || !g_conversationIsNarrator) {
            StartNarratorConversation("typed input");
        }
        SendPlayerMessage(message);
        return;
    }

    ClearConversationIfPartnerLeftScene("typed input");

    if (!g_conversationActive) {
        if (preparedGenerationCurrent) {
            Logger::LogInfo("GameLoop: Starting typed conversation with prepared chatbox target %s (0x%08X)",
                preparedTarget.name.c_str(), preparedTarget.formId);
            TargetManager::SetCurrentTarget(preparedTarget.formId, preparedTarget.name, true);
            if (!StartConversation()) {
                const auto failedTarget = TargetManager::GetCurrentTarget();
                Logger::LogWarning(
                    "[TEXT_INPUT_TARGET] prepared conversation start failed retained(form=0x%08X name=%s) current(form=0x%08X name=%s actor=%d alive=%d)",
                    preparedTarget.formId,
                    preparedTarget.name.c_str(),
                    failedTarget.formId,
                    failedTarget.name.c_str(),
                    failedTarget.isActor ? 1 : 0,
                    failedTarget.isAlive ? 1 : 0);
                return;
            }
        } else {
            Logger::LogInfo("GameLoop: No active conversation for typed message, trying current target");
            const ConversationStartResult startResult = TryStartConversationFromCurrentTarget();
            if (startResult != ConversationStartResult::Started) {
                if (startResult == ConversationStartResult::NoTarget) {
                    Console::Print("[DIALECTIC] Target an NPC before sending text");
                }
                return;
            }
        }
    } else if (!g_conversationIsNarrator) {
        if (IsConversationTargetOnCooldown(g_conversationPartnerFormId, g_conversationPartner, true)) {
            g_conversationActive = false;
            g_conversationIsNarrator = false;
            g_conversationPartner.clear();
            g_conversationPartnerFormId = 0;
            return;
        }
        if (preparedGenerationCurrent) {
            TargetManager::SetCurrentTarget(preparedTarget.formId, preparedTarget.name, true);
            if (!UpdateConversationPartnerFromCurrentTarget("typed input prepared chatbox target")) {
                const auto failedTarget = TargetManager::GetCurrentTarget();
                Logger::LogWarning(
                    "[TEXT_INPUT_TARGET] prepared conversation retarget failed retained(form=0x%08X name=%s) current(form=0x%08X name=%s actor=%d alive=%d)",
                    preparedTarget.formId,
                    preparedTarget.name.c_str(),
                    failedTarget.formId,
                    failedTarget.name.c_str(),
                    failedTarget.isActor ? 1 : 0,
                    failedTarget.isAlive ? 1 : 0);
                return;
            }
            SendPlayerMessage(message);
            return;
        }
        const FreshTargetResult crosshairResult = TrySetCurrentTargetFromCrosshair(false);
        if (crosshairResult == FreshTargetResult::Blocked) {
            return;
        }
        if (crosshairResult == FreshTargetResult::Ready) {
            UpdateConversationPartnerFromCurrentTarget("typed input crosshair");
        } else {
            const auto& currentTarget = TargetManager::GetCurrentTarget();
            const bool currentDiffers = currentTarget.formId != 0 &&
                (currentTarget.formId != g_conversationPartnerFormId ||
                 !EqualsIgnoreCase(currentTarget.name, g_conversationPartner));
            if (currentDiffers &&
                currentTarget.isActor &&
                currentTarget.isAlive &&
                !currentTarget.name.empty() &&
                !NPCDetector::IsExcluded(currentTarget.formId, currentTarget.name) &&
                !IsConversationTargetOnCooldown(currentTarget.formId, currentTarget.name, true)) {
                UpdateConversationPartnerFromCurrentTarget("typed input prepared target");
            }
        }
    }

    SendPlayerMessage(message);
}

static bool SelectRpgCommentSpeaker(uint32_t& outFormId, std::string& outName) {
    outFormId = 0;
    outName.clear();

    if (g_conversationActive && !g_conversationIsNarrator && !g_conversationPartner.empty()) {
        outFormId = g_conversationPartnerFormId;
        outName = g_conversationPartner;
        return true;
    }

    const auto& target = TargetManager::GetCurrentTarget();
    if (target.isActor && target.isAIAgent && target.formId != 0 && !target.name.empty()) {
        outFormId = target.formId;
        outName = target.name;
        return true;
    }

    const float maxDistance = std::max(Config::distanceActivatingNpcInterior, Config::distanceActivatingNpcExterior);
    const auto freshCandidates = BuildFreshBoredCandidates(maxDistance);
    if (!freshCandidates.empty()) {
        outFormId = freshCandidates.front().first;
        outName = freshCandidates.front().second;
        return outFormId != 0 && !outName.empty();
    }

    return false;
}

static bool IsCombatRpgSpeakerCandidate(
    uint32_t formId,
    const RuntimeSnapshot::GameState& gameState,
    RuntimeSnapshot::ActorState& outActor) {
    return formId != 0 &&
        AgentManager::IsAIAgent(formId) &&
        RuntimeSnapshot::TryGetActor(formId, outActor) &&
        outActor.loaded3D &&
        !outActor.dead &&
        outActor.inCombat &&
        !outActor.name.empty() &&
        RuntimeSnapshot::IsActorInScene(outActor, gameState);
}

static bool SelectCombatRpgCommentSpeaker(uint32_t& outFormId, std::string& outName) {
    outFormId = 0;
    outName.clear();

    const RuntimeSnapshot::GameState gameState = RuntimeSnapshot::GetGameState();
    RuntimeSnapshot::ActorState actor;
    uint32_t conversationFormId = g_conversationPartnerFormId;
    if (conversationFormId == 0 && !g_conversationPartner.empty()) {
        conversationFormId = AgentManager::FindAgentFormIdByName(g_conversationPartner);
    }
    if (g_conversationActive && !g_conversationIsNarrator &&
        IsCombatRpgSpeakerCandidate(conversationFormId, gameState, actor)) {
        outFormId = actor.formId;
        outName = actor.name;
        return true;
    }

    const auto& target = TargetManager::GetCurrentTarget();
    if (target.isActor && target.isAIAgent &&
        IsCombatRpgSpeakerCandidate(target.formId, gameState, actor)) {
        outFormId = actor.formId;
        outName = actor.name;
        return true;
    }

    const float maxDistance = std::max(Config::distanceActivatingNpcInterior, Config::distanceActivatingNpcExterior);
    float nearestDistance = std::numeric_limits<float>::max();
    for (const RuntimeSnapshot::ActorState& candidate : RuntimeSnapshot::GetActors()) {
        if (!IsCombatRpgSpeakerCandidate(candidate.formId, gameState, actor) ||
            (maxDistance > 0.0f && actor.distanceToPlayer > maxDistance) ||
            actor.distanceToPlayer >= nearestDistance) {
            continue;
        }
        nearestDistance = actor.distanceToPlayer;
        outFormId = actor.formId;
        outName = actor.name;
    }

    return outFormId != 0 && !outName.empty();
}

struct CombatPromptContext {
    std::vector<std::string> allies;
    std::vector<std::string> hostiles;
};

// Builds the current combat graph without applying conversational race eligibility to enemies.
static CombatPromptContext BuildCombatPromptContext(
    uint32_t speakerFormId,
    const std::string& playerName) {
    CombatPromptContext context;
    const RuntimeSnapshot::GameState gameState = RuntimeSnapshot::GetGameState();
    const std::vector<RuntimeSnapshot::ActorState> actors = RuntimeSnapshot::GetActors();
    std::unordered_map<uint32_t, const RuntimeSnapshot::ActorState*> actorsByFormId;
    actorsByFormId.reserve(actors.size());
    for (const RuntimeSnapshot::ActorState& actor : actors) {
        if (actor.formId != 0) {
            actorsByFormId[actor.formId] = &actor;
        }
    }

    auto isLiveCombatActor = [&](const RuntimeSnapshot::ActorState& actor) {
        return actor.formId != 0 && actor.inCombat && actor.loaded3D && !actor.dead &&
            !actor.name.empty() && RuntimeSnapshot::IsActorInScene(actor, gameState);
    };

    std::set<uint32_t> alliedFormIds;
    std::set<uint32_t> hostileFormIds;
    std::set<std::string> alliedNames;
    std::set<std::string> hostileNames;
    auto addName = [](std::vector<std::string>& values, std::set<std::string>& names, const std::string& name) {
        const std::string trimmed = TrimInput(name);
        if (!trimmed.empty() && names.insert(ToLowerCopy(trimmed)).second) {
            values.push_back(trimmed);
        }
    };
    if (gameState.inCombat) {
        addName(context.allies, alliedNames, playerName);
    }
    if (gameState.playerFormId != 0) {
        alliedFormIds.insert(gameState.playerFormId);
    }
    alliedFormIds.insert(speakerFormId);
    for (const RuntimeSnapshot::ActorState& actor : actors) {
        if (!isLiveCombatActor(actor)) {
            continue;
        }
        if (actor.playerTeammate) {
            alliedFormIds.insert(actor.formId);
        } else if (actor.hostileToPlayer) {
            hostileFormIds.insert(actor.formId);
        }
    }

    bool expanded = true;
    while (expanded) {
        expanded = false;
        for (const RuntimeSnapshot::ActorState& actor : actors) {
            if (!isLiveCombatActor(actor) || actor.combatTargetFormId == 0) {
                continue;
            }
            const auto targetIt = actorsByFormId.find(actor.combatTargetFormId);
            if (targetIt == actorsByFormId.end() || !isLiveCombatActor(*targetIt->second)) {
                continue;
            }

            const bool actorAllied = alliedFormIds.count(actor.formId) != 0;
            const bool actorHostile = hostileFormIds.count(actor.formId) != 0;
            const bool targetAllied = alliedFormIds.count(actor.combatTargetFormId) != 0;
            const bool targetHostile = hostileFormIds.count(actor.combatTargetFormId) != 0;
            if (actorAllied && !targetAllied && !targetHostile) {
                expanded = hostileFormIds.insert(actor.combatTargetFormId).second || expanded;
            } else if (actorHostile && !targetAllied && !targetHostile) {
                expanded = alliedFormIds.insert(actor.combatTargetFormId).second || expanded;
            } else if (targetAllied && !actorAllied && !actorHostile) {
                expanded = hostileFormIds.insert(actor.formId).second || expanded;
            } else if (targetHostile && !actorAllied && !actorHostile) {
                expanded = alliedFormIds.insert(actor.formId).second || expanded;
            }
        }
    }

    const auto speakerIt = actorsByFormId.find(speakerFormId);
    if (speakerIt != actorsByFormId.end()) {
        addName(context.allies, alliedNames, speakerIt->second->name);
    }
    for (const RuntimeSnapshot::ActorState& actor : actors) {
        if (!isLiveCombatActor(actor)) {
            continue;
        }
        if (alliedFormIds.count(actor.formId) != 0) {
            addName(context.allies, alliedNames, actor.name);
        } else if (hostileFormIds.count(actor.formId) != 0) {
            addName(context.hostiles, hostileNames, actor.name);
        }
    }

    return context;
}

struct RpgBridgeEvent {
    std::string eventType;
    std::string eventText;
    std::string itemBaseId;
    std::string itemName;
    std::vector<std::pair<std::string, std::string>> items;
};

static std::mutex g_rpgCommentQueueMutex;
static std::deque<RpgBridgeEvent> g_rpgCommentQueue;
static std::unordered_map<std::string, std::chrono::steady_clock::time_point> g_lastQueuedRpgEvent;

void QueueRpgCommentEvent(const std::string& eventType, const std::string& eventText) {
    const std::string type = TrimInput(eventType);
    const std::string text = TrimInput(eventText);
    if (type.empty() || text.empty()) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const std::string dedupeKey = type + "\n" + text;
    std::lock_guard<std::mutex> lock(g_rpgCommentQueueMutex);
    for (auto it = g_lastQueuedRpgEvent.begin(); it != g_lastQueuedRpgEvent.end();) {
        if (now - it->second >= std::chrono::minutes(1)) {
            it = g_lastQueuedRpgEvent.erase(it);
        } else {
            ++it;
        }
    }
    const auto previous = g_lastQueuedRpgEvent.find(dedupeKey);
    if (previous != g_lastQueuedRpgEvent.end() && now - previous->second < std::chrono::seconds(10)) {
        return;
    }
    g_lastQueuedRpgEvent[dedupeKey] = now;
    g_rpgCommentQueue.push_back({type, text});
    while (g_rpgCommentQueue.size() > 16) {
        g_rpgCommentQueue.pop_front();
    }
}

static std::string JoinHumanList(const std::vector<std::string>& values) {
    if (values.empty()) {
        return "";
    }
    if (values.size() == 1) {
        return values.front();
    }
    if (values.size() == 2) {
        return values[0] + " and " + values[1];
    }

    std::ostringstream joined;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i > 0) {
            joined << (i + 1 == values.size() ? ", and " : ", ");
        }
        joined << values[i];
    }
    return joined.str();
}

static RpgBridgeEvent CombinePlayerConsumedEvents(const std::vector<RpgBridgeEvent>& events) {
    if (events.empty()) {
        return {};
    }
    if (events.size() == 1) {
        return events.front();
    }

    RpgBridgeEvent combined = events.front();
    std::vector<std::string> itemNames;
    combined.items.clear();
    for (const auto& event : events) {
        std::string itemName = !event.itemName.empty() ? event.itemName : event.eventText;
        const std::string consumedMarker = " consumed ";
        const size_t marker = itemName.find(consumedMarker);
        if (marker != std::string::npos) {
            itemName = TrimInput(itemName.substr(marker + consumedMarker.size()));
        }
        if (itemName.empty()) {
            itemName = "aid item";
        }
        itemNames.push_back(itemName);
        combined.items.push_back({ event.itemBaseId, itemName });
    }

    std::string actorName = "The Courier";
    const std::string consumedMarker = " consumed ";
    const size_t marker = events.front().eventText.find(consumedMarker);
    if (marker != std::string::npos) {
        actorName = TrimInput(events.front().eventText.substr(0, marker));
    }
    combined.eventText = actorName + " consumed " + JoinHumanList(itemNames);
    combined.itemBaseId.clear();
    combined.itemName = JoinHumanList(itemNames);
    return combined;
}

static void ProcessRpgEventBridge() {
    const std::string data = ReadAndDeleteTextInputFile("Data\\NVSE\\Plugins\\dialectic_rpg_events.tmp");
    std::vector<RpgBridgeEvent> events;
    {
        std::lock_guard<std::mutex> lock(g_rpgCommentQueueMutex);
        while (!g_rpgCommentQueue.empty()) {
            events.push_back(std::move(g_rpgCommentQueue.front()));
            g_rpgCommentQueue.pop_front();
        }
    }
    if (data.empty() && events.empty()) {
        return;
    }

    std::vector<RpgBridgeEvent> pendingConsumed;
    auto flushPendingConsumed = [&events, &pendingConsumed]() {
        if (!pendingConsumed.empty()) {
            events.push_back(CombinePlayerConsumedEvents(pendingConsumed));
            pendingConsumed.clear();
        }
    };

    std::istringstream stream(data);
    std::string line;
    while (std::getline(stream, line)) {
        line = TrimInput(line);
        if (line.empty()) {
            continue;
        }

        std::vector<std::string> fields;
        std::string field;
        std::istringstream fieldStream(line);
        while (std::getline(fieldStream, field, '|')) {
            fields.push_back(TrimInput(field));
        }

        if (fields.size() < 2) {
            Logger::LogWarning("GameLoop: Ignoring malformed RPG event bridge line: %s", line.c_str());
            continue;
        }

        const std::string eventType = fields[0];
        const std::string eventText = fields[1];
        if (eventType.empty() || eventText.empty()) {
            continue;
        }
        const std::string itemBaseId = fields.size() >= 3 ? fields[2] : "";
        const std::string itemName = fields.size() >= 4 ? fields[3] : "";

        RpgBridgeEvent event;
        event.eventType = eventType;
        event.eventText = eventText;
        event.itemBaseId = itemBaseId;
        event.itemName = itemName;
        if (eventType == "player_consumed") {
            event.items.push_back({ itemBaseId, itemName });
            pendingConsumed.push_back(event);
            continue;
        }

        flushPendingConsumed();
        events.push_back(event);
    }
    flushPendingConsumed();

    for (const auto& event : events) {
        if (event.eventType == "goodnight" ||
            event.eventType == "waitstart" ||
            event.eventType == "waitstop") {
            RefreshPlayerNameFromGame();
            const std::string playerName = Config::playerName.empty() ? "Player" : Config::playerName;
            const std::string location = Misc::GetPlayerLocation();
            const std::string audienceJson = BuildAudienceSnapshotJson("sleep_wait");
            const std::string people = ExtractPeopleFromAudienceSnapshotJson(audienceJson);

            std::ostringstream payload;
            payload << "{"
                    << "\"schema\":\"dialectic.sleep_wait.v1\","
                    << "\"event\":\"" << HTTPManager::EscapeJson(event.eventType) << "\","
                    << "\"player\":\"" << HTTPManager::EscapeJson(playerName) << "\","
                    << "\"text\":\"" << HTTPManager::EscapeJson(event.eventText) << "\","
                    << "\"location\":\"" << HTTPManager::EscapeJson(location) << "\","
                    << "\"people\":\"" << HTTPManager::EscapeJson(people) << "\","
                    << "\"audience_snapshot\":" << audienceJson << ","
                    << "\"game\":\"fnv\""
                    << "}";

            Logger::LogInfo("GameLoop: Forwarding sleep/wait lifecycle event %s: %s",
                event.eventType.c_str(),
                event.eventText.c_str());
            HTTPManager::SendEvent(event.eventType, payload.str(), audienceJson);
            continue;
        }

        uint32_t speakerFormId = 0;
        std::string speakerName;
        const bool speakerSelected = event.eventType == "combatbark"
            ? SelectCombatRpgCommentSpeaker(speakerFormId, speakerName)
            : SelectRpgCommentSpeaker(speakerFormId, speakerName);
        if (!speakerSelected) {
            Logger::LogInfo("GameLoop: RPG event %s skipped; no active Dialectic NPC speaker", event.eventType.c_str());
            continue;
        }

        RefreshPlayerNameFromGame();
        const std::string playerName = Config::playerName.empty() ? "Player" : Config::playerName;
        const std::string location = Misc::GetPlayerLocation();
        const bool useNearbyAudience = event.eventType == "player_consumed";
        const std::string narrowPeople = "|" + speakerName + "|" + playerName + "|";
        const std::string audienceJson = useNearbyAudience
            ? BuildAudienceSnapshotJson("npc_close")
            : std::string("{\"people\":\"") + HTTPManager::EscapeJson(narrowPeople) + "\"}";
        std::string people = useNearbyAudience ? ExtractPeopleFromAudienceSnapshotJson(audienceJson) : narrowPeople;
        if (people.empty()) {
            people = narrowPeople;
        }

        const CombatPromptContext combatContext = event.eventType == "combatbark"
            ? BuildCombatPromptContext(speakerFormId, playerName)
            : CombatPromptContext{};

        std::ostringstream payload;
        payload << "{"
                << "\"schema\":\"dialectic.rpg_event.v1\","
                << "\"event\":\"" << HTTPManager::EscapeJson(event.eventType) << "\","
                << "\"npc\":\"" << HTTPManager::EscapeJson(speakerName) << "\","
                << "\"npc_id\":\"" << HTTPManager::EscapeJson(FormatFormIdJsonValue(speakerFormId)) << "\","
                << "\"speaker\":\"" << HTTPManager::EscapeJson(speakerName) << "\","
                << "\"speaker_formid\":\"" << HTTPManager::EscapeJson(FormatFormIdJsonValue(speakerFormId)) << "\","
                << "\"player\":\"" << HTTPManager::EscapeJson(playerName) << "\","
                << "\"text\":\"" << HTTPManager::EscapeJson(event.eventText) << "\","
                << "\"location\":\"" << HTTPManager::EscapeJson(location) << "\","
                << "\"people\":\"" << HTTPManager::EscapeJson(people) << "\","
                << "\"audience_snapshot\":" << audienceJson;
        if (event.eventType == "combatbark") {
            payload << ",\"combat\":{\"allies_currently_fighting\":[";
            for (size_t i = 0; i < combatContext.allies.size(); ++i) {
                if (i > 0) {
                    payload << ",";
                }
                payload << "\"" << HTTPManager::EscapeJson(combatContext.allies[i]) << "\"";
            }
            payload << "],\"hostile_combatants\":[";
            for (size_t i = 0; i < combatContext.hostiles.size(); ++i) {
                if (i > 0) {
                    payload << ",";
                }
                payload << "\"" << HTTPManager::EscapeJson(combatContext.hostiles[i]) << "\"";
            }
            payload << "]}";
        }
        if (event.eventType == "player_consumed") {
            payload << ",\"item\":{"
                    << "\"name\":\"" << HTTPManager::EscapeJson(event.itemName.empty() ? event.eventText : event.itemName) << "\","
                    << "\"baseid\":\"" << HTTPManager::EscapeJson(event.itemBaseId) << "\","
                    << "\"type\":\"aid\","
                    << "\"count\":" << std::max<size_t>(event.items.size(), 1)
                    << "}";
            if (!event.items.empty()) {
                payload << ",\"items\":[";
                for (size_t i = 0; i < event.items.size(); ++i) {
                    if (i > 0) {
                        payload << ",";
                    }
                    payload << "{"
                            << "\"name\":\"" << HTTPManager::EscapeJson(event.items[i].second) << "\","
                            << "\"baseid\":\"" << HTTPManager::EscapeJson(event.items[i].first) << "\","
                            << "\"type\":\"aid\""
                            << "}";
                }
                payload << "]";
            }
        }
        payload << ","
                << "\"game\":\"fnv\""
                << "}";

        if (event.eventType == "combatbark") {
            Logger::LogInfo("GameLoop: Forwarding combat bark for %s (allies=%zu hostiles=%zu): %s",
                speakerName.c_str(),
                combatContext.allies.size(),
                combatContext.hostiles.size(),
                event.eventText.c_str());
        } else {
            Logger::LogInfo("GameLoop: Forwarding RPG comment event %s for %s: %s",
                event.eventType.c_str(),
                speakerName.c_str(),
                event.eventText.c_str());
        }
        HTTPManager::SendEvent(event.eventType, payload.str(), audienceJson);
    }
}

static void MaybeSplitSpeakerPrefix(std::string& speaker, std::string& text) {
    if (!speaker.empty() || text.empty()) {
        return;
    }

    const size_t colonPos = text.find(':');
    if (colonPos == std::string::npos || colonPos == 0 || colonPos > 48) {
        return;
    }

    std::string candidate = TrimInput(text.substr(0, colonPos));
    if (candidate.empty() || candidate.size() > 48) {
        return;
    }

    bool plausibleName = true;
    for (char ch : candidate) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (!std::isalnum(c) && ch != ' ' && ch != '\'' && ch != '-' && ch != '.' && ch != '_') {
            plausibleName = false;
            break;
        }
    }

    if (!plausibleName) {
        return;
    }

    speaker = candidate;
    text = TrimInput(text.substr(colonPos + 1));
}

static uint32_t ResolveCapturedDialogueSpeakerFormId(const std::string& speaker, uint32_t bridgeFormId) {
    if (bridgeFormId != 0) {
        return bridgeFormId;
    }
    if (speaker.empty() || EqualsIgnoreCase(speaker, "Unknown")) {
        return 0;
    }
    return ResolveResponseSpeakerFormId(speaker);
}

struct CapturedDialogueSpeakerAttribution {
    std::string name;
    uint32_t formId = 0;
    std::string source;
};

static CapturedDialogueSpeakerAttribution g_recentDialogueMenuSpeakerAttribution;
static std::chrono::steady_clock::time_point g_recentDialogueMenuSpeakerAttributionTime;

static bool IsUnknownCapturedSpeakerName(const std::string& speaker) {
    const std::string name = TrimInput(speaker);
    return name.empty() ||
        EqualsIgnoreCase(name, "Unknown") ||
        EqualsIgnoreCase(name, "<no name>") ||
        EqualsIgnoreCase(name, "%e");
}

static bool IsUsableCapturedSpeakerName(const std::string& speaker) {
    const std::string name = TrimInput(speaker);
    return !name.empty() &&
        !IsUnknownCapturedSpeakerName(name) &&
        !IsPlayerSpeakerName(name);
}

static bool IsDialogueMenuCaptureSource(const std::string& source) {
    return source == "dialogue_menu" ||
        source == "dialogue_menu_npc" ||
        source == "dialogue_menu_player";
}

static bool IsDialogueMenuNpcCaptureSource(const std::string& source) {
    return source == "dialogue_menu" ||
        source == "dialogue_menu_npc";
}

static void RememberDialogueMenuSpeakerAttribution(const CapturedDialogueSpeakerAttribution& attribution) {
    if (attribution.formId == 0 || !IsUsableCapturedSpeakerName(attribution.name)) {
        return;
    }

    g_recentDialogueMenuSpeakerAttribution = attribution;
    g_recentDialogueMenuSpeakerAttributionTime = std::chrono::steady_clock::now();
}

static CapturedDialogueSpeakerAttribution GetRecentDialogueMenuSpeakerAttribution() {
    if (g_recentDialogueMenuSpeakerAttribution.formId == 0 ||
        !IsUsableCapturedSpeakerName(g_recentDialogueMenuSpeakerAttribution.name) ||
        g_recentDialogueMenuSpeakerAttributionTime.time_since_epoch().count() == 0) {
        return {};
    }

    constexpr auto kMaxDialogueMenuSpeakerAge = std::chrono::seconds(15);
    const auto now = std::chrono::steady_clock::now();
    if (now - g_recentDialogueMenuSpeakerAttributionTime > kMaxDialogueMenuSpeakerAge) {
        g_recentDialogueMenuSpeakerAttribution = {};
        g_recentDialogueMenuSpeakerAttributionTime = {};
        return {};
    }

    CapturedDialogueSpeakerAttribution attribution = g_recentDialogueMenuSpeakerAttribution;
    attribution.source = "recent_dialogue_menu_speaker";
    return attribution;
}

static bool IsAttributionActorEligible(const ActorPositionResolverFNV::PositionResult& position) {
    if (!position.resolved ||
        position.formId == 0 ||
        position.formId == 0x00000014 ||
        !IsUsableCapturedSpeakerName(position.actorName)) {
        return false;
    }

    ActorEligibilityFNV::Metadata metadata = EligibilityMetadataFromPosition(position);
    return !ActorEligibilityFNV::IsClearlyDisallowedCreature(metadata);
}

static CapturedDialogueSpeakerAttribution GetCurrentTargetSpeakerAttribution() {
    const auto& target = TargetManager::GetCurrentTarget();
    if (target.formId == 0 ||
        !target.isActor ||
        !target.isAlive ||
        !IsUsableCapturedSpeakerName(target.name)) {
        return {};
    }

    CapturedDialogueSpeakerAttribution attribution;
    attribution.name = target.name;
    attribution.formId = target.formId;
    attribution.source = "current_target";
    return attribution;
}

static CapturedDialogueSpeakerAttribution InferCapturedDialogueSpeakerFromSpatialCache() {
    const auto player = ActorPositionResolverFNV::ResolvePlayer();
    if (!player.resolved) {
        return {};
    }

    struct Candidate {
        std::string name;
        uint32_t formId = 0;
        float score = 0.0f;
        float distance = 0.0f;
    };

    std::vector<Candidate> candidates;
    const auto positions = ActorPositionResolverFNV::GetRecentActorPositions();
    for (const auto& position : positions) {
        if (!IsAttributionActorEligible(position)) {
            continue;
        }

        const auto spatial = SpatialAwarenessFNV::Evaluate(player, position);
        if (!std::isfinite(spatial.airDistance) || spatial.airDistance > 1400.0f) {
            continue;
        }

        float score = spatial.airDistance;
        if (position.actorHasLosToPlayerKnown && !position.actorHasLosToPlayer) {
            score += 300.0f;
        }
        if (position.playerHasLosToActorKnown && !position.playerHasLosToActor) {
            score += 150.0f;
        }
        if (!spatial.canCommunicate) {
            score += 500.0f;
        }
        if (!AgentManager::IsAIAgent(position.formId)) {
            score += 50.0f;
        }

        candidates.push_back({ position.actorName, position.formId, score, spatial.airDistance });
    }

    if (candidates.empty()) {
        return {};
    }

    std::sort(candidates.begin(), candidates.end(), [](const Candidate& left, const Candidate& right) {
        return left.score < right.score;
    });

    if (candidates.size() > 1 &&
        candidates[0].distance > 250.0f &&
        std::fabs(candidates[0].score - candidates[1].score) < 125.0f) {
        Logger::LogInfo("GameLoop: Captured dialogue speaker attribution ambiguous: %s 0x%08X vs %s 0x%08X",
            candidates[0].name.c_str(),
            candidates[0].formId,
            candidates[1].name.c_str(),
            candidates[1].formId);
        return {};
    }

    CapturedDialogueSpeakerAttribution attribution;
    attribution.name = candidates[0].name;
    attribution.formId = candidates[0].formId;
    attribution.source = "nearest_spatial_actor";
    return attribution;
}

static CapturedDialogueSpeakerAttribution ResolveCapturedDialogueSpeakerAttribution(
    const std::string& source,
    const std::string& speaker,
    uint32_t speakerFormId) {
    if (!IsUnknownCapturedSpeakerName(speaker) && speakerFormId != 0) {
        CapturedDialogueSpeakerAttribution attribution{ speaker, speakerFormId, "dialogue_bridge" };
        if (IsDialogueMenuNpcCaptureSource(source)) {
            RememberDialogueMenuSpeakerAttribution(attribution);
        }
        return attribution;
    }

    if (IsDialogueMenuCaptureSource(source) && !IsUnknownCapturedSpeakerName(speaker)) {
        return { speaker, speakerFormId, speakerFormId != 0 ? "dialogue_bridge" : "dialogue_bridge_name_only" };
    }

    CapturedDialogueSpeakerAttribution attribution;
    if (IsDialogueMenuNpcCaptureSource(source)) {
        attribution = GetRecentDialogueMenuSpeakerAttribution();
        if (attribution.formId != 0) {
            Logger::LogInfo("GameLoop: Using recent dialogue menu speaker attribution: %s 0x%08X",
                attribution.name.c_str(),
                attribution.formId);
            return attribution;
        }
    }

    attribution = GetCurrentTargetSpeakerAttribution();
    if (attribution.formId != 0 && speakerFormId == 0) {
        if (IsDialogueMenuNpcCaptureSource(source)) {
            RememberDialogueMenuSpeakerAttribution(attribution);
            Logger::LogInfo("GameLoop: Using current dialogue target attribution: %s 0x%08X",
                attribution.name.c_str(), attribution.formId);
            return attribution;
        }
        if (!IsDialogueMenuCaptureSource(source)) {
            return attribution;
        }
    }

    attribution = InferCapturedDialogueSpeakerFromSpatialCache();
    if (attribution.formId != 0) {
        return attribution;
    }

    if (!IsUnknownCapturedSpeakerName(speaker)) {
        return { speaker, speakerFormId, speakerFormId != 0 ? "dialogue_bridge" : "name_only" };
    }

    return {};
}

static bool IsRecentDuplicateCapturedDialogue(const std::string& source,
                                               const std::string& speaker,
                                               const std::string& text,
                                               const std::string& transport) {
    const int windowMs = std::max(0, Config::dialogueCaptureRepeatWindowMs);
    if (windowMs == 0) {
        return false;
    }

    static std::string lastKey;
    static std::string lastTransport;
    static std::chrono::steady_clock::time_point lastTime;
    const std::string key = ToLowerCopy(source + "|" + speaker + "|" + text);
    const auto now = std::chrono::steady_clock::now();
    if (key == lastKey &&
        now - lastTime < std::chrono::milliseconds(windowMs)) {
        if (!transport.empty() && !lastTransport.empty() && transport != lastTransport) {
            Logger::LogInfo(
                "[NATIVE_COMPARE] dialogue transports matched source=%s speaker=%s native=%d bridge=%d",
                source.c_str(),
                speaker.c_str(),
                (transport == "native_command" || lastTransport == "native_command") ? 1 : 0,
                (transport == "bridge_file" || lastTransport == "bridge_file") ? 1 : 0);
            NativeComparisonTelemetry::Record(
                "dialogue_capture", NativeComparisonTelemetry::Result::Match,
                source + " speaker=" + speaker);
        }
        return true;
    }

    lastKey = key;
    lastTransport = transport;
    lastTime = now;
    return false;
}

static void ProcessDialogueCaptureBridge() {
    if (!Config::dialogueCaptureEnabled) {
        return;
    }

    std::string bridgeData;
    {
        std::lock_guard<std::mutex> lock(g_nativeDialogueCaptureMutex);
        if (!g_nativeDialogueCaptures.empty()) {
            bridgeData = std::move(g_nativeDialogueCaptures.front());
            g_nativeDialogueCaptures.pop_front();
        }
    }
    if (bridgeData.empty()) {
        return;
    }

    const auto fields = ParseBridgeKeyValueData(bridgeData);
    auto getField = [&fields](const char* key) -> std::string {
        const auto it = fields.find(key);
        return it == fields.end() ? "" : it->second;
    };

    if (getField("end") != "1") {
        static std::chrono::steady_clock::time_point lastIncompleteLogTime;
        const auto now = std::chrono::steady_clock::now();
        if (lastIncompleteLogTime.time_since_epoch().count() == 0 ||
            now - lastIncompleteLogTime > std::chrono::seconds(2)) {
            Logger::LogInfo("GameLoop: Waiting for dialogue capture bridge payload to finish");
            lastIncompleteLogTime = now;
        }
        return;
    }

    std::string source = ToLowerCopy(getField("source"));
    if (source.empty()) {
        source = "radiant";
    }
    const std::string transport = getField("transport").empty()
        ? "bridge_file" : ToLowerCopy(getField("transport"));
    if (IsDialogueMenuCaptureSource(source) && !Config::dialogueCaptureDialogueMenu) {
        return;
    }
    if (!IsDialogueMenuCaptureSource(source) && !Config::dialogueCaptureRadiant) {
        return;
    }

    std::string text = StripDialogueMetadata(getField("text"));
    const std::string rawCapturedText = text;
    std::string speaker = TrimInput(getField("speaker"));
    if (SpeakManager::IsRecentAISubtitleText(rawCapturedText)) {
        Logger::LogInfo("GameLoop: Skipping captured dialogue because it matches a recent Dialectic AI subtitle: %s",
            rawCapturedText.c_str());
        return;
    }
    MaybeSplitSpeakerPrefix(speaker, text);
    if (SpeakManager::IsRecentAISubtitleText(text)) {
        Logger::LogInfo("GameLoop: Skipping captured dialogue because stripped text matches a recent Dialectic AI subtitle: %s",
            text.c_str());
        return;
    }

    if (text.empty() || text == "%e") {
        return;
    }

    RefreshPlayerNameFromGame();
    const bool isPlayerLine = getField("is_player_line") == "1" || IsPlayerSpeakerName(speaker);
    if (isPlayerLine && !Config::dialogueCapturePlayerMenuChoices) {
        return;
    }
    if (isPlayerLine && !Config::playerName.empty()) {
        speaker = Config::playerName;
    }

    const std::string activeAiSubtitle = TrimInput(ReadFileIfExists("Data\\NVSE\\Plugins\\dialectic_subtitle.txt"));
    if (!activeAiSubtitle.empty() && EqualsIgnoreCase(StripDialogueMetadata(activeAiSubtitle), text)) {
        Logger::LogInfo("GameLoop: Skipping captured subtitle that matches active Dialectic AI playback");
        return;
    }

    uint32_t speakerFormId = ResolveCapturedDialogueSpeakerFormId(
        speaker,
        ParseFormIdString(getField("speaker_refid")));
    if (isPlayerLine && speakerFormId == 0) {
        speakerFormId = Misc::GetPlayerFormId();
    }

    std::string speakerAttribution = isPlayerLine ? "player_dialogue_menu" : "dialogue_bridge";
    if (!isPlayerLine) {
        CapturedDialogueSpeakerAttribution attribution =
            ResolveCapturedDialogueSpeakerAttribution(source, speaker, speakerFormId);
        if (attribution.formId != 0 && IsUsableCapturedSpeakerName(attribution.name)) {
            speaker = attribution.name;
            speakerFormId = attribution.formId;
            speakerAttribution = attribution.source;
        }
    }

    if (speaker.empty()) {
        speaker = "Unknown";
        speakerAttribution = "unknown";
    } else if (EqualsIgnoreCase(speaker, "Unknown")) {
        speakerAttribution = "unknown";
    }

    if (Config::dialogueCaptureRequireKnownSpeaker && speakerFormId == 0 && !IsPlayerSpeakerName(speaker)) {
        Logger::LogInfo("GameLoop: Skipping captured dialogue with unknown speaker: %s", text.c_str());
        return;
    }

    if (IsRecentDuplicateCapturedDialogue(source, speaker, text, transport)) {
        return;
    }

    std::string listener = TrimInput(getField("target"));
    uint32_t listenerFormId = ParseFormIdString(getField("target_refid"));
    if (listener.empty()) {
        listener = isPlayerLine ? g_conversationPartner : (Config::playerName.empty() ? "Player" : Config::playerName);
    }
    if (IsPlayerSpeakerName(listener) && !Config::playerName.empty()) {
        listener = Config::playerName;
    }

    const std::string rawLocation = TrimInput(Misc::GetPlayerLocation());
    const std::string location = rawLocation.empty() ? "Unknown" : rawLocation;
    std::ostringstream payload;
    payload << "{";
    payload << "\"schema\":\"dialectic.captured_dialogue.v1\",";
    payload << "\"source\":\"" << HTTPManager::EscapeJson(source) << "\",";
    payload << "\"speaker\":\"" << HTTPManager::EscapeJson(speaker) << "\",";
    payload << "\"speaker_formid\":\"" << HTTPManager::EscapeJson(FormatFormIdJsonValue(speakerFormId)) << "\",";
    payload << "\"speaker_attribution\":\"" << HTTPManager::EscapeJson(speakerAttribution) << "\",";
    payload << "\"listener\":\"" << HTTPManager::EscapeJson(listener) << "\",";
    payload << "\"listener_formid\":\"" << HTTPManager::EscapeJson(FormatFormIdJsonValue(listenerFormId)) << "\",";
    payload << "\"text\":\"" << HTTPManager::EscapeJson(text) << "\",";
    payload << "\"player_name\":\"" << HTTPManager::EscapeJson(Config::playerName.empty() ? "Player" : Config::playerName) << "\",";
    payload << "\"is_player_line\":" << (isPlayerLine ? "true" : "false") << ",";
    payload << "\"menu_mode\":" << (getField("menu_mode") == "1" ? "true" : "false") << ",";
    payload << "\"capture_id\":\"" << HTTPManager::EscapeJson(getField("capture_id")) << "\",";
    payload << "\"location\":\"" << HTTPManager::EscapeJson(location) << "\"";
    payload << "}";

    Logger::LogInfo("GameLoop: Captured %s dialogue transport=%s speaker=%s ref=0x%08X attribution=%s text=%s",
        source.c_str(),
        transport.c_str(),
        speaker.c_str(),
        speakerFormId,
        speakerAttribution.c_str(),
        text.c_str());
    ResetBoredEventTimer("captured dialogue");
    HTTPManager::SendEvent("captured_dialogue", payload.str());
}

void SubmitCapturedDialogue(const std::string& source,
                            const std::string& speaker,
                            const std::string& speakerRefId,
                            const std::string& target,
                            const std::string& targetRefId,
                            const std::string& text,
                            bool isPlayerLine,
                            bool menuMode,
                            const std::string& captureId) {
    auto isBridgeVariable = [](const std::string& value) {
        return _stricmp(value.c_str(), "sSubtitle") == 0 ||
            _stricmp(value.c_str(), "sSpeakerName") == 0 ||
            _stricmp(value.c_str(), "sTargetName") == 0;
    };
    if (isBridgeVariable(text) || isBridgeVariable(speaker) ||
        text.find("sSubtitle") != std::string::npos) {
        Logger::LogWarning("[NATIVE_DIALOGUE] rejected unresolved script variable source=%s speaker=%s text=%s",
            source.c_str(), speaker.c_str(), text.c_str());
        return;
    }
    auto singleLine = [](std::string value) {
        std::replace(value.begin(), value.end(), '\r', ' ');
        std::replace(value.begin(), value.end(), '\n', ' ');
        return value;
    };
    std::ostringstream payload;
    payload << "source=" << singleLine(source) << "\n"
            << "speaker=" << singleLine(speaker) << "\n"
            << "speaker_refid=" << singleLine(speakerRefId) << "\n"
            << "target=" << singleLine(target) << "\n"
            << "target_refid=" << singleLine(targetRefId) << "\n"
            << "text=" << singleLine(text) << "\n"
            << "is_player_line=" << (isPlayerLine ? 1 : 0) << "\n"
            << "menu_mode=" << (menuMode ? 1 : 0) << "\n"
            << "capture_id=" << singleLine(captureId) << "\n"
            << "transport=native_command\n"
            << "end=1\n";
    {
        std::lock_guard<std::mutex> lock(g_nativeDialogueCaptureMutex);
        while (g_nativeDialogueCaptures.size() >= 64) {
            g_nativeDialogueCaptures.pop_front();
            Logger::LogWarning("[NATIVE_DIALOGUE] capture queue full; dropped oldest payload");
        }
        g_nativeDialogueCaptures.push_back(payload.str());
    }
    Logger::LogDebug("[NATIVE_DIALOGUE] queued capture source=%s speaker=%s id=%s",
        source.c_str(), speaker.c_str(), captureId.c_str());
}

static void HandleOpenMicVoiceDetected() {
    if (!Config::openMicEnabled || Config::openMicMuted) {
        return;
    }
    if (g_voiceInputActive || VoiceRecorder::IsRecording()) {
        Logger::LogInfo("GameLoop: Ignoring open mic trigger voiceActive=%d recording=%d",
            g_voiceInputActive.load() ? 1 : 0,
            VoiceRecorder::IsRecording() ? 1 : 0);
        return;
    }

    if (!g_conversationActive) {
        if (Config::narratorModeEnabled) {
            StartNarratorConversation("open mic narrator mode");
        } else if (TryStartConversationFromCurrentTarget() != ConversationStartResult::Started) {
            Logger::LogInfo("GameLoop: Open mic detected speech without an available NPC target");
            return;
        }
    }

    StartVoiceInputInternal(true);
}

static void UpdateOpenMicMonitoringState() {
    VoiceRecorder::UpdateOpenMicSettings(
        Config::openMicSensitivity,
        Config::openMicEndDelaySeconds,
        Config::openMicMuted);

    const bool shouldMonitor =
        Config::openMicEnabled &&
        !Config::openMicMuted &&
        g_gameState.isInGame &&
        !g_gameState.isLoading &&
        !g_gameState.isPaused &&
        !g_gameState.isInMenu &&
        !g_gameState.isInDialogue &&
        InputManager::IsGameForeground() &&
        !g_voiceInputActive &&
        !VoiceRecorder::IsRecording();

    if (shouldMonitor) {
        if (!VoiceRecorder::IsOpenMicMonitoring()) {
            Logger::LogInfo("GameLoop: Starting open mic monitor");
            VoiceRecorder::StartOpenMicMonitoring(HandleOpenMicVoiceDetected);
        }
    } else if (VoiceRecorder::IsOpenMicMonitoring()) {
        Logger::LogInfo("GameLoop: Stopping open mic monitor");
        VoiceRecorder::StopOpenMicMonitoring();
    }
}

static int ResetRuntimeForAIActions(const char* reason, bool notifyServer,
    bool haltActorActions, bool clearCapturedDialogue) {
    InputManager::ResetChatGestures();
    const bool hadConversation = g_conversationActive;
    const std::string previousPartner = g_conversationPartner;
    const char* resetReason = reason ? reason : "runtime reset";

    Logger::LogInfo("GameLoop: Resetting AI runtime state (%s)", resetReason);

    g_voiceInputActive = false;
    VoiceRecorder::StopRecording();
    VoiceRecorder::StopOpenMicMonitoring();
    if (clearCapturedDialogue) {
        std::lock_guard<std::mutex> lock(g_nativeDialogueCaptureMutex);
        g_nativeDialogueCaptures.clear();
    }

    SpeakManager::CancelDialogueTurn(resetReason, false, false);
    AutoGreetingFNV::Cancel(resetReason);
    int haltedActors = 0;
    if (haltActorActions) {
        haltedActors = ActionManager::HaltAIActions(resetReason);
    }

    if (notifyServer && hadConversation && !previousPartner.empty()) {
        std::ostringstream payload;
        payload << "{"
                << "\"schema\":\"dialectic.conversation.v1\","
                << "\"action\":\"conversation_end\","
                << "\"npc\":\"" << HTTPManager::EscapeJson(previousPartner) << "\","
                << "\"game\":\"fnv\","
                << "\"reason\":\"" << HTTPManager::EscapeJson(resetReason) << "\""
                << "}";
        HTTPManager::SendEvent("conversation_end", payload.str());
    }

    ResetBoredEventTimer(resetReason);

    g_conversationActive = false;
    g_conversationIsNarrator = false;
    g_conversationPartner.clear();
    g_conversationPartnerFormId = 0;

    Logger::LogInfo("GameLoop: AI runtime reset completed haltedActors=%d haltActorActions=%d (%s)",
                    haltedActors,
                    haltActorActions ? 1 : 0,
                    resetReason);
    return haltedActors;
}

static void MaybeSendLoadedSaveInit() {
    if (g_loadedSaveInitBlocked) {
        return;
    }

    const long long currentGamets = WorldContextFNV::GetGameTimestamp();
    if (currentGamets <= 0) {
        return;
    }

    const bool firstInit = !g_loadedSaveInitSent;
    const bool detectedRollback = g_lastSeenGamets > 0 && currentGamets + 1000000 < g_lastSeenGamets;
    if (!firstInit && !detectedRollback) {
        if (currentGamets > g_lastSeenGamets) {
            g_lastSeenGamets = currentGamets;
        }
        return;
    }

    g_loadedSaveInitSent = true;
    g_lastSeenGamets = currentGamets;

    const char* reason = firstInit ? "loaded_save" : "gamets_rollback";
    ResetRuntimeForAIActions(reason, false, false);
    Logger::LogInfo("GameLoop: Sending init event for %s at gamets=%lld (plugin=%s)",
                    reason,
                    currentGamets,
                    DIALECTIC_VERSION);
    HTTPManager::SendEvent("init", DIALECTIC_VERSION);
    VersionCheck::Schedule();
}

static void ProcessNativeRuntimeEvents() {
    const std::vector<RuntimeEventBus::Event> events = RuntimeEventBus::Drain();
    for (const RuntimeEventBus::Event& event : events) {
        using Type = RuntimeEventBus::EventType;
        switch (event.type) {
            case Type::PreLoadGame:
                PlayerInventoryManagerFNV::Reset("native_pre_load_game");
                PlayerSurvivalManagerFNV::Reset("native_pre_load_game");
                FalloutStatsManagerFNV::Reset("native_pre_load_game");
                ResetRuntimeForAIActions("native_pre_load_game", false, false);
                BeginDynamicProfileTimerBlock("pre-load game");
                g_loadedSaveInitSent = false;
                g_lastSeenGamets = 0;
                g_loadedSaveInitBlocked = true;
                break;
            case Type::LoadGame:
                PlayerInventoryManagerFNV::Reset("native_load_game");
                PlayerInventoryManagerFNV::ForceRefresh("native_load_game", 2000);
                PlayerSurvivalManagerFNV::Reset("native_load_game");
                PlayerSurvivalManagerFNV::ForceRefresh("native_load_game", 3000);
                FalloutStatsManagerFNV::Reset("native_load_game");
                ResetRuntimeForAIActions("native_load_game", false, false);
                BeginDynamicProfileTimerBlock("load game");
                DelayDynamicProfileTimerAfterLoad("game load");
                g_loadedSaveInitSent = false;
                g_lastSeenGamets = 0;
                g_loadedSaveInitBlocked = true;
                break;
            case Type::PostLoadGame:
                g_lastSeenGamets = 0;
                if (event.flag) {
                    g_loadedSaveInitSent = false;
                    g_loadedSaveInitBlocked = false;
                    Logger::LogInfo("GameLoop: successful PostLoadGame; waiting for fresh loaded-save timestamp");
                } else {
                    g_loadedSaveInitSent = true;
                    g_loadedSaveInitBlocked = false;
                    Logger::LogWarning("GameLoop: PostLoadGame reported failure; suppressing loaded-save init");
                }
                break;
            case Type::NewGame:
                PlayerInventoryManagerFNV::Reset("native_new_game");
                PlayerInventoryManagerFNV::ForceRefresh("native_new_game", 3000);
                PlayerSurvivalManagerFNV::Reset("native_new_game");
                PlayerSurvivalManagerFNV::ForceRefresh("native_new_game", 4000);
                FalloutStatsManagerFNV::Reset("native_new_game");
                ResetRuntimeForAIActions("native_new_game", false, false);
                g_lastDynamicProfileTimerUpdate = std::chrono::steady_clock::now();
                g_dynamicProfileBlockedAt = {};
                g_dynamicProfileResumeNotBefore = {};
                g_lastDynamicProfileLoadDelayAt = {};
                g_loadedSaveInitSent = false;
                g_lastSeenGamets = 0;
                g_loadedSaveInitBlocked = false;
                break;
            case Type::ExitToMainMenu:
            case Type::ExitGame:
                PlayerInventoryManagerFNV::Reset("native_runtime_exit");
                PlayerSurvivalManagerFNV::Reset("native_runtime_exit");
                FalloutStatsManagerFNV::Reset("native_runtime_exit");
                ResetRuntimeForAIActions("native_runtime_exit", false, false);
                BeginDynamicProfileTimerBlock("runtime exit");
                g_loadedSaveInitSent = false;
                g_lastSeenGamets = 0;
                g_loadedSaveInitBlocked = false;
                break;
            case Type::CellChanged:
                // The event flag marks a hard interior/worldspace boundary.
                if (event.flag) {
                    ResetRuntimeForAIActions("native_cell_changed", false, false);
                }
                break;
            case Type::ReloadConfig:
                MarkRuntimeConfigDirty();
                break;
            default:
                break;
        }
    }
}

void Initialize() {
    Log("GameLoop: Initializing...");
    
    // Initialize state
    g_gameState = {};
    g_conversationActive = false;
    g_conversationIsNarrator = false;
    g_conversationPartner.clear();
    g_conversationPartnerFormId = 0;
    g_voiceInputActive = false;
    g_loadedSaveInitSent = false;
    g_lastSeenGamets = 0;
    g_loadedSaveInitBlocked = false;
    {
        std::lock_guard<std::mutex> lock(g_conversationCooldownMutex);
        g_conversationCooldownByFormId.clear();
        g_conversationCooldownByName.clear();
    }
    
    g_lastUpdateTime = std::chrono::steady_clock::now();
    g_updateAccumulator = 0.0f;
    g_lastDynamicProfileTimerUpdate = {};
    g_dynamicProfileBlockedAt = {};
    g_dynamicProfileResumeNotBefore = {};
    g_lastDynamicProfileLoadDelayAt = {};
    g_lastModeSelectionPoll = {};
    g_lastDynamicProfileSelectionPoll = {};
    g_lastLegacyToolPoll = {};
    g_lastTextInputPoll = {};
    g_lastRuntimeConfigFallbackPoll = {};
    g_lastRpgEventPoll = {};
    g_runtimeConfigDirty.store(false);
    g_runtimeConfigDirtyTick.store(0);
    g_lastBoredEventTimerUpdate = g_lastUpdateTime;
    g_lastBoredBlockingActivityTime = g_lastUpdateTime;
    g_lastUpdatePerfSummaryTime = {};
    g_updatePerfAggregates.clear();
    g_updatePerfTickCount = 0;
    DeleteFileIfExists(kRuntimeConfigReloadPath);
    
    ApplyModeIndex(Config::currentModeIndex, false);
    PlayerInventoryManagerFNV::Initialize();
    PlayerSurvivalManagerFNV::Initialize();
    FalloutStatsManagerFNV::Initialize();
    TradeManager::Initialize();
    LoadedPluginsFNV::RequestSync();
    
    Log("GameLoop: Initialized (native frame and response queue pump)");
}

void Shutdown() {
    Log("GameLoop: Shutting down...");
    
    VoiceRecorder::Shutdown();
    TradeManager::Shutdown();
    PlayerInventoryManagerFNV::Shutdown();
    PlayerSurvivalManagerFNV::Shutdown();
    FalloutStatsManagerFNV::Shutdown();
    PipVisionManager::Shutdown();
    
    // Stop any active conversation
    if (g_conversationActive) {
        StopConversation();
    }
    
    Log("GameLoop: Shutdown complete");
}

void Update(float deltaTime) {
    const auto frameProfileStart = std::chrono::steady_clock::now();

    // Update subsystems
    ProfileUpdateSubsystem("ProcessNativeRuntimeEvents", []() { ProcessNativeRuntimeEvents(); });
    ProfileUpdateSubsystem("ResponseQueueFNV::DispatchPending", []() {
        ResponseQueueFNV::DispatchPending(64);
    });
    ProfileUpdateSubsystem("InputManager::Update", []() { InputManager::Update(); });
    ProfileUpdateSubsystem("UpdateOpenMicMonitoringState", []() { UpdateOpenMicMonitoringState(); });
    ProfileUpdateSubsystem("TargetManager::Update", []() { TargetManager::Update(); });
    ProfileUpdateSubsystem("RefreshGameState", []() { RefreshGameState(); });
    if (g_gameState.isInGame && !g_gameState.isLoading) {
        PlaythroughNotices::Notice notice;
        if (PlaythroughNotices::Take(notice)) IngameNotifier::Notify("[DIALECTIC] " + notice.text,
            notice.error ? IngameNotifier::Level::Error : IngameNotifier::Level::Info);
    }
    if (ShouldPoll(g_lastRuntimeConfigFallbackPoll, std::chrono::seconds(1))) {
        ProfileUpdateSubsystem("PollRuntimeConfigReloadFallback", []() { PollRuntimeConfigReloadFallback(); });
    }
    ProfileUpdateSubsystem("ApplyPendingRuntimeConfigReload", []() { ApplyPendingRuntimeConfigReload(); });
    ProfileUpdateSubsystem("MaybeSendLoadedSaveInit", []() { MaybeSendLoadedSaveInit(); });
    ProfileUpdateSubsystem("WorldContextFNV::Update", []() { WorldContextFNV::Update(); });
    ProfileUpdateSubsystem("NearbyActorsFNV::Update", []() { NearbyActorsFNV::Update(); });
    ProfileUpdateSubsystem("AutoGreetingFNV::Update", []() { AutoGreetingFNV::Update(); });
    ProfileUpdateSubsystem("ActivityStatusFNV::Update", []() { ActivityStatusFNV::Update(); });
    ProfileUpdateSubsystem("NearbyItemsFNV::Update", []() { NearbyItemsFNV::Update(); });
    ProfileUpdateSubsystem("NearbyPoiFNV::Update", []() { NearbyPoiFNV::Update(); });
    ProfileUpdateSubsystem("QuestJournalFNV::Update", []() { QuestJournalFNV::Update(); });
    ProfileUpdateSubsystem("PlayerInventoryManagerFNV::Update", []() { PlayerInventoryManagerFNV::Update(); });
    ProfileUpdateSubsystem("PlayerSurvivalManagerFNV::Update", []() { PlayerSurvivalManagerFNV::Update(); });
    ProfileUpdateSubsystem("FalloutStatsManagerFNV::Update", []() { FalloutStatsManagerFNV::Update(); });
    ProfileUpdateSubsystem("ActionManager::Update", []() { ActionManager::Update(); });
    ProfileUpdateSubsystem("TradeManager::Update", []() { TradeManager::Update(); });
    ProfileUpdateSubsystem("UpdateDynamicProfileTimer", []() { UpdateDynamicProfileTimer(); });
    ProfileUpdateSubsystem("UpdateBoredEventTimer", []() { UpdateBoredEventTimer(); });
    if (ShouldPoll(g_lastDynamicProfileSelectionPoll, std::chrono::milliseconds(100))) {
        ProfileUpdateSubsystem("PollDynamicProfileSelection", []() { PollDynamicProfileSelection(); });
    }
    if (ShouldPoll(g_lastModeSelectionPoll, std::chrono::milliseconds(100))) {
        ProfileUpdateSubsystem("PollModeSelection", []() { PollModeSelection(); });
    }
    if (ShouldPoll(g_lastLegacyToolPoll, std::chrono::seconds(1))) {
        ProfileUpdateSubsystem("MaybeSyncRuntimeStateFromServer", []() {
            MaybeSyncRuntimeStateFromServer(false);
        });
        ProfileUpdateSubsystem("PollVoiceSampleToolRequest", []() { PollVoiceSampleToolRequest(); });
        ProfileUpdateSubsystem("PollLegacyDynamicProfileToolRequests", []() { PollLegacyDynamicProfileToolRequests(); });
    }
    ProfileUpdateSubsystem("ProcessDialogueCaptureBridge", []() { ProcessDialogueCaptureBridge(); });
    if (ShouldPoll(g_lastRpgEventPoll, std::chrono::milliseconds(100))) {
        ProfileUpdateSubsystem("ProcessRpgEventBridge", []() { ProcessRpgEventBridge(); });
    }
    if (g_textInputMenuPending.load() &&
        ShouldPoll(g_lastTextInputPoll, std::chrono::milliseconds(50))) {
        ProfileUpdateSubsystem("ProcessTextInputBridge", []() { ProcessTextInputBridge(); });
    }
    
    // Process input actions
    if (InputManager::IsActionTriggered(InputManager::HotkeyAction::StopTalking)) {
        Logger::LogInfo("GameLoop: Halt AI Actions hotkey pressed through native input");
        HaltAIActionsNow();
    }

    if (InputManager::IsActionTriggered(InputManager::HotkeyAction::ManualActivateNPC)) {
        Logger::LogInfo("GameLoop: ManualActivateNPC hotkey pressed");
        ActivationManager::ActivateCurrentTarget(ActivationManager::ActivationSource::Manual);
    }

    ProfileUpdateSubsystem("ActivationManager::Update", []() { ActivationManager::Update(); });

    const auto textGestures = InputManager::ConsumeChatGestures(InputManager::HotkeyAction::TalkToNPC);
    if ((textGestures & InputManager::Hold) != 0) {
        RequestChatHotkeyWaitHere();
    }
    if ((textGestures & InputManager::Tap) != 0) {
        Logger::LogInfo("GameLoop: Chatbox hotkey tapped");
        RequestTextInputMenuOpen();
    }

    if (InputManager::IsActionTriggered(InputManager::HotkeyAction::PipVision, 60)) {
        Logger::LogInfo("GameLoop: PipVision hotkey pressed");
        PipVisionManager::BeginHotkeyPress();
    }
    if (InputManager::IsActionReleased(InputManager::HotkeyAction::PipVision)) {
        PipVisionManager::EndHotkeyPress();
    }
    ProfileUpdateSubsystem("PipVisionManager::Update", []() { PipVisionManager::Update(); });
    
    if (InputManager::IsActionTriggered(InputManager::HotkeyAction::DialecticControl)) {
        Logger::LogInfo("GameLoop: DialecticControl hotkey pressed");
        RequestDialecticControlMenuOpen();
    }

    if (InputManager::IsActionTriggered(InputManager::HotkeyAction::OpenMicMute)) {
        Config::openMicMuted = !Config::openMicMuted;
        Config::WriteCustomINIValue(
            "OpenMic",
            "Muted",
            Config::openMicMuted ? "1" : "0");
        UpdateOpenMicMonitoringState();
        Logger::LogInfo("GameLoop: Open mic mute toggled muted=%d", Config::openMicMuted ? 1 : 0);
        Console::Print(Config::openMicMuted ? "[DIALECTIC] Open mic muted" : "[DIALECTIC] Open mic unmuted");
    }
    
    const auto voiceGestures = InputManager::ConsumeChatGestures(InputManager::HotkeyAction::ToggleVoice);
    if ((voiceGestures & InputManager::Tap) != 0) {
        // Clear speech and late replies, not NPC packages, dialogue history or the selected partner.
        SpeakManager::CancelDialogueTurn("voice_hotkey_tap", false, true);
        ResetBoredEventTimer("voice hotkey tap");
        Console::Print("[DIALECTIC] Stopped all dialogue");
    }
    if ((voiceGestures & InputManager::DoubleTap) != 0) {
        RequestChatHotkeyWaitHere();
    }
    // A release processed after a slow frame may identify a hold, but must not start a late recording.
    if ((voiceGestures & InputManager::Hold) != 0 &&
        InputManager::IsActionHeld(InputManager::HotkeyAction::ToggleVoice)) {
        if (g_voiceInputActive) {
            Console::Print("[DIALECTIC] Recording...");
        } else {
            ClearConversationIfPartnerLeftScene("voice input");
            if (!g_conversationActive) {
                Logger::LogInfo("GameLoop: Voice hotkey pressed without active conversation, trying current target");
                if (ShouldRouteToNarrator("", true)) {
                    StartNarratorConversation("voice input sky pitch");
                } else {
                    const ConversationStartResult startResult = TryStartConversationFromCurrentTarget();
                    if (startResult != ConversationStartResult::Started) {
                        if (startResult == ConversationStartResult::NoTarget) {
                            Console::Print("[DIALECTIC] Target an NPC");
                        }
                        return;
                    }
                }
            } else if (!g_conversationIsNarrator &&
                       IsConversationTargetOnCooldown(g_conversationPartnerFormId, g_conversationPartner, true)) {
                g_conversationActive = false;
                g_conversationIsNarrator = false;
                g_conversationPartner.clear();
                g_conversationPartnerFormId = 0;
                return;
            } else if (ShouldRouteToNarrator("", true) && !g_conversationIsNarrator) {
                StartNarratorConversation("voice input sky pitch");
            } else if (!g_conversationIsNarrator) {
                const FreshTargetResult crosshairResult = TrySetCurrentTargetFromCrosshair(false);
                if (crosshairResult == FreshTargetResult::Blocked) {
                    return;
                }
                if (crosshairResult == FreshTargetResult::Ready) {
                    UpdateConversationPartnerFromCurrentTarget("voice input crosshair");
                }
            }
            StartVoiceInput();
        }
    }
    
    // Keep active playback systems close to the flat-screen timing while
    // leaving the heavier queue processing on the normal periodic cadence.
    ProfileUpdateSubsystem("SpeakManager::UpdatePlaybackFrame", []() { SpeakManager::UpdatePlaybackFrame(); });

    // Accumulate time for periodic updates
    g_updateAccumulator += deltaTime;
    if (g_updateAccumulator >= UPDATE_INTERVAL) {
        g_updateAccumulator = 0.0f;
        
        // Process speak queue. Responses may arrive after the conversation flag
        // drops, but HTTPManager has already filtered stale/cancelled responses.
        ProfileUpdateSubsystem("SpeakManager::ProcessQueue", []() { SpeakManager::ProcessQueue(); });
        ProfileUpdateSubsystem("LoadedPluginsFNV::Update", []() { LoadedPluginsFNV::Update(); });
        ProfileUpdateSubsystem("WorldDataSyncFNV::Update", []() { WorldDataSyncFNV::Update(); });
        ProfileUpdateSubsystem("DialecticInitialization::Update", []() { DialecticInitialization::Update(); });
        ProfileUpdateSubsystem("SpatialSnapshotManagerFNV::Update", []() {
            SpatialSnapshotManagerFNV::Update(1);
        });
    }
    
    const long long frameElapsedUs = ElapsedUsSince(frameProfileStart);
    if (frameElapsedUs >= kPerfSlowFrameUs) {
        Logger::LogInfo("[PERF] GameLoop frame slow elapsed_ms=%.3f delta=%.4f conversation=%d voice=%d speaking=%d",
            static_cast<double>(frameElapsedUs) / 1000.0,
            deltaTime,
            g_conversationActive ? 1 : 0,
            g_voiceInputActive.load() ? 1 : 0,
            SpeakManager::IsSpeaking() ? 1 : 0);
    }
    MaybeLogUpdatePerfSummary(frameElapsedUs);
}

const GameState& GetGameState() {
    return g_gameState;
}

bool IsCombatDialogueAllowed(uint32_t actorFormId) {
    if (Config::enableCombatDialogue) {
        return true;
    }

    bool playerInCombat = g_gameState.isInCombat;
    const RuntimeSnapshot::GameState state = RuntimeSnapshot::GetGameState();
    if (state.valid) {
        playerInCombat = playerInCombat || state.inCombat;
    }
    if (playerInCombat) {
        return false;
    }

    if (actorFormId != 0) {
        RuntimeSnapshot::ActorState actor;
        if (RuntimeSnapshot::TryGetActor(actorFormId, actor) && actor.inCombat) {
            return false;
        }
    }
    return true;
}

static bool EnforceCombatDialogueGate(uint32_t actorFormId, const char* source, bool notify) {
    if (IsCombatDialogueAllowed(actorFormId)) {
        return true;
    }

    Logger::LogInfo("GameLoop: Blocked %s AI dialogue while combat dialogue is disabled actor=0x%08X",
        source ? source : "unknown",
        actorFormId);
    if (notify) {
        Console::Print("[DIALECTIC] AI dialogue is disabled during combat");
    }
    return false;
}

void HaltAIActionsNow() {
    Log("GameLoop: Halt AI Actions requested");
    Console::Print("[DIALECTIC] Halting AI actions");

    const int haltedActors = ResetRuntimeForAIActions("halt_ai_actions", true, true);
    Logger::LogInfo("GameLoop: Halt AI Actions completed for %d actor(s)", haltedActors);
}

static bool BeginConversationWithActor(uint32_t actorFormId,
                                       const std::string& actorName,
                                       bool notify) {
    if (actorFormId == 0 || actorFormId == 0x00000014 || actorName.empty()) {
        Logger::LogWarning("GameLoop: Cannot start exact conversation - invalid actor");
        if (notify) {
            Console::Print("[DIALECTIC] Target is not a valid NPC");
        }
        return false;
    }

    if (!EnforceCombatDialogueGate(actorFormId, "conversation_start", notify)) {
        return false;
    }

    if (!IsConversationTargetEligible(actorFormId, actorName, notify)) {
        return false;
    }

    if (!IsConversationTargetWithinModeRadius(actorFormId, actorName, notify)) {
        return false;
    }

    if (IsConversationTargetOnCooldown(actorFormId, actorName, notify)) {
        return false;
    }

    TargetManager::SetCurrentTarget(actorFormId, actorName, true);
    g_conversationPartner = actorName;
    g_conversationPartnerFormId = actorFormId;
    g_conversationActive = true;
    g_conversationIsNarrator = false;
    RefreshPlayerNameFromGame();
    SpeakManager::GuardActorForPendingDialogue(g_conversationPartnerFormId, g_conversationPartner);
    
    Log("GameLoop: Started conversation with %s (0x%08X)", 
        g_conversationPartner.c_str(), g_conversationPartnerFormId.load());
    
    std::ostringstream npcId;
    npcId << "0x" << std::hex << std::setw(8) << std::setfill('0') << g_conversationPartnerFormId << std::dec;

    const std::string playerName = Config::playerName.empty() ? "Player" : Config::playerName;
    std::ostringstream payload;
    payload << "{"
            << "\"schema\":\"dialectic.conversation.v1\","
            << "\"action\":\"conversation_start\","
            << "\"npc\":\"" << HTTPManager::EscapeJson(g_conversationPartner) << "\","
            << "\"npc_id\":\"" << npcId.str() << "\","
            << "\"player\":\"" << HTTPManager::EscapeJson(playerName) << "\","
            << "\"target\":{\"name\":\"" << HTTPManager::EscapeJson(g_conversationPartner) << "\",\"refid\":\"" << npcId.str() << "\"},"
            << "\"player_actor\":{\"name\":\"" << HTTPManager::EscapeJson(playerName) << "\"},"
            << "\"game\":\"fnv\""
            << "}";
    
    HTTPManager::SendEvent("conversation_start", payload.str());
    ResetBoredEventTimer("conversation start");
    return true;
}

bool StartConversationForActor(uint32_t actorFormId, const std::string& actorName, bool notify) {
    RuntimeSnapshot::GameState gameState;
    RuntimeSnapshot::ActorState actor;
    if (!RuntimeSnapshot::TryGetFreshGameState(gameState, std::chrono::milliseconds(500)) ||
        !RuntimeSnapshot::TryGetActor(actorFormId, actor) ||
        actor.deleted || actor.dead || !actor.loaded3D ||
        !RuntimeSnapshot::IsActorInScene(actor, gameState)) {
        Logger::LogWarning("GameLoop: Exact conversation target is not in the current scene actor=0x%08X",
            actorFormId);
        if (notify) {
            Console::Print("[DIALECTIC] Target is not a valid NPC");
        }
        return false;
    }

    const std::string resolvedName = actor.name.empty() ? actorName : actor.name;
    return BeginConversationWithActor(actorFormId, resolvedName, notify);
}

bool StartConversation() {
    const auto& target = TargetManager::GetCurrentTarget();
    if (!target.isActor || !target.isAlive) {
        Log("GameLoop: Cannot start conversation - invalid target");
        Console::Print("[DIALECTIC] Target is not a valid NPC");
        return false;
    }
    return BeginConversationWithActor(target.formId, target.name, true);
}

static bool ResolveExternalEventActor(uint32_t actorFormId,
                                      RuntimeSnapshot::ActorState& actor,
                                      RuntimeSnapshot::GameState& gameState) {
    if (actorFormId == 0 || actorFormId == 0x00000014 ||
        !RuntimeSnapshot::TryGetFreshGameState(gameState, std::chrono::milliseconds(500)) ||
        !gameState.inGame || gameState.loadingMenuOpen ||
        !RuntimeSnapshot::TryGetActor(actorFormId, actor) ||
        actor.deleted || actor.dead || !actor.loaded3D ||
        !RuntimeSnapshot::IsActorInScene(actor, gameState) ||
        actor.name.empty() || NPCDetector::IsExcluded(actorFormId, actor.name) ||
        !IsConversationTargetEligible(actorFormId, actor.name, false, true, actor.creature)) {
        Logger::LogWarning("[xNVSE event API] actor rejected by exact scene/eligibility gate actor=0x%08X",
            actorFormId);
        return false;
    }
    return true;
}

static bool ExternalGeneratedSpeechAllowed(uint32_t actorFormId,
                                           const RuntimeSnapshot::GameState& gameState) {
    if (gameState.paused || gameState.inMenu || gameState.dialogueMenuOpen ||
        IsTextInputMenuActiveOrRecentlyClosed()) {
        Logger::LogInfo("[xNVSE event API] generated speech rejected because a menu/dialogue is active actor=0x%08X",
            actorFormId);
        return false;
    }
    if (!EnforceCombatDialogueGate(actorFormId, "external_event", false)) {
        return false;
    }
    std::string activityReason;
    if (!ActivityStatusFNV::IsAutomaticDialogueAllowed(actorFormId, &activityReason)) {
        Logger::LogInfo("[xNVSE event API] generated speech rejected by activity gate actor=0x%08X reason=%s",
            actorFormId, activityReason.c_str());
        return false;
    }

    const SpeakManager::QueueStatus speech = SpeakManager::GetQueueStatus();
    const HTTPManager::QueueStatus http = HTTPManager::GetQueueStatus();
    if (speech.isProcessing || speech.isPlaying || speech.currentPlaybackLineActive ||
        speech.dialogueLinesQueued > 0 || speech.ttsDownloadsInProgress > 0 ||
        speech.ttsTasksPending > 0 || speech.ttsTasksActive > 0 ||
        speech.preparedAudioCount > 0 || http.streamInProgress ||
        http.pendingHttpTasks > 0 || http.activeHttpTasks > 0 ||
        http.httpResponsesQueued > 0) {
        Logger::LogInfo("[xNVSE event API] generated speech rejected because dialogue pipeline is busy actor=0x%08X",
            actorFormId);
        return false;
    }
    return true;
}

static std::string BuildExternalSpeechPayload(const RuntimeSnapshot::ActorState& actor,
                                              const std::string& request,
                                              const std::string& instruction,
                                              const std::string& audienceSnapshot) {
    RefreshPlayerNameFromGame();
    const std::string playerName = Config::playerName.empty() ? "Player" : Config::playerName;
    std::ostringstream payload;
    payload << "{"
            << "\"schema\":\"dialectic.external_request.v1\","
            << "\"request\":\"" << HTTPManager::EscapeJson(request) << "\","
            << "\"npc\":\"" << HTTPManager::EscapeJson(actor.name) << "\","
            << "\"npc_id\":\"" << FormatFormIdJsonValue(actor.formId) << "\","
            << "\"speaker_formid\":\"" << FormatFormIdJsonValue(actor.formId) << "\","
            << "\"player\":\"" << HTTPManager::EscapeJson(playerName) << "\",";
    if (!instruction.empty()) {
        payload << "\"instruction\":\"" << HTTPManager::EscapeJson(instruction) << "\",";
    }
    payload << "\"location\":\"" << HTTPManager::EscapeJson(Misc::GetPlayerLocation()) << "\","
            << "\"audience_snapshot\":" << audienceSnapshot << ","
            << "\"game\":\"fnv\""
            << "}";
    return payload.str();
}

bool RequestExternalExactSpeech(uint32_t actorFormId, const std::string& text) {
    RuntimeSnapshot::ActorState actor;
    RuntimeSnapshot::GameState gameState;
    const std::string exactText = TrimInput(text);
    if (exactText.empty() || exactText.size() > 1000 ||
        !ResolveExternalEventActor(actorFormId, actor, gameState)) {
        Logger::LogWarning("[xNVSE event API] SpeakExact rejected actor=0x%08X chars=%zu",
            actorFormId, exactText.size());
        return false;
    }
    return HTTPManager::QueueNpcTtsPlay(actorFormId, actor.name, exactText);
}

bool RequestExternalComment(uint32_t actorFormId) {
    RuntimeSnapshot::ActorState actor;
    RuntimeSnapshot::GameState gameState;
    if (!ResolveExternalEventActor(actorFormId, actor, gameState) ||
        !ExternalGeneratedSpeechAllowed(actorFormId, gameState)) {
        return false;
    }
    const std::string audience = BuildAudienceSnapshotJson("external_comment");
    HTTPManager::SendEvent("external_comment",
        BuildExternalSpeechPayload(actor, "comment", "", audience), audience);
    return true;
}

bool RequestExternalReaction(uint32_t actorFormId, const std::string& instruction) {
    RuntimeSnapshot::ActorState actor;
    RuntimeSnapshot::GameState gameState;
    const std::string cleanInstruction = TrimInput(instruction);
    if (cleanInstruction.empty() || cleanInstruction.size() > 1000 ||
        !ResolveExternalEventActor(actorFormId, actor, gameState) ||
        !ExternalGeneratedSpeechAllowed(actorFormId, gameState)) {
        Logger::LogWarning("[xNVSE event API] React rejected actor=0x%08X chars=%zu",
            actorFormId, cleanInstruction.size());
        return false;
    }
    const std::string audience = BuildAudienceSnapshotJson("external_reaction");
    HTTPManager::SendEvent("external_reaction",
        BuildExternalSpeechPayload(actor, "reaction", cleanInstruction, audience), audience);
    return true;
}

bool RequestExternalQuestion(uint32_t actorFormId, const std::string& question) {
    RuntimeSnapshot::ActorState actor;
    RuntimeSnapshot::GameState gameState;
    const std::string cleanQuestion = TrimInput(question);
    if (cleanQuestion.empty() || cleanQuestion.size() > 1000 ||
        !ResolveExternalEventActor(actorFormId, actor, gameState)) {
        Logger::LogWarning("[xNVSE event API] Ask rejected actor=0x%08X chars=%zu",
            actorFormId, cleanQuestion.size());
        return false;
    }
    if (g_conversationActive &&
        (g_conversationIsNarrator || g_conversationPartnerFormId != actorFormId)) {
        Logger::LogInfo("[xNVSE event API] Ask rejected because another actor owns the conversation actor=0x%08X owner=0x%08X",
            actorFormId, g_conversationPartnerFormId.load());
        return false;
    }
    if (!g_conversationActive && !StartConversationForActor(actorFormId, actor.name, false)) {
        return false;
    }
    SendPlayerMessage(cleanQuestion);
    return true;
}

bool RequestTextInputMenuOpenForActor(uint32_t actorFormId, const std::string& actorName) {
    RuntimeSnapshot::ActorState actor;
    RuntimeSnapshot::GameState gameState;
    if (!ResolveExternalEventActor(actorFormId, actor, gameState)) {
        return false;
    }
    if (g_conversationActive &&
        (g_conversationIsNarrator || g_conversationPartnerFormId != actorFormId)) {
        Logger::LogInfo("[xNVSE event API] OpenPrompt rejected because another actor owns the conversation actor=0x%08X owner=0x%08X",
            actorFormId, g_conversationPartnerFormId.load());
        return false;
    }
    if (!EnforceCombatDialogueGate(actorFormId, "external_open_prompt", false) ||
        IsConversationTargetOnCooldown(actorFormId, actor.name, false)) {
        return false;
    }

    const DWORD now = GetTickCount();
    const DWORD blockUntil = g_textInputMenuBlockUntilTick.load();
    if (IsTickBefore(now, blockUntil) || g_textInputMenuPending.load()) {
        Logger::LogInfo("[xNVSE event API] OpenPrompt rejected because text input is pending/debounced");
        return false;
    }

    TargetManager::SetCurrentTarget(actorFormId, actor.name.empty() ? actorName : actor.name, true);
    if (!WriteTextInputTargetHint(actorFormId, actor.name.empty() ? actorName : actor.name)) {
        return false;
    }
    WriteToolBridgeSignal(kOpenTextInputMenuPath, "text_input");
    g_textInputMenuPending.store(true);
    g_textInputMenuRequestTick.store(now);
    Logger::LogInfo("[xNVSE event API] OpenPrompt prepared exact actor=0x%08X name=%s",
        actorFormId, actor.name.c_str());
    return true;
}

void ApplyEndConversationCooldown(uint32_t formId, const std::string& name) {
    RegisterConversationCooldown(formId, name);

    const bool sameActiveConversation =
        g_conversationActive &&
        !g_conversationIsNarrator &&
        ((formId != 0 && formId == g_conversationPartnerFormId) ||
         (!name.empty() && EqualsIgnoreCase(name, g_conversationPartner)));

    if (sameActiveConversation) {
        StopConversation();
    }
}

void StopConversation() {
    if (!g_conversationActive) return;
    
    Log("GameLoop: Ending conversation with %s", g_conversationPartner.c_str());
    Console::Print("[DIALECTIC] Conversation ended");
    
    // Stop any ongoing speech
    SpeakManager::CancelDialogueTurn("conversation_stop", true, false);
    
    // Notify server
    std::ostringstream payload;
    payload << "{"
            << "\"schema\":\"dialectic.conversation.v1\","
            << "\"action\":\"conversation_end\","
            << "\"npc\":\"" << HTTPManager::EscapeJson(g_conversationPartner) << "\","
            << "\"game\":\"fnv\""
            << "}";
    HTTPManager::SendEvent("conversation_end", payload.str());
    ResetBoredEventTimer("conversation end");
    
    g_conversationActive = false;
    g_conversationIsNarrator = false;
    g_conversationPartner.clear();
    g_conversationPartnerFormId = 0;
}

void SendPlayerMessage(const std::string& message) {
    if (!g_conversationActive) {
        Log("GameLoop: Cannot send message - no active conversation");
    Console::Print("[DIALECTIC] No active conversation");
        return;
    }

    if (!EnforceCombatDialogueGate(g_conversationPartnerFormId, "player_message", true)) {
        return;
    }

    if (!g_conversationIsNarrator &&
        !IsConversationTargetEligible(g_conversationPartnerFormId, g_conversationPartner, true)) {
        g_conversationActive = false;
        g_conversationPartner.clear();
        g_conversationPartnerFormId = 0;
        return;
    }
    if (!g_conversationIsNarrator &&
        !IsConversationTargetWithinModeRadius(g_conversationPartnerFormId, g_conversationPartner, true)) {
        return;
    }
    
    ResetBoredEventTimer("player message");

    const std::uint64_t turnGeneration = RuntimeGeneration::Advance("player_interruption");
    RuntimeSnapshot::RebaseGeneration(turnGeneration);
    GameThreadDispatcher::CancelAll("player_interruption");
    TaskManager::CancelOlderThanGeneration(turnGeneration);

    const bool injectionLogMode =
        !g_conversationIsNarrator && EqualsIgnoreCase(Config::currentMode, "INJECTION_LOG");
    const bool injectionChatMode =
        !g_conversationIsNarrator && EqualsIgnoreCase(Config::currentMode, "INJECTION_CHAT");
    const bool directorMode =
        !g_conversationIsNarrator && EqualsIgnoreCase(Config::currentMode, "DIRECTOR");
    const bool cheatMode =
        !g_conversationIsNarrator && EqualsIgnoreCase(Config::currentMode, "CHEATMODE");
    const bool injectionMode = injectionLogMode || injectionChatMode;
    const bool privateConversationMode = !g_conversationIsNarrator && IsPrivateConversationMode();
    const bool skipPlayerTtsMode = injectionMode || directorMode || cheatMode;

    SpeakManager::CancelDialogueTurn("player_input", true, true);
    if (!g_conversationIsNarrator) {
        SpeakManager::GuardActorForPendingDialogue(g_conversationPartnerFormId, g_conversationPartner);
    }
    Log("GameLoop: Player says to %s: %s", g_conversationPartner.c_str(), message.c_str());
    Console::Print("[Player] %s", message.c_str());
    RefreshPlayerNameFromGame();
    bool playerTtsQueued = false;
    if (!skipPlayerTtsMode) {
        SpeakManager::BeginPlayerInputTtsGate();
        playerTtsQueued = HTTPManager::QueuePlayerTtsPlay(message);
        Log("GameLoop: Priority player TTS async queued=%d before inputtext", playerTtsQueued ? 1 : 0);
    } else {
        Log("GameLoop: Skipping player TTS for mode %s", Config::currentMode.c_str());
    }
    if (!g_conversationIsNarrator && g_conversationPartnerFormId != 0) {
        const bool metadataOk = AgentManager::RefreshActorMetadataForPrompt(
            g_conversationPartnerFormId,
            g_conversationPartner,
            900);
        Log("GameLoop: Prompt metadata refresh for %s (0x%08X) ok=%d",
            g_conversationPartner.c_str(),
            g_conversationPartnerFormId.load(),
            metadataOk ? 1 : 0);
    }
    if (Config::worldContextSendBeforePlayerInput) {
        WorldContextFNV::SendNow(true);
    }
    if (Config::nearbyActorsSendBeforePlayerInput) {
        NearbyActorsFNV::SendNow(true);
    }
    if (Config::activityStatusSendBeforePlayerInput) {
        ActivityStatusFNV::SendNow(true);
    }
    if (Config::nearbyItemsSendBeforePlayerInput) {
        NearbyItemsFNV::SendNow(true);
    }
    if (Config::pointsOfInterestSendBeforePlayerInput) {
        NearbyPoiFNV::SendNow(true);
    }
    const bool stealthPlayerInput = !g_conversationIsNarrator && IsStealthPlayerInputActive();
    const char* playerInputEventType = g_conversationIsNarrator
        ? "narrator_inputtext"
        : (stealthPlayerInput ? "inputtext_s" : "inputtext");
    const bool targetOnlyConversationMode =
        !g_conversationIsNarrator && EqualsIgnoreCase(Config::currentMode, "WHISPER");
    const std::string audienceSnapshot = g_conversationIsNarrator
        ? BuildPrivateNarratorAudienceSnapshotJson()
        : ((injectionMode || targetOnlyConversationMode)
            ? BuildTargetOnlyAudienceSnapshotJson(privateConversationMode)
            : BuildAudienceSnapshotJson(EqualsIgnoreCase(Config::currentMode, "CLOSE") ? "player_close" : ""));
    SpeakManager::SetPlayerTurnAudience(ExtractPeopleFromAudienceSnapshotJson(audienceSnapshot));
    Log("GameLoop: Audience snapshot for player input type=%s stealth=%d distanceMultiplier=%.3f: %s",
        playerInputEventType,
        stealthPlayerInput ? 1 : 0,
        GetPlayerSpeechDistanceMultiplier(),
        audienceSnapshot.c_str());
    
    std::ostringstream npcId;
    npcId << "0x" << std::hex << std::setw(8) << std::setfill('0') << g_conversationPartnerFormId << std::dec;

    std::ostringstream payload;
    const std::string playerName = Config::playerName.empty() ? "Player" : Config::playerName;
    payload << "{"
            << "\"schema\":\"dialectic.input.v1\","
            << "\"npc\":\"" << HTTPManager::EscapeJson(g_conversationPartner) << "\","
            << "\"npc_id\":\"" << npcId.str() << "\","
            << "\"player\":\"" << HTTPManager::EscapeJson(playerName) << "\","
            << "\"text\":\"" << HTTPManager::EscapeJson(message) << "\","
            << "\"skip_player_tts\":" << ((skipPlayerTtsMode || playerTtsQueued) ? "true" : "false") << ","
            << "\"dialectic_mode\":\"" << HTTPManager::EscapeJson(Config::currentMode) << "\","
            << "\"mode\":\"" << HTTPManager::EscapeJson(Config::currentMode) << "\","
            << "\"target\":{\"name\":\"" << HTTPManager::EscapeJson(g_conversationPartner) << "\",\"refid\":\"" << npcId.str() << "\"},"
            << "\"player_actor\":{\"name\":\"" << HTTPManager::EscapeJson(playerName) << "\"},";
    if (g_conversationIsNarrator || privateConversationMode) {
        payload << "\"private\":true,\"listener\":\""
                << HTTPManager::EscapeJson(g_conversationIsNarrator ? "The Narrator" : g_conversationPartner)
                << "\",";
    }
    payload << "\"audience_snapshot\":" << audienceSnapshot << ","
            << "\"game\":\"fnv\""
            << "}";
    
    const bool shouldResetOneShotMode = EqualsIgnoreCase(Config::currentMode, "DIRECTOR");
    HTTPManager::SendEvent(playerInputEventType, payload.str(), audienceSnapshot);
    if (shouldResetOneShotMode) {
        ApplyModeIndex(0, false);
        Log("GameLoop: Director mode consumed one input and reset locally to Standard");
    }
}

bool IsConversationActive() {
    return g_conversationActive.load();
}

const std::string& GetConversationPartner() {
    return g_conversationPartner;
}

uint32_t GetConversationPartnerFormId() {
    return g_conversationPartnerFormId.load();
}

void StartVoiceInput() {
    StartVoiceInputInternal(false);
}

static void StartVoiceInputInternal(bool openMicTriggered) {
    if (g_voiceInputActive) return;
    if (!g_conversationActive) {
        Log("GameLoop: Voice input requested without active conversation");
        if (!openMicTriggered) {
            Console::Print("[DIALECTIC] Target an NPC");
        }
        return;
    }

    if (!EnforceCombatDialogueGate(g_conversationPartnerFormId, "voice_input", !openMicTriggered)) {
        return;
    }
    
    // Get the bound voice key
    int voiceKey = openMicTriggered ? -1 : InputManager::GetHotkey(InputManager::HotkeyAction::ToggleVoice);
    if (!openMicTriggered && voiceKey <= 0) {
        Log("GameLoop: Voice input requested with no bound hotkey");
        Console::Print("[DIALECTIC] Voice key not bound");
        return;
    }

    Log("GameLoop: Starting %s voice input on device: %s",
        openMicTriggered ? "open-mic" : "push-to-talk",
        VoiceRecorder::GetCurrentRecordingDeviceName().c_str());
    Console::Print("[DIALECTIC] Recording...");

    ResetBoredEventTimer(openMicTriggered ? "open mic recording start" : "voice recording start");
    // Treat mic input as player interruption: new player intent cancels
    // queued/rechat speech before capture starts instead of after STT returns.
    SpeakManager::CancelDialogueTurn("voice_input_start", true, true);
    if (!g_conversationIsNarrator) {
        SpeakManager::GuardActorForPendingDialogue(g_conversationPartnerFormId, g_conversationPartner);
    }

    g_voiceInputActive = true;
    
    // Start recording with callback for when STT completes
    const int openMicSilenceMs = openMicTriggered
        ? static_cast<int>(std::max(0.1f, Config::openMicEndDelaySeconds) * 1000.0f)
        : -1;

    VoiceRecorder::StartRecording(voiceKey, [openMicTriggered](const std::string& transcribedText) {
        g_voiceInputActive = false;
        ResetBoredEventTimer(openMicTriggered ? "open mic recording end" : "voice recording end");

        const std::string cleanedText = TrimInput(transcribedText);
        if (!cleanedText.empty()) {
            Log("GameLoop: STT result: %s", cleanedText.c_str());
            
            // Send the transcribed text as player input
            if (ShouldRouteToNarrator(cleanedText, false) &&
                (!g_conversationActive || !g_conversationIsNarrator)) {
                StartNarratorConversation("voice input explicit phrase");
            }

            if (g_conversationActive) {
                SendPlayerMessage(cleanedText);
            } else {
                Log("GameLoop: No active conversation for voice input");
                Console::Print("[DIALECTIC] No active conversation");
            }
        } else {
            Log("GameLoop: STT returned empty result");
            if (!openMicTriggered) {
                Console::Print("[DIALECTIC] No speech detected");
            }
        }
    }, openMicSilenceMs);
}

void StopVoiceInput() {
    if (!g_voiceInputActive) return;
    
    Log("GameLoop: Voice input will stop when key is released");
    // The VoiceRecorder will automatically stop when the key is released
    // or after silence timeout, and will call the callback
}

bool IsVoiceInputActive() {
    return g_voiceInputActive;
}

} // namespace GameLoop
