#include "PipVisionManager.h"

#include "Config.h"
#include "HTTPManager.h"
#include "IngameNotifier.h"
#include "InputManager.h"
#include "Logger.h"
#include "Misc.h"
#include "RuntimeGeneration.h"
#include "RuntimeSnapshot.h"
#include "SpeakManager.h"
#include "TaskManager.h"
#include "TargetManager.h"
#include "WorldContextFNV.h"
#include "XNVSEAdapter.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace PipVisionManager {
namespace {

constexpr std::uintmax_t kMaximumCaptureBytes = 8U * 1024U * 1024U;
constexpr auto kCaptureReadyTimeout = std::chrono::seconds(6);
constexpr auto kCapturePollInterval = std::chrono::milliseconds(50);
constexpr auto kHoldThreshold = std::chrono::milliseconds(700);
constexpr auto kDoubleTapWindow = std::chrono::milliseconds(350);
constexpr int kHudSettleFrames = 2;
constexpr const char* kCaptureRelativePath = "Data\\textures\\SUPScreenshots\\Dialectic\\pipvision_capture.jpg";

std::atomic_bool g_captureInFlight{false};
bool g_hudHidden = false;
bool g_hotkeyPressActive = false;
bool g_holdCaptureTriggered = false;
bool g_singleTapPending = false;
bool g_secondTapCandidate = false;
std::chrono::steady_clock::time_point g_hotkeyPressedAt{};
std::chrono::steady_clock::time_point g_firstTapReleasedAt{};

enum class CaptureMode {
    StoreOnly,
    NpcPortrait,
    NpcDescribe,
};

struct ResponseActor {
    std::string name;
    std::uint32_t formId{0};
};

struct PendingCapture {
    bool active{false};
    int framesRemaining{0};
    std::uint64_t generation{0};
    std::filesystem::path path;
    std::string captureId;
    std::string metadata;
    CaptureMode mode{CaptureMode::StoreOnly};
    ResponseActor responseActor;
};

PendingCapture g_pendingCapture;

const char* CaptureModeName(CaptureMode mode) {
    switch (mode) {
        case CaptureMode::NpcPortrait: return "npc_portrait";
        case CaptureMode::NpcDescribe: return "npc_describe";
        default: return "store_only";
    }
}

std::string FormatFormId(std::uint32_t formId) {
    if (formId == 0) return {};
    std::ostringstream value;
    value << "0x" << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << formId;
    return value.str();
}

std::filesystem::path CapturePath() {
    const std::string runtimeDirectory = XNVSEAdapter::RuntimeDirectory();
    if (runtimeDirectory.empty()) return std::filesystem::path(kCaptureRelativePath);
    return std::filesystem::path(runtimeDirectory) / kCaptureRelativePath;
}

std::string BuildCaptureMetadata(const RuntimeSnapshot::GameState& gameState,
                                 const WorldContextFNV::Context& world,
                                 const std::string& captureId,
                                 CaptureMode mode,
                                 const ResponseActor& responseActor) {
    std::string subjectType = "scene";
    std::string subjectName;
    std::uint32_t subjectRefId = 0;
    std::uint32_t subjectBaseId = 0;

    if (gameState.crosshairFormId != 0) {
        RuntimeSnapshot::ActorState actor;
        RuntimeSnapshot::ReferenceState reference;
        if (RuntimeSnapshot::TryGetActor(gameState.crosshairFormId, actor)) {
            subjectType = "actor";
            subjectName = actor.name;
            subjectRefId = actor.formId;
            subjectBaseId = actor.baseFormId;
        } else if (RuntimeSnapshot::TryGetReference(gameState.crosshairFormId, reference)) {
            subjectType = "object";
            subjectName = reference.name;
            subjectRefId = reference.formId;
            subjectBaseId = reference.baseFormId;
        }
    }

    std::vector<std::string> nearbyNames;
    for (const RuntimeSnapshot::ActorState& actor : RuntimeSnapshot::GetActors()) {
        if (actor.name.empty() || actor.dead || actor.deleted || !actor.loaded3D ||
            !RuntimeSnapshot::IsActorInScene(actor, gameState)) {
            continue;
        }
        nearbyNames.push_back(actor.name);
        if (nearbyNames.size() >= 20) break;
    }

    const std::string location = !world.location.empty() ? world.location : gameState.cellName;
    const std::string worldspace = !world.worldspace.empty() ? world.worldspace : gameState.worldspaceName;
    const std::string cellFormId = !world.cellFormId.empty() ? world.cellFormId : FormatFormId(gameState.cellFormId);
    const std::string worldspaceFormId = !world.worldspaceFormId.empty()
        ? world.worldspaceFormId
        : FormatFormId(gameState.worldspaceFormId);

    std::ostringstream json;
    json << "{";
    json << "\"schema\":\"dialectic.visual_context.capture.v1\",";
    json << "\"capture_id\":\"" << HTTPManager::EscapeJson(captureId) << "\",";
    json << "\"game\":\"fnv\",";
    json << "\"perspective\":\"first_person\",";
    json << "\"visual_type\":\"" << subjectType << "\",";
    json << "\"interaction_mode\":\"" << CaptureModeName(mode) << "\",";
    json << "\"runtime_generation\":" << RuntimeGeneration::Current() << ",";
    json << "\"localts\":" << (Misc::GetCurrentTimeMillis() / 1000) << ",";
    json << "\"gamets\":" << (world.gamets > 0 ? world.gamets : 0) << ",";
    json << "\"player\":\"" << HTTPManager::EscapeJson(gameState.playerName) << "\",";
    json << "\"player_formid\":\"" << FormatFormId(gameState.playerFormId) << "\",";
    json << "\"location\":\"" << HTTPManager::EscapeJson(location) << "\",";
    json << "\"cell_formid\":\"" << HTTPManager::EscapeJson(cellFormId) << "\",";
    json << "\"worldspace\":\"" << HTTPManager::EscapeJson(worldspace) << "\",";
    json << "\"worldspace_formid\":\"" << HTTPManager::EscapeJson(worldspaceFormId) << "\",";
    json << "\"camera\":{";
    json << "\"x\":" << gameState.playerX << ",\"y\":" << gameState.playerY
         << ",\"z\":" << gameState.playerZ << ",\"pitch\":" << gameState.playerPitch
         << ",\"yaw\":" << gameState.playerYaw << "},";
    json << "\"subject\":{";
    json << "\"type\":\"" << subjectType << "\",";
    json << "\"name\":\"" << HTTPManager::EscapeJson(subjectName) << "\",";
    json << "\"refid\":\"" << FormatFormId(subjectRefId) << "\",";
    json << "\"baseid\":\"" << FormatFormId(subjectBaseId) << "\"},";
    json << "\"response_actor\":{";
    json << "\"name\":\"" << HTTPManager::EscapeJson(responseActor.name) << "\",";
    json << "\"refid\":\"" << FormatFormId(responseActor.formId) << "\"},";
    json << "\"nearby_actors\":[";
    for (std::size_t index = 0; index < nearbyNames.size(); ++index) {
        if (index > 0) json << ',';
        json << "\"" << HTTPManager::EscapeJson(nearbyNames[index]) << "\"";
    }
    json << "]}";
    return json.str();
}

std::string ExtractJsonString(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    const std::size_t keyPosition = json.find(needle);
    if (keyPosition == std::string::npos) return {};
    const std::size_t colonPosition = json.find(':', keyPosition + needle.size());
    if (colonPosition == std::string::npos) return {};
    const std::size_t quotePosition = json.find('"', colonPosition + 1);
    if (quotePosition == std::string::npos) return {};

    std::string value;
    bool escaped = false;
    for (std::size_t index = quotePosition + 1; index < json.size(); ++index) {
        const char character = json[index];
        if (escaped) {
            switch (character) {
                case 'n': value.push_back('\n'); break;
                case 'r': value.push_back('\r'); break;
                case 't': value.push_back('\t'); break;
                default: value.push_back(character); break;
            }
            escaped = false;
        } else if (character == '\\') {
            escaped = true;
        } else if (character == '"') {
            return value;
        } else {
            value.push_back(character);
        }
    }
    return {};
}

bool IsResponseActorStillPresent(const ResponseActor& actor) {
    RuntimeSnapshot::ActorState actorState;
    const RuntimeSnapshot::GameState gameState = RuntimeSnapshot::GetGameState();
    return actor.formId != 0 &&
        RuntimeSnapshot::TryGetActor(actor.formId, actorState) &&
        !actorState.dead && !actorState.deleted && actorState.loaded3D &&
        RuntimeSnapshot::IsActorInScene(actorState, gameState);
}

void SendNpcVisionRequest(const PendingCapture& capture, const std::string& description) {
    if (!IsResponseActorStillPresent(capture.responseActor)) {
        Logger::LogWarning("[PIPVISION] held capture response actor left the scene actor=%s refid=0x%08X",
            capture.responseActor.name.c_str(), capture.responseActor.formId);
        IngameNotifier::Notify("PipVision's nearby speaker is no longer present", IngameNotifier::Level::Warning);
        return;
    }

    const RuntimeSnapshot::GameState gameState = RuntimeSnapshot::GetGameState();
    const std::string playerName = !gameState.playerName.empty()
        ? gameState.playerName
        : (Config::playerName.empty() ? "Player" : Config::playerName);
    const std::string actorRefId = FormatFormId(capture.responseActor.formId);

    std::ostringstream audience;
    audience << "{\"people\":\"|" << HTTPManager::EscapeJson(capture.responseActor.name)
             << "|" << HTTPManager::EscapeJson(playerName)
             << "|\",\"target_only\":true,\"target_form_id\":\""
             << actorRefId << "\"}";

    std::ostringstream payload;
    payload << "{";
    payload << "\"schema\":\"dialectic.vision.v1\",";
    payload << "\"capture_id\":\"" << HTTPManager::EscapeJson(capture.captureId) << "\",";
    payload << "\"npc\":\"" << HTTPManager::EscapeJson(capture.responseActor.name) << "\",";
    payload << "\"npc_id\":\"" << actorRefId << "\",";
    payload << "\"player\":\"" << HTTPManager::EscapeJson(playerName) << "\",";
    payload << "\"text\":\"" << HTTPManager::EscapeJson(description) << "\",";
    payload << "\"target\":{\"name\":\"" << HTTPManager::EscapeJson(capture.responseActor.name)
            << "\",\"refid\":\"" << actorRefId << "\"},";
    payload << "\"player_actor\":{\"name\":\"" << HTTPManager::EscapeJson(playerName) << "\"},";
    payload << "\"audience_snapshot\":" << audience.str() << ",";
    payload << "\"game\":\"fnv\"}";

    HTTPManager::CancelPendingResponses();
    SpeakManager::CancelDialogueTurn("pipvision_vision", true, true);
    SpeakManager::GuardActorForPendingDialogue(capture.responseActor.formId, capture.responseActor.name);
    HTTPManager::SendEvent("vision", payload.str(), audience.str());
    Logger::LogInfo("[PIPVISION] queued held capture vision response actor=%s refid=0x%08X capture_id=%s",
        capture.responseActor.name.c_str(), capture.responseActor.formId, capture.captureId.c_str());
}

bool WaitForStableCapture(const std::filesystem::path& path,
                          const TaskManager::CancellationToken& token,
                          std::string& imageData) {
    const auto deadline = std::chrono::steady_clock::now() + kCaptureReadyTimeout;
    std::uintmax_t previousSize = 0;
    int stableSamples = 0;

    while (std::chrono::steady_clock::now() < deadline) {
        if (token.IsCancellationRequested()) return false;

        std::error_code error;
        if (std::filesystem::is_regular_file(path, error)) {
            const std::uintmax_t size = std::filesystem::file_size(path, error);
            if (!error && size > 0 && size <= kMaximumCaptureBytes) {
                stableSamples = size == previousSize ? stableSamples + 1 : 0;
                previousSize = size;
                if (stableSamples >= 2) {
                    std::ifstream input(path, std::ios::binary);
                    if (!input.is_open()) return false;
                    imageData.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
                    return !imageData.empty() && imageData.size() <= kMaximumCaptureBytes;
                }
            } else if (!error && size > kMaximumCaptureBytes) {
                Logger::LogWarning("[PIPVISION] capture exceeded maximum size bytes=%llu",
                    static_cast<unsigned long long>(size));
                return false;
            }
        }

        if (!token.WaitFor(kCapturePollInterval)) return false;
    }
    return false;
}

void RestoreHud() {
    if (!g_hudHidden) return;
    if (!XNVSEAdapter::ToggleNativePipVisionMenus()) {
        Logger::LogError("[PIPVISION] failed to restore Fallout HUD");
        return;
    }
    g_hudHidden = false;
    Logger::LogInfo("[PIPVISION] Fallout HUD restored");
}

void AbortPendingCapture(const char* reason, bool notify) {
    RestoreHud();
    g_pendingCapture = {};
    g_captureInFlight.store(false, std::memory_order_release);
    Logger::LogWarning("[PIPVISION] pending capture aborted reason=%s", reason ? reason : "unknown");
    if (notify) {
        IngameNotifier::Notify("PipVision capture was cancelled", IngameNotifier::Level::Warning);
    }
}

bool StartUploadTask(const PendingCapture& capture) {
    struct UploadResult {
        bool success{false};
        std::string description;
    };
    const auto result = std::make_shared<UploadResult>();

    TaskManager::Options options;
    options.type = "pipvision";
    options.key = "capture";
    options.generation = capture.generation;
    options.lane = TaskManager::Lane::Interactive;
    options.priority = true;
    options.deadlineFromEnqueue = true;
    options.timeout = std::chrono::seconds(75);
    options.coalescing = TaskManager::CoalescingPolicy::RejectIfPendingOrActive;
    options.concurrencyLimit = 1;

    const TaskManager::TaskHandle task = TaskManager::Submit(
        std::move(options),
        [capturePath = capture.path, captureId = capture.captureId, metadata = capture.metadata, result](
            const TaskManager::CancellationToken& token) {
            std::string imageData;
            if (!WaitForStableCapture(capturePath, token, imageData)) {
                if (!token.IsCancellationRequested()) {
                    Logger::LogWarning("[PIPVISION] screenshot did not become ready capture_id=%s path=%s",
                        captureId.c_str(), capturePath.string().c_str());
                    IngameNotifier::Notify("PipVision could not read the screenshot", IngameNotifier::Level::Error);
                }
                return;
            }

            Logger::LogInfo("[PIPVISION] uploading capture_id=%s bytes=%zu",
                captureId.c_str(), imageData.size());
            const std::string response = HTTPManager::UploadPipVisionImage(
                imageData, metadata, "pipvision_capture.jpg", &token);
            if (token.IsCancellationRequested()) return;

            const bool success = response.find("\"ok\":true") != std::string::npos ||
                response.find("\"ok\": true") != std::string::npos;
            if (success) {
                result->success = true;
                result->description = ExtractJsonString(response, "description");
                Logger::LogInfo("[PIPVISION] capture completed capture_id=%s response_bytes=%zu",
                    captureId.c_str(), response.size());
            } else {
                Logger::LogWarning("[PIPVISION] upload failed capture_id=%s response=%s",
                    captureId.c_str(), response.substr(0, 500).c_str());
                IngameNotifier::Notify("PipVision capture failed on the server", IngameNotifier::Level::Error);
            }
        },
        [capture, result](bool, const char*) {
            std::error_code error;
            std::filesystem::remove(capture.path, error);
            g_captureInFlight.store(false, std::memory_order_release);
            if (!result->success) return;

            if (capture.mode == CaptureMode::NpcPortrait) {
                IngameNotifier::Notify(capture.responseActor.name + " portrait updated",
                    IngameNotifier::Level::Success);
            } else {
                IngameNotifier::Notify("PipVision visual context captured", IngameNotifier::Level::Success);
            }
            if (capture.mode == CaptureMode::NpcDescribe) {
                if (result->description.empty()) {
                    Logger::LogWarning("[PIPVISION] held capture returned no description capture_id=%s",
                        capture.captureId.c_str());
                    IngameNotifier::Notify("PipVision received no scene description", IngameNotifier::Level::Error);
                    return;
                }
                SendNpcVisionRequest(capture, result->description);
            }
        });

    return static_cast<bool>(task);
}

} // namespace

