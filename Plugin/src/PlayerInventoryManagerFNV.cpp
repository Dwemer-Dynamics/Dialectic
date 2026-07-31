#include "PlayerInventoryManagerFNV.h"

#include "HTTPManager.h"
#include "Logger.h"
#include "Misc.h"
#include "RuntimeGeneration.h"
#include "RuntimeSnapshot.h"
#include "TaskManager.h"
#include "WorldContextFNV.h"
#include "XNVSEAdapter.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace PlayerInventoryManagerFNV {
namespace {

constexpr auto kInitialRefreshDelay = std::chrono::seconds(2);
constexpr auto kReconcileInterval = std::chrono::seconds(30);
constexpr auto kRetryDelay = std::chrono::seconds(2);

struct InventoryItem {
    std::string name;
    std::uint32_t baseFormId{0};
    int count{0};
    int value{0};
    bool equipped{false};
    int type{0};
    float condition{-1.0f};
};

std::mutex g_mutex;
bool g_initialized{false};
bool g_dirty{false};
bool g_force{false};
std::string g_reason;
std::string g_lastObservedHash;
std::string g_lastDeliveredHash;
std::chrono::steady_clock::time_point g_dueAt{};
std::chrono::steady_clock::time_point g_nextReconcileAt{};

std::string FormIdHex(std::uint32_t formId) {
    std::ostringstream output;
    output << "0x" << std::hex << std::setw(8) << std::setfill('0') << formId;
    return output.str();
}

std::vector<InventoryItem> NormalizeInventory(
    const std::vector<XNVSEAdapter::NativeInventoryItem>& nativeItems) {
    std::vector<InventoryItem> items;
    items.reserve(nativeItems.size());
    for (const auto& nativeItem : nativeItems) {
        if (nativeItem.baseFormId == 0 || nativeItem.count <= 0 || nativeItem.name.empty()) {
            continue;
        }
        InventoryItem item;
        item.name = nativeItem.name;
        item.baseFormId = nativeItem.baseFormId;
        item.count = nativeItem.count;
        item.value = nativeItem.value;
        item.equipped = nativeItem.equipped;
        item.type = nativeItem.type;
        item.condition = nativeItem.condition;
        items.push_back(std::move(item));
    }
    std::sort(items.begin(), items.end(), [](const InventoryItem& left, const InventoryItem& right) {
        if (left.baseFormId != right.baseFormId) return left.baseFormId < right.baseFormId;
        if (left.name != right.name) return left.name < right.name;
        return left.count < right.count;
    });
    return items;
}

std::string BuildInventoryHash(const std::vector<InventoryItem>& items) {
    std::ostringstream hash;
    hash << std::fixed << std::setprecision(4);
    for (const auto& item : items) {
        hash << item.baseFormId << '^' << item.name << '^' << item.count << '^'
             << item.value << '^' << (item.equipped ? 1 : 0) << '^'
             << item.type << '^' << item.condition << '|';
    }
    return hash.str();
}

std::string BuildInventoryJson(const std::vector<InventoryItem>& items,
                               const std::string& playerName,
                               std::uint32_t playerFormId) {
    std::ostringstream json;
    json << "{";
    json << "\"schema\":\"dialectic.inventory.v1\",";
    json << "\"type\":\"inventory\",";
    json << "\"game\":\"fnv\",";
    json << "\"actor_name\":\"" << HTTPManager::EscapeJson(playerName) << "\",";
    json << "\"actor_type\":\"player\",";
    json << "\"refid\":\"" << FormIdHex(playerFormId) << "\",";
    json << "\"timestamp\":" << Misc::GetCurrentTimeMillis() << ",";
    const std::string gameTimestamp = Misc::GetGameTimeStamp();
    json << "\"gamets\":" << (gameTimestamp.empty() ? "0" : gameTimestamp) << ",";
    json << "\"items\":[";
    for (std::size_t index = 0; index < items.size(); ++index) {
        if (index > 0) json << ',';
        const auto& item = items[index];
        json << "{";
        json << "\"name\":\"" << HTTPManager::EscapeJson(item.name) << "\",";
        json << "\"baseid\":\"" << FormIdHex(item.baseFormId) << "\",";
        json << "\"count\":" << item.count << ',';
        json << "\"value\":" << item.value << ',';
        json << "\"equipped\":" << (item.equipped ? "true" : "false") << ',';
        if (item.condition >= 0.0f) {
            json << "\"condition\":" << item.condition << ',';
        } else {
            json << "\"condition\":null,";
        }
        json << "\"type\":" << item.type << ',';
        json << "\"ammo\":null,\"mods\":[]";
        json << "}";
    }
    json << "]}";
    return json.str();
}

void ScheduleRetry(const std::string& hash, const char* reason) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized || g_lastObservedHash != hash) return;
    g_lastObservedHash.clear();
    g_dirty = true;
    g_force = true;
    g_reason = reason ? reason : "delivery_retry";
    g_dueAt = std::chrono::steady_clock::now() + kRetryDelay;
}

