#include "PlayerSurvivalManagerFNV.h"

#include "HTTPManager.h"
#include "Logger.h"
#include "Misc.h"
#include "RuntimeGeneration.h"
#include "RuntimeSnapshot.h"
#include "TaskManager.h"
#include "XNVSEAdapter.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>

namespace PlayerSurvivalManagerFNV {
namespace {

constexpr auto kPollInterval = std::chrono::seconds(15);
constexpr auto kHeartbeatInterval = std::chrono::seconds(60);
constexpr auto kInitialDelay = std::chrono::seconds(3);
constexpr auto kRetryDelay = std::chrono::seconds(2);
constexpr float kStageSize = 200.0f;

std::mutex g_mutex;
bool g_initialized{false};
bool g_force{false};
std::string g_reason;
std::string g_lastObservedHash;
std::string g_lastDeliveredHash;
std::chrono::steady_clock::time_point g_nextPollAt{};
std::chrono::steady_clock::time_point g_nextHeartbeatAt{};

int NeedStage(float value) {
    return std::clamp(static_cast<int>(std::floor(std::max(0.0f, value) / kStageSize)), 0, 5);
}

std::string BuildHash(const XNVSEAdapter::NativePlayerSurvivalState& state) {
    std::ostringstream output;
    output << (state.hardcoreEnabled ? 1 : 0) << '|'
           << state.playerLevel << '|'
           << std::fixed << std::setprecision(1)
           << state.hunger << '|'
           << state.dehydration << '|'
           << state.sleepDeprivation << '|'
           << state.radiation;
    return output.str();
}

std::string BuildPayload(const XNVSEAdapter::NativePlayerSurvivalState& state) {
    std::string playerName = Misc::GetPlayerName();
    if (playerName.empty()) playerName = "The Courier";

    const std::string gameTimestamp = Misc::GetGameTimeStamp();
    std::ostringstream json;
    json << '{';
    json << "\"schema\":\"dialectic.player_survival.v1\",";
    json << "\"type\":\"player_survival\",";
    json << "\"actor_name\":\"" << HTTPManager::EscapeJson(playerName) << "\",";
    json << "\"actor_type\":\"player\",";
    json << "\"refid\":\"0x00000014\",";
    json << "\"timestamp\":" << Misc::GetCurrentTimeMillis() << ',';
    json << "\"gamets\":" << (gameTimestamp.empty() ? "0" : gameTimestamp) << ',';
    json << "\"hardcore_enabled\":" << (state.hardcoreEnabled ? "true" : "false") << ',';
    if (state.playerLevel > 0) json << "\"player_level\":" << state.playerLevel << ',';
    json << "\"needs\":{";
    json << "\"hunger\":{\"value\":" << state.hunger
         << ",\"stage\":" << NeedStage(state.hunger) << "},";
    json << "\"dehydration\":{\"value\":" << state.dehydration
         << ",\"stage\":" << NeedStage(state.dehydration) << "},";
    json << "\"sleep_deprivation\":{\"value\":" << state.sleepDeprivation
         << ",\"stage\":" << NeedStage(state.sleepDeprivation) << "}";
    json << "},";
    json << "\"radiation\":{\"value\":" << state.radiation
         << ",\"stage\":" << NeedStage(state.radiation) << "}";
    json << '}';
    return json.str();
}

void ScheduleRetry(const char* reason) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized) return;
    g_force = true;
    g_reason = reason ? reason : "delivery_retry";
    g_nextPollAt = std::chrono::steady_clock::now() + kRetryDelay;
}