bool RequestCapture(CaptureMode mode, const ResponseActor& responseActor = {}) {
    bool expected = false;
    if (!g_captureInFlight.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        IngameNotifier::Notify("PipVision is already processing a capture", IngameNotifier::Level::Warning);
        return false;
    }

    const RuntimeSnapshot::GameState gameState = RuntimeSnapshot::GetGameState();
    if (!gameState.valid || !gameState.inGame || gameState.paused || gameState.loadingMenuOpen) {
        g_captureInFlight.store(false, std::memory_order_release);
        IngameNotifier::Notify("PipVision is unavailable while the game is paused or loading", IngameNotifier::Level::Warning);
        return false;
    }

    const std::filesystem::path capturePath = CapturePath();
    std::error_code removeError;
    std::filesystem::remove(capturePath, removeError);

    const long long captureTimestamp = Misc::GetCurrentTimeMillis();
    const std::string captureId = "pv_" + std::to_string(RuntimeGeneration::Current()) + "_" +
        std::to_string(captureTimestamp);
    const std::string metadata = BuildCaptureMetadata(
        gameState, WorldContextFNV::GetCurrent(), captureId, mode, responseActor);

    if (!XNVSEAdapter::ToggleNativePipVisionMenus()) {
        g_captureInFlight.store(false, std::memory_order_release);
        IngameNotifier::Notify("PipVision could not hide the Fallout HUD", IngameNotifier::Level::Error);
        return false;
    }
    g_hudHidden = true;

    g_pendingCapture.active = true;
    g_pendingCapture.framesRemaining = kHudSettleFrames;
    g_pendingCapture.generation = RuntimeGeneration::Current();
    g_pendingCapture.path = capturePath;
    g_pendingCapture.captureId = captureId;
    g_pendingCapture.metadata = metadata;
    g_pendingCapture.mode = mode;
    g_pendingCapture.responseActor = responseActor;

    Logger::LogInfo("[PIPVISION] capture requested capture_id=%s mode=%s response_actor=%s response_refid=0x%08X crosshair=0x%08X location=%s worldspace=%s hud_settle_frames=%d",
        captureId.c_str(), CaptureModeName(mode),
        responseActor.name.c_str(), responseActor.formId,
        gameState.crosshairFormId, gameState.cellName.c_str(),
        gameState.worldspaceName.c_str(), kHudSettleFrames);
    return true;
}