void QueueInventoryDelivery(std::string payload, std::string hash,
                            std::size_t itemCount, std::string reason) {
    TaskManager::Options options;
    options.type = "player_inventory_update";
    options.key = "player_inventory";
    options.generation = RuntimeGeneration::Current();
    options.lane = TaskManager::Lane::Background;
    options.timeout = std::chrono::seconds(30);
    options.coalescing = TaskManager::CoalescingPolicy::ReplacePending;
    options.concurrencyLimit = 1;

    const TaskManager::TaskHandle task = TaskManager::Submit(
        std::move(options),
        [payload = std::move(payload), hash, itemCount, reason](const TaskManager::CancellationToken& token) {
            if (token.IsCancellationRequested()) return;
            const std::string response = HTTPManager::SendJson("gamedata.php", payload);
            if (response.empty()) {
                Logger::LogWarning(
                    "PlayerInventoryManagerFNV: delivery failed items=%zu trigger=%s; retry scheduled",
                    itemCount, reason.c_str());
                ScheduleRetry(hash, "delivery_failed");
                return;
            }
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_lastDeliveredHash = hash;
            }
            Logger::LogInfo(
                "[PLAYER_INVENTORY_UPDATE] delivered items=%zu trigger=%s",
                itemCount, reason.c_str());
        });

    if (!task) {
        Logger::LogWarning("PlayerInventoryManagerFNV: task queue rejected inventory delivery");
        ScheduleRetry(hash, "task_rejected");
    }
}

} // namespace

void Initialize() {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_initialized = true;
        g_dirty = true;
        g_force = true;
        g_reason = "initial_load";
        g_dueAt = std::chrono::steady_clock::now() + kInitialRefreshDelay;
        g_nextReconcileAt = std::chrono::steady_clock::now() + kReconcileInterval;
        g_lastObservedHash.clear();
        g_lastDeliveredHash.clear();
    }
    XNVSEAdapter::SetPlayerInventoryChangeCallback([](const char* reason) {
        MarkDirty(reason, 200);
    });
    Logger::LogInfo(
        "PlayerInventoryManagerFNV: initialized native_events=%d reconcile_seconds=%lld",
        XNVSEAdapter::HasPlayerInventoryEventHooks() ? 1 : 0,
        static_cast<long long>(
            std::chrono::duration_cast<std::chrono::seconds>(kReconcileInterval).count()));
}

void Shutdown() {
    XNVSEAdapter::SetPlayerInventoryChangeCallback({});
    TaskManager::CancelByType("player_inventory_update");
    std::lock_guard<std::mutex> lock(g_mutex);
    g_initialized = false;
    g_dirty = false;
    g_force = false;
    g_reason.clear();
    g_lastObservedHash.clear();
    g_lastDeliveredHash.clear();
    g_dueAt = {};
    g_nextReconcileAt = {};
}

void MarkDirty(const char* reason, int delayMs) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized) return;
    const auto now = std::chrono::steady_clock::now();
    if (!g_dirty) {
        g_dueAt = now + std::chrono::milliseconds(std::max(0, delayMs));
        g_reason = reason ? reason : "inventory_event";
    }
    g_dirty = true;
}

void ForceRefresh(const char* reason, int delayMs) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized) return;
    g_dirty = true;
    g_force = true;
    g_reason = reason ? reason : "forced_refresh";
    g_dueAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(0, delayMs));
}

void Reset(const char* reason) {
    TaskManager::CancelByType("player_inventory_update");
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized) return;
    g_dirty = false;
    g_force = false;
    g_reason = reason ? reason : "runtime_reset";
    g_lastObservedHash.clear();
    g_lastDeliveredHash.clear();
    g_dueAt = {};
    g_nextReconcileAt = std::chrono::steady_clock::now() + kReconcileInterval;
}

void Update() {
    const auto now = std::chrono::steady_clock::now();
    bool force = false;
    std::string reason;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_initialized) return;
        if (g_nextReconcileAt.time_since_epoch().count() == 0 || now >= g_nextReconcileAt) {
            g_nextReconcileAt = now + kReconcileInterval;
            if (!g_dirty) {
                g_dirty = true;
                g_dueAt = now;
                g_reason = "periodic_reconcile";
            }
        }
        if (!g_dirty || now < g_dueAt) return;
        const RuntimeSnapshot::GameState gameState = RuntimeSnapshot::GetGameState();
        if (!gameState.inGame || WorldContextFNV::GetGameTimestamp() <= 0) {
            g_dueAt = now + std::chrono::seconds(1);
            return;
        }
        force = g_force;
        reason = g_reason.empty() ? "inventory_event" : g_reason;
        g_dirty = false;
        g_force = false;
    }

    const auto captureStarted = std::chrono::steady_clock::now();
    std::vector<XNVSEAdapter::NativeInventoryItem> nativeItems;
    const std::uint32_t playerFormId = Misc::GetPlayerFormId() != 0
        ? Misc::GetPlayerFormId() : 0x00000014;
    if (!XNVSEAdapter::CaptureNativeInventory(playerFormId, nativeItems)) {
        Logger::LogWarning(
            "PlayerInventoryManagerFNV: native capture unavailable trigger=%s; retry scheduled",
            reason.c_str());
        ForceRefresh("capture_retry", 1000);
        return;
    }

    std::vector<InventoryItem> items = NormalizeInventory(nativeItems);
    const std::string hash = BuildInventoryHash(items);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!force && hash == g_lastObservedHash) return;
        g_lastObservedHash = hash;
    }

    std::string playerName = Misc::GetPlayerName();
    if (playerName.empty()) playerName = "The Courier";
    const std::string payload = BuildInventoryJson(items, playerName, playerFormId);
    const auto captureMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - captureStarted).count();
    if (captureMs >= 5) {
        Logger::LogInfo(
            "[PERF] player inventory capture items=%zu elapsed_ms=%lld trigger=%s",
            items.size(), static_cast<long long>(captureMs), reason.c_str());
    }
    QueueInventoryDelivery(payload, hash, items.size(), reason);
}

} // namespace PlayerInventoryManagerFNV
