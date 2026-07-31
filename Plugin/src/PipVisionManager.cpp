#include "PipVisionManager.h"

#include "HTTPManager.h"
#include "IngameNotifier.h"
#include "Logger.h"
#include "Misc.h"
#include "RuntimeGeneration.h"
#include "RuntimeSnapshot.h"
#include "TaskManager.h"
#include "WorldContextFNV.h"
#include "XNVSEAdapter.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace PipVisionManager {
namespace {

constexpr std::uintmax_t kMaximumCaptureBytes = 8U * 1024U * 1024U;
constexpr auto kCaptureReadyTimeout = std::chrono::seconds(6);
constexpr auto kCapturePollInterval = std::chrono::milliseconds(50);
constexpr const char* kCaptureRelativePath = "Data\\textures\\SUPScreenshots\\Dialectic\\pipvision_capture.jpg";

std::atomic_bool g_captureInFlight{false};

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
                                 const std::string& captureId) {
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
    json << "\"nearby_actors\":[";
    for (std::size_t index = 0; index < nearbyNames.size(); ++index) {
        if (index > 0) json << ',';
        json << "\"" << HTTPManager::EscapeJson(nearbyNames[index]) << "\"";
    }
    json << "]}";
    return json.str();
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

} // namespace

bool RequestCapture() {
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

    if (!XNVSEAdapter::CaptureNativePipVisionScreenshot()) {
        g_captureInFlight.store(false, std::memory_order_release);
        IngameNotifier::Notify("PipVision requires SUP NVSE 8.55 or newer", IngameNotifier::Level::Error);
        return false;
    }

    const long long captureTimestamp = Misc::GetCurrentTimeMillis();
    const std::string captureId = "pv_" + std::to_string(RuntimeGeneration::Current()) + "_" +
        std::to_string(captureTimestamp);
    const std::string metadata = BuildCaptureMetadata(gameState, WorldContextFNV::GetCurrent(), captureId);

    TaskManager::Options options;
    options.type = "pipvision";
    options.key = "capture";
    options.generation = RuntimeGeneration::Current();
    options.lane = TaskManager::Lane::Interactive;
    options.priority = true;
    options.deadlineFromEnqueue = true;
    options.timeout = std::chrono::seconds(75);
    options.coalescing = TaskManager::CoalescingPolicy::RejectIfPendingOrActive;
    options.concurrencyLimit = 1;

    const TaskManager::TaskHandle task = TaskManager::Submit(
        std::move(options),
        [capturePath, captureId, metadata](const TaskManager::CancellationToken& token) {
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
                Logger::LogInfo("[PIPVISION] capture completed capture_id=%s response_bytes=%zu",
                    captureId.c_str(), response.size());
                IngameNotifier::Notify("PipVision visual context captured", IngameNotifier::Level::Success);
            } else {
                Logger::LogWarning("[PIPVISION] upload failed capture_id=%s response=%s",
                    captureId.c_str(), response.substr(0, 500).c_str());
                IngameNotifier::Notify("PipVision capture failed on the server", IngameNotifier::Level::Error);
            }
        },
        [capturePath](bool, const char*) {
            std::error_code error;
            std::filesystem::remove(capturePath, error);
            g_captureInFlight.store(false, std::memory_order_release);
        });

    if (!task) {
        g_captureInFlight.store(false, std::memory_order_release);
        IngameNotifier::Notify("PipVision capture queue is unavailable", IngameNotifier::Level::Error);
        return false;
    }

    Logger::LogInfo("[PIPVISION] capture requested capture_id=%s crosshair=0x%08X location=%s worldspace=%s",
        captureId.c_str(), gameState.crosshairFormId, gameState.cellName.c_str(), gameState.worldspaceName.c_str());
    IngameNotifier::Notify("PipVision capture started", IngameNotifier::Level::Info);
    return true;
}

void Shutdown() {
    TaskManager::CancelByType("pipvision");
    g_captureInFlight.store(false, std::memory_order_release);
}

} // namespace PipVisionManager