bool ResolveNearestResponseActor(ResponseActor& actor) {
    const RuntimeSnapshot::GameState gameState = RuntimeSnapshot::GetGameState();
    const float maxDistance = gameState.worldspaceFormId == 0
        ? Config::distanceActivatingNpcInterior
        : Config::distanceActivatingNpcExterior;
    if (!TargetManager::FindNearestNPC(maxDistance)) return false;

    const TargetManager::TargetInfo& target = TargetManager::GetCurrentTarget();
    if (!TargetManager::HasValidNPCTarget() || target.name.empty()) return false;
    actor.name = target.name;
    actor.formId = target.formId;
    return true;
}

bool ResolveTargetedPortraitActor(ResponseActor& actor) {
    const RuntimeSnapshot::GameState gameState = RuntimeSnapshot::GetGameState();
    RuntimeSnapshot::ActorState actorState;
    if (gameState.crosshairFormId == 0 ||
        !RuntimeSnapshot::TryGetActor(gameState.crosshairFormId, actorState) ||
        actorState.name.empty() || actorState.dead || actorState.deleted || !actorState.loaded3D ||
        !RuntimeSnapshot::IsActorInScene(actorState, gameState)) {
        return false;
    }
    actor.name = actorState.name;
    actor.formId = actorState.formId;
    return true;
}