void QueueDelivery(std::string payload, std::string hash, std::string reason) {
    TaskManager::Options options;
    options.type = "player_survival_update";
    options.key = "player_survival";
    options.generation = RuntimeGeneration::Current();
    options.lane = TaskManager::Lane::Background;
    options.timeout = std::chrono::seconds(30);
    options.coalescing = TaskManager::CoalescingPolicy::ReplacePending;
    options.concurrencyLimit = 1;

    const TaskManager::TaskHandle task = TaskManager::Submit(
        std::move(options),
        [payload = std::move(payload), hash = std::move(hash), reason = std::move(reason)](
            const TaskManager::CancellationToken& token) {
            if (token.IsCancellationRequested()) return;
            const std::string response = HTTPManager::SendJson("gamedata.php", payload);
            if (response.empty()) {
                Logger::LogWarning(
                    "PlayerSurvivalManagerFNV: delivery failed trigger=%s; retry scheduled",
                    reason.c_str());
                ScheduleRetry("delivery_failed");
                return;
            }
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_lastDeliveredHash = hash;
            }
            Logger::LogInfo("[PLAYER_SURVIVAL_UPDATE] delivered trigger=%s", reason.c_str());
        });

    if (!task) {
        Logger::LogWarning("PlayerSurvivalManagerFNV: task queue rejected survival delivery");
        ScheduleRetry("task_rejected");
    }
}

} // namespace

void Initialize() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_initialized = true;
    g_force = true;
    g_reason = "initial_load";
    g_lastObservedHash.clear();
    g_lastDeliveredHash.clear();
    const auto now = std::chrono::steady_clock::now();
    g_nextPollAt = now + kInitialDelay;
    g_nextHeartbeatAt = now + kHeartbeatInterval;
    Logger::LogInfo("PlayerSurvivalManagerFNV: initialized poll_seconds=15 heartbeat_seconds=60");
}

void Shutdown() {
    TaskManager::CancelByType("player_survival_update");
    std::lock_guard<std::mutex> lock(g_mutex);
    g_initialized = false;
    g_force = false;
    g_reason.clear();
    g_lastObservedHash.clear();
    g_lastDeliveredHash.clear();
    g_nextPollAt = {};
    g_nextHeartbeatAt = {};
}

void Reset(const char* reason) {
    TaskManager::CancelByType("player_survival_update");
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized) return;
    g_force = false;
    g_reason = reason ? reason : "runtime_reset";
    g_lastObservedHash.clear();
    g_lastDeliveredHash.clear();
    g_nextPollAt = {};
    g_nextHeartbeatAt = {};
}

void ForceRefresh(const char* reason, int delayMs) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized) return;
    g_force = true;
    g_reason = reason ? reason : "forced_refresh";
    g_nextPollAt = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(std::max(0, delayMs));
}

void Update() {
    const auto now = std::chrono::steady_clock::now();
    bool force = false;
    bool heartbeat = false;
    std::string reason;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_initialized || (g_nextPollAt.time_since_epoch().count() != 0 && now < g_nextPollAt)) {
            return;
        }
        const RuntimeSnapshot::GameState gameState = RuntimeSnapshot::GetGameState();
        if (!gameState.inGame || gameState.loadingMenuOpen) {
            g_nextPollAt = now + std::chrono::seconds(1);
            return;
        }
        force = g_force;
        heartbeat = g_nextHeartbeatAt.time_since_epoch().count() == 0 || now >= g_nextHeartbeatAt;
        reason = g_reason.empty() ? (heartbeat ? "heartbeat" : "periodic_poll") : g_reason;
        g_force = false;
        g_reason.clear();
        g_nextPollAt = now + kPollInterval;
    }

    XNVSEAdapter::NativePlayerSurvivalState state;
    if (!XNVSEAdapter::CaptureNativePlayerSurvivalState(state) || !state.valid) {
        ScheduleRetry("capture_retry");
        return;
    }

    const std::string hash = BuildHash(state);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!force && !heartbeat && hash == g_lastObservedHash) return;
        g_lastObservedHash = hash;
        g_nextHeartbeatAt = now + kHeartbeatInterval;
    }

    QueueDelivery(BuildPayload(state), hash, reason);
}

} // namespace PlayerSurvivalManagerFNV
