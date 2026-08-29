#include "FalloutStatsManagerFNV.h"

#include "HTTPManager.h"
#include "Logger.h"
#include "Misc.h"
#include "RuntimeGeneration.h"
#include "TaskManager.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>

namespace FalloutStatsManagerFNV {
namespace {

constexpr auto kChangeDebounce = std::chrono::milliseconds(750);
constexpr auto kRetryDelay = std::chrono::seconds(2);
constexpr std::int64_t kMaximumStatValue = 2147483647LL;

constexpr std::array<const char*, 43> kStatNames = {
    "Quests Completed",
    "Locations Discovered",
    "People Killed",
    "Creatures Killed",
    "Locks Picked",
    "Computers Hacked",
    "Stimpaks Taken",
    "Rad-X Taken",
    "RadAway Taken",
    "Chems Taken",
    "Times Addicted",
    "Mines Disarmed",
    "Speech Successes",
    "Pockets Picked",
    "Pants Exploded",
    "Books Read",
    "Health From Stimpaks",
    "Weapons Created",
    "Health From Food",
    "Water Consumed",
    "Sandman Kills",
    "Paralyzing Punches",
    "Robots Disabled",
    "Times Slept",
    "Corpses Eaten",
    "Mysterious Stranger Visits",
    "Doctor Bags Used",
    "Challenges Completed",
    "Miss Fortunate Occurrences",
    "Disintegrations",
    "Have Limbs Crippled",
    "Speech Failures",
    "Items Crafted",
    "Weapon Modifications",
    "Items Repaired",
    "Total Things Killed",
    "Dismembered Limbs",
    "Caravan Games Won",
    "Caravan Games Lost",
    "Barter Amount Traded",
    "Roulette Games Played",
    "Blackjack Games Played",
    "Slots Games Played",
};

std::mutex g_mutex;
bool g_initialized{false};
bool g_dirty{false};
std::string g_reason;
std::unordered_map<std::string, std::int64_t> g_stats;
std::chrono::steady_clock::time_point g_deliverAt{};

std::int64_t NormalizeValue(std::int64_t value) {
    return std::clamp<std::int64_t>(value, 0, kMaximumStatValue);
}

std::string BuildPayload(const std::unordered_map<std::string, std::int64_t>& stats) {
    std::string playerName = Misc::GetPlayerName();
    if (playerName.empty()) playerName = "The Courier";

    const std::string gameTimestamp = Misc::GetGameTimeStamp();
    std::ostringstream json;
    json << '{';
    json << "\"schema\":\"dialectic.fallout_stats.v1\",";
    json << "\"type\":\"fallout_stats\",";
    json << "\"actor_name\":\"" << HTTPManager::EscapeJson(playerName) << "\",";
    json << "\"actor_type\":\"player\",";
    json << "\"refid\":\"0x00000014\",";
    json << "\"timestamp\":" << Misc::GetCurrentTimeMillis() << ',';
    json << "\"gamets\":" << (gameTimestamp.empty() ? "0" : gameTimestamp) << ',';
    json << "\"stats\":{";

    bool first = true;
    for (const char* statName : kStatNames) {
        const auto value = stats.find(statName);
        if (value == stats.end()) continue;
        if (!first) json << ',';
        first = false;
        json << '\"' << HTTPManager::EscapeJson(statName) << "\":" << value->second;
    }

    json << "}}";
    return json.str();
}

void ScheduleRetry(const char* reason) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized) return;
    g_dirty = true;
    g_reason = reason ? reason : "delivery_retry";
    g_deliverAt = std::chrono::steady_clock::now() + kRetryDelay;
}

void QueueDelivery(std::unordered_map<std::string, std::int64_t> stats, std::string reason) {
    const std::string payload = BuildPayload(stats);

    TaskManager::Options options;
    options.type = "fallout_stats_update";
    options.key = "fallout_stats";
    options.generation = RuntimeGeneration::Current();
    options.lane = TaskManager::Lane::Background;
    options.timeout = std::chrono::seconds(30);
    options.coalescing = TaskManager::CoalescingPolicy::ReplacePending;
    options.concurrencyLimit = 1;

    const TaskManager::TaskHandle task = TaskManager::Submit(
        std::move(options),
        [payload, reason = std::move(reason), count = stats.size()](
            const TaskManager::CancellationToken& token) {
            if (token.IsCancellationRequested()) return;
            const std::string response = HTTPManager::SendJson("gamedata.php", payload);
            if (response.find("\"ok\":true") == std::string::npos) {
                Logger::LogWarning(
                    "FalloutStatsManagerFNV: delivery failed trigger=%s; retry scheduled",
                    reason.c_str());
                ScheduleRetry("delivery_failed");
                return;
            }
            Logger::LogInfo(
                "[FALLOUT_STATS_UPDATE] delivered trigger=%s stats=%zu",
                reason.c_str(), count);
        });

    if (!task) {
        Logger::LogWarning("FalloutStatsManagerFNV: task queue rejected stats delivery");
        ScheduleRetry("task_rejected");
    }
}

} // namespace

void Initialize() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_initialized = true;
    g_dirty = false;
    g_reason.clear();
    g_stats.clear();
    g_deliverAt = {};
    Logger::LogInfo("FalloutStatsManagerFNV: initialized event_driven=1 reconciliation_seconds=300");
}

void Shutdown() {
    TaskManager::CancelByType("fallout_stats_update");
    std::lock_guard<std::mutex> lock(g_mutex);
    g_initialized = false;
    g_dirty = false;
    g_reason.clear();
    g_stats.clear();
    g_deliverAt = {};
}

void Reset(const char* reason) {
    TaskManager::CancelByType("fallout_stats_update");
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized) return;
    g_dirty = false;
    g_reason = reason ? reason : "runtime_reset";
    g_stats.clear();
    g_deliverAt = {};
}

bool UpdateStat(int statCode, std::int64_t value) {
    if (statCode < 0 || static_cast<std::size_t>(statCode) >= kStatNames.size()) return false;

    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized) return false;
    g_stats[kStatNames[static_cast<std::size_t>(statCode)]] = NormalizeValue(value);
    g_dirty = true;
    g_reason = "stat_changed";
    g_deliverAt = std::chrono::steady_clock::now() + kChangeDebounce;
    return true;
}

void Update() {
    std::unordered_map<std::string, std::int64_t> stats;
    std::string reason;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto now = std::chrono::steady_clock::now();
        if (!g_initialized || !g_dirty || g_deliverAt.time_since_epoch().count() == 0 || now < g_deliverAt) {
            return;
        }
        stats = g_stats;
        reason = g_reason.empty() ? "scheduled_update" : g_reason;
        g_dirty = false;
        g_reason.clear();
        g_deliverAt = {};
    }

    if (!stats.empty()) QueueDelivery(std::move(stats), std::move(reason));
}

} // namespace FalloutStatsManagerFNV