void BeginHotkeyPress() {
    if (g_hotkeyPressActive) return;
    const auto now = std::chrono::steady_clock::now();
    if (g_singleTapPending && now - g_firstTapReleasedAt > kDoubleTapWindow) {
        g_singleTapPending = false;
        RequestCapture(CaptureMode::StoreOnly);
    }
    g_hotkeyPressActive = true;
    g_holdCaptureTriggered = false;
    g_secondTapCandidate = g_singleTapPending && now - g_firstTapReleasedAt <= kDoubleTapWindow;
    g_hotkeyPressedAt = now;
}

void EndHotkeyPress() {
    if (!g_hotkeyPressActive) return;
    const bool heldCaptureTriggered = g_holdCaptureTriggered;
    const bool secondTapCandidate = g_secondTapCandidate;
    g_hotkeyPressActive = false;
    g_holdCaptureTriggered = false;
    g_secondTapCandidate = false;
    if (heldCaptureTriggered) return;

    if (secondTapCandidate) {
        g_singleTapPending = false;
        ResponseActor portraitActor;
        if (!ResolveTargetedPortraitActor(portraitActor)) {
            IngameNotifier::Notify("Target an NPC before double-tapping PipVision",
                IngameNotifier::Level::Warning);
            return;
        }
        RequestCapture(CaptureMode::NpcPortrait, portraitActor);
        return;
    }

    g_singleTapPending = true;
    g_firstTapReleasedAt = std::chrono::steady_clock::now();
}

void Update() {
    if (g_hotkeyPressActive && !g_holdCaptureTriggered) {
        if (!InputManager::IsActionHeld(InputManager::HotkeyAction::PipVision)) {
            g_hotkeyPressActive = false;
        } else if (std::chrono::steady_clock::now() - g_hotkeyPressedAt >= kHoldThreshold) {
            g_holdCaptureTriggered = true;
            g_singleTapPending = false;
            g_secondTapCandidate = false;
            ResponseActor responseActor;
            if (!ResolveNearestResponseActor(responseActor)) {
                IngameNotifier::Notify("PipVision found no nearby NPC", IngameNotifier::Level::Warning);
            } else {
                RequestCapture(CaptureMode::NpcDescribe, responseActor);
            }
        }
    }

    if (g_singleTapPending && !g_hotkeyPressActive &&
        std::chrono::steady_clock::now() - g_firstTapReleasedAt > kDoubleTapWindow) {
        g_singleTapPending = false;
        RequestCapture(CaptureMode::StoreOnly);
    }

    if (!g_pendingCapture.active) return;

    const RuntimeSnapshot::GameState gameState = RuntimeSnapshot::GetGameState();
    if (g_pendingCapture.generation != RuntimeGeneration::Current() || !gameState.valid ||
        !gameState.inGame || gameState.paused || gameState.loadingMenuOpen) {
        AbortPendingCapture("game_state_changed", false);
        return;
    }

    if (--g_pendingCapture.framesRemaining > 0) return;

    PendingCapture capture = std::move(g_pendingCapture);
    g_pendingCapture = {};
    const bool captured = XNVSEAdapter::CaptureNativePipVisionScreenshot();
    RestoreHud();

    if (!captured) {
        g_captureInFlight.store(false, std::memory_order_release);
        IngameNotifier::Notify("PipVision requires SUP NVSE 8.55 or newer", IngameNotifier::Level::Error);
        return;
    }

    if (!StartUploadTask(capture)) {
        std::error_code error;
        std::filesystem::remove(capture.path, error);
        g_captureInFlight.store(false, std::memory_order_release);
        IngameNotifier::Notify("PipVision capture queue is unavailable", IngameNotifier::Level::Error);
    }
}

void Shutdown() {
    TaskManager::CancelByType("pipvision");
    RestoreHud();
    g_pendingCapture = {};
    g_hotkeyPressActive = false;
    g_holdCaptureTriggered = false;
    g_singleTapPending = false;
    g_secondTapCandidate = false;
    g_captureInFlight.store(false, std::memory_order_release);
}

} // namespace PipVisionManager
