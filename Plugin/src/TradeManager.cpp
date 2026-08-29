#include "TradeManager.h"

#include "Config.h"
#include "GameThreadDispatcher.h"
#include "HTTPManager.h"
#include "Logger.h"
#include "Misc.h"
#include "PlayerInventoryManagerFNV.h"
#include "RuntimeGeneration.h"
#include "RuntimeSnapshot.h"
#include "TaskManager.h"
#include "XNVSEAdapter.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace TradeManager {
namespace {

constexpr auto kPendingOpenTimeout = std::chrono::seconds(15);
constexpr auto kOpenSessionTimeout = std::chrono::minutes(5);

struct TradeInventoryItem {
    std::string name;
    std::string baseid;
    int count = 0;
    int type = -1;
    float condition = -1.0f;
};

struct TradeLineItem {
    TradeInventoryItem item;
    int count = 0;
};

enum class SessionState {
    Idle,
    PendingOpen,
    Open,
    AwaitingSnapshot,
};

struct ActiveSession {
    TradeSessionRequest request;
    SessionState state = SessionState::Idle;
    bool lastTradeMenuOpen = false;
    std::chrono::steady_clock::time_point createdAt;
    std::chrono::steady_clock::time_point openedAt;
    std::chrono::steady_clock::time_point closedAt;
    bool nativePreCaptured = false;
    bool nativePostCaptured = false;
    bool nativePostPending = false;
    std::vector<XNVSEAdapter::NativeInventoryItem> nativePlayerPre;
    std::vector<XNVSEAdapter::NativeInventoryItem> nativePlayerPost;
    std::vector<XNVSEAdapter::NativeInventoryItem> nativeCounterpartyPre;
    std::vector<XNVSEAdapter::NativeInventoryItem> nativeCounterpartyPost;
};

std::mutex g_mutex;
ActiveSession g_session;
bool g_lastObservedTradeMenuOpen = false;
std::chrono::steady_clock::time_point g_lastManagerUpdate;

std::string Trim(std::string value) {
    const char* whitespace = " \t\r\n";
    const size_t start = value.find_first_not_of(whitespace);
    if (start == std::string::npos) {
        return "";
    }
    const size_t end = value.find_last_not_of(whitespace);
    value = value.substr(start, end - start + 1);
    while (value.size() >= 2) {
        const char escaped = value[value.size() - 1];
        if (value[value.size() - 2] != '\\' || (escaped != 'n' && escaped != 'r')) {
            break;
        }
        value.resize(value.size() - 2);
        const size_t trimmedEnd = value.find_last_not_of(whitespace);
        if (trimmedEnd == std::string::npos) {
            return "";
        }
        value.resize(trimmedEnd + 1);
    }
    return value;
}

std::string ToLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string FormatFormId(uint32_t formId) {
    if (formId == 0) {
        return "";
    }
    std::ostringstream hex;
    hex << "0x" << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << formId;
    return hex.str();
}

bool IsTradeMenuOpen() {
    RuntimeSnapshot::GameState native;
    if (RuntimeSnapshot::TryGetFreshGameState(native, std::chrono::milliseconds(500))) {
        g_lastObservedTradeMenuOpen = native.barterMenuOpen || native.containerMenuOpen;
    }
    return g_lastObservedTradeMenuOpen;
}

bool IsCapsItem(const TradeInventoryItem& item) {
    std::string baseid = ToLower(Trim(item.baseid));
    if (baseid.size() > 2 && baseid[0] == '0' && baseid[1] == 'x') {
        baseid = baseid.substr(2);
    }
    return baseid == "0000000f" || baseid == "f" || ToLower(Trim(item.name)) == "bottle caps";
}

std::unordered_map<std::string, TradeInventoryItem> NativeInventoryMap(
    const std::vector<XNVSEAdapter::NativeInventoryItem>& source) {
    std::unordered_map<std::string, TradeInventoryItem> inventory;
    for (const auto& native : source) {
        if (native.baseFormId == 0 || native.count <= 0) continue;
        TradeInventoryItem item;
        item.name = native.name.empty() ? FormatFormId(native.baseFormId) : native.name;
        item.baseid = FormatFormId(native.baseFormId);
        item.count = native.count;
        item.type = native.type;
        item.condition = native.condition;
        std::string key = IsCapsItem(item) ? "0000000f" : ToLower(item.baseid);
        if (!IsCapsItem(item) && item.condition >= 0.0f) {
            key += ":condition:" + std::to_string(static_cast<int>(std::lround(item.condition * 1000.0f)));
        }
        auto& existing = inventory[key];
        if (existing.name.empty()) existing = item;
        else existing.count += item.count;
    }
    return inventory;
}

int GetItemCount(const std::unordered_map<std::string, TradeInventoryItem>& inventory,
                 const std::string& key) {
    const auto it = inventory.find(key);
    return it == inventory.end() ? 0 : it->second.count;
}

TradeInventoryItem PickItemMetadata(
    const std::string& key,
    const std::unordered_map<std::string, TradeInventoryItem>& playerPre,
    const std::unordered_map<std::string, TradeInventoryItem>& playerPost) {
    const std::unordered_map<std::string, TradeInventoryItem>* sources[] = {
        &playerPost,
        &playerPre,
    };
    for (const auto* source : sources) {
        const auto it = source->find(key);
        if (it != source->end()) {
            return it->second;
        }
    }

    TradeInventoryItem fallback;
    fallback.name = key;
    return fallback;
}

void AddTradeItem(std::map<std::string, TradeLineItem>& items,
                  const TradeInventoryItem& item,
                  int count) {
    if (count <= 0) {
        return;
    }

    std::string key = !item.baseid.empty() ? ToLower(Trim(item.baseid)) : ToLower(Trim(item.name));
    if (item.condition >= 0.0f) {
        key += ":condition:" + std::to_string(static_cast<int>(std::lround(item.condition * 1000.0f)));
    }
    if (key.empty()) {
        key = "unknown item";
    }

    auto& line = items[key];
    if (line.item.name.empty()) {
        line.item = item;
    }
    line.count += count;
}

std::string FormatTradeItemList(const std::map<std::string, TradeLineItem>& items) {
    std::ostringstream text;
    bool first = true;
    for (const auto& entry : items) {
        const TradeLineItem& line = entry.second;
        if (line.count <= 0) {
            continue;
        }
        if (!first) {
            text << ", ";
        }
        first = false;
        text << line.count << " " << (line.item.name.empty() ? "item" : line.item.name);
    }
    return text.str();
}

void AppendTradeItemsJson(std::ostringstream& payload,
                          const std::map<std::string, TradeLineItem>& items) {
    payload << "[";
    bool first = true;
    for (const auto& entry : items) {
        const TradeLineItem& line = entry.second;
        if (line.count <= 0) {
            continue;
        }
        if (!first) {
            payload << ",";
        }
        first = false;
        payload << "{";
        payload << "\"name\":\"" << HTTPManager::EscapeJson(line.item.name.empty() ? "item" : line.item.name) << "\",";
        payload << "\"baseid\":\"" << HTTPManager::EscapeJson(line.item.baseid) << "\",";
        payload << "\"type\":" << line.item.type << ",";
        if (line.item.condition >= 0.0f) {
            payload << "\"condition\":" << line.item.condition << ",";
        } else {
            payload << "\"condition\":null,";
        }
        payload << "\"count\":" << line.count;
        payload << "}";
    }
    payload << "]";
}

void EmitTradeSummaryEvent(const TradeSessionRequest& request,
                           const std::map<std::string, TradeLineItem>& playerReceivedItems,
                           const std::map<std::string, TradeLineItem>& playerGaveItems,
                           int capsDelta) {
    if (playerReceivedItems.empty() && playerGaveItems.empty() && capsDelta == 0) {
        return;
    }

    std::string playerName = Trim(Misc::GetPlayerName());
    if (playerName.empty()) {
        playerName = Config::playerName.empty() ? "Player" : Config::playerName;
    }

    const std::string speaker = request.speakerName.empty() ? "Unknown" : request.speakerName;
    const std::string tradeMode = request.tradeMode.empty() ? "trade" : request.tradeMode;
    const bool isBarter = tradeMode == "barter";

    std::vector<std::string> clauses;
    if (!playerReceivedItems.empty()) {
        const std::string items = FormatTradeItemList(playerReceivedItems);
        if (!items.empty()) {
            clauses.push_back(speaker + (isBarter ? " sold: " : " gave: ") + items);
        }
    }
    if (!playerGaveItems.empty()) {
        const std::string items = FormatTradeItemList(playerGaveItems);
        if (!items.empty()) {
            clauses.push_back(playerName + (isBarter ? " sold: " : " gave: ") + items);
        }
    }
    if (capsDelta < 0) {
        clauses.push_back(playerName + " gave " + speaker + " " + std::to_string(-capsDelta) + " caps");
    } else if (capsDelta > 0) {
        clauses.push_back(speaker + " gave " + playerName + " " + std::to_string(capsDelta) + " caps");
    }

    std::ostringstream textBuilder;
    textBuilder << speaker << " traded with " << playerName;
    if (!clauses.empty()) {
        textBuilder << ". ";
        for (size_t i = 0; i < clauses.size(); ++i) {
            if (i > 0) {
                textBuilder << ". ";
            }
            textBuilder << clauses[i];
        }
    }
    std::string text = textBuilder.str();
    if (!text.empty() && text.back() != '.') {
        text.push_back('.');
    }

    const std::string location = Trim(Misc::GetPlayerLocation());
    std::ostringstream payload;
    payload << "{";
    payload << "\"type\":\"trade_summary\",";
    payload << "\"schema\":\"dialectic.trade_summary.v1\",";
    payload << "\"request_id\":\"" << request.requestId << "\",";
    payload << "\"action\":\"" << HTTPManager::EscapeJson(request.action) << "\",";
    payload << "\"trade_mode\":\"" << HTTPManager::EscapeJson(tradeMode) << "\",";
    payload << "\"speaker\":\"" << HTTPManager::EscapeJson(speaker) << "\",";
    payload << "\"speaker_formid\":\"" << HTTPManager::EscapeJson(FormatFormId(request.speakerFormId)) << "\",";
    payload << "\"player\":\"" << HTTPManager::EscapeJson(playerName) << "\",";
    payload << "\"text\":\"" << HTTPManager::EscapeJson(text) << "\",";
    payload << "\"caps_delta\":" << capsDelta << ",";
    payload << "\"player_received_items\":";
    AppendTradeItemsJson(payload, playerReceivedItems);
    payload << ",";
    payload << "\"player_gave_items\":";
    AppendTradeItemsJson(payload, playerGaveItems);
    payload << ",";
    payload << "\"location\":\"" << HTTPManager::EscapeJson(location.empty() ? "Unknown" : location) << "\"";
    payload << "}";

    Logger::LogInfo("TradeManager: trade summary %s", text.c_str());
    const std::string json = payload.str();
    TaskManager::Enqueue("gamedata", "trade_summary", RuntimeGeneration::Current(), true,
        std::chrono::seconds(30), [json](const TaskManager::CancellationToken& token) {
        if (token.IsCancellationRequested()) return;
        const std::string response = HTTPManager::SendJson("gamedata.php", json);
        if (response.find("OK") == std::string::npos) {
            Logger::LogWarning("TradeManager: trade_summary upload returned non-OK response");
        }
    });
}

bool EmitInventoryDelta(const TradeSessionRequest& request,
                        const std::unordered_map<std::string, TradeInventoryItem>& playerPre,
                        const std::unordered_map<std::string, TradeInventoryItem>& playerPost,
                        const char* source,
                        const std::unordered_map<std::string, TradeInventoryItem>* counterpartyPre = nullptr,
                        const std::unordered_map<std::string, TradeInventoryItem>* counterpartyPost = nullptr) {
    std::set<std::string> keys;
    for (const auto& entry : playerPre) keys.insert(entry.first);
    for (const auto& entry : playerPost) keys.insert(entry.first);

    std::map<std::string, TradeLineItem> playerReceivedItems;
    std::map<std::string, TradeLineItem> playerGaveItems;
    int capsDelta = 0;
    int changedItems = 0;
    for (const std::string& key : keys) {
        const int playerDelta = GetItemCount(playerPost, key) - GetItemCount(playerPre, key);
        if (playerDelta == 0) continue;
        if (counterpartyPre && counterpartyPost) {
            const int counterpartyDelta = GetItemCount(*counterpartyPost, key) -
                GetItemCount(*counterpartyPre, key);
            if (playerDelta + counterpartyDelta != 0) {
                Logger::LogInfo("TradeManager: native conservation mismatch request=%llu key=%s player_delta=%d counterparty_delta=%d",
                    static_cast<unsigned long long>(request.requestId), key.c_str(),
                    playerDelta, counterpartyDelta);
            }
        }
        const TradeInventoryItem item = PickItemMetadata(key, playerPre, playerPost);
        if (IsCapsItem(item)) {
            capsDelta += playerDelta;
        } else if (playerDelta > 0) {
            AddTradeItem(playerReceivedItems, item, playerDelta);
            ++changedItems;
        } else {
            AddTradeItem(playerGaveItems, item, -playerDelta);
            ++changedItems;
        }
    }

    if (changedItems == 0 && capsDelta == 0) {
        Logger::LogInfo("TradeManager: session %llu closed with no %s inventory deltas",
            static_cast<unsigned long long>(request.requestId), source ? source : "trade");
        return false;
    }
    EmitTradeSummaryEvent(request, playerReceivedItems, playerGaveItems, capsDelta);
    Logger::LogInfo("TradeManager: session %llu emitted %s trade_summary items=%d caps_delta=%d",
        static_cast<unsigned long long>(request.requestId), source ? source : "trade",
        changedItems, capsDelta);
    return true;
}

bool ProcessNativeSnapshot() {
    ActiveSession session;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_session.state != SessionState::AwaitingSnapshot ||
            !g_session.nativePreCaptured || !g_session.nativePostCaptured) {
            return false;
        }
        session = g_session;
    }
    const auto playerPre = NativeInventoryMap(session.nativePlayerPre);
    const auto playerPost = NativeInventoryMap(session.nativePlayerPost);
    const auto counterpartyPre = NativeInventoryMap(session.nativeCounterpartyPre);
    const auto counterpartyPost = NativeInventoryMap(session.nativeCounterpartyPost);
    EmitInventoryDelta(session.request, playerPre, playerPost, "native",
        &counterpartyPre, &counterpartyPost);
    PlayerInventoryManagerFNV::MarkDirty("trade_closed", 200);
    CancelAll("native_snapshot_processed");
    return true;
}

void MarkOpenIfNeeded(bool tradeMenuOpen, const std::chrono::steady_clock::time_point& now) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_session.state == SessionState::PendingOpen && tradeMenuOpen) {
        g_session.state = SessionState::Open;
        g_session.openedAt = now;
        g_session.lastTradeMenuOpen = true;
        Logger::LogInfo("TradeManager: %s menu opened request=%llu speaker=%s",
            g_session.request.tradeMode.c_str(),
            static_cast<unsigned long long>(g_session.request.requestId),
            g_session.request.speakerName.c_str());
    }
}

void QueueNativePostSnapshot(const TradeSessionRequest& request) {
    const std::uint64_t generation = RuntimeGeneration::Current();
    const std::uint32_t playerFormId = Misc::GetPlayerFormId() != 0
        ? Misc::GetPlayerFormId() : 0x00000014;
    if (!GameThreadDispatcher::Enqueue("trade_snapshot", "post", generation,
        [request, playerFormId]() {
            std::vector<XNVSEAdapter::NativeInventoryItem> playerPost;
            std::vector<XNVSEAdapter::NativeInventoryItem> counterpartyPost;
            const bool playerOk = XNVSEAdapter::CaptureNativeInventory(playerFormId, playerPost);
            const bool counterpartyOk = XNVSEAdapter::CaptureNativeInventory(
                request.inventoryOwnerFormId != 0 ? request.inventoryOwnerFormId : request.speakerFormId,
                counterpartyPost);
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_session.request.requestId != request.requestId) return;
            g_session.nativePlayerPost = std::move(playerPost);
            g_session.nativeCounterpartyPost = std::move(counterpartyPost);
            g_session.nativePostCaptured = playerOk && counterpartyOk;
            g_session.nativePostPending = false;
            Logger::LogInfo("TradeManager: native post snapshot request=%llu player=%d counterparty=%d",
                static_cast<unsigned long long>(request.requestId), playerOk ? 1 : 0,
                counterpartyOk ? 1 : 0);
        },
        [request](const char* reason) {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_session.request.requestId == request.requestId) {
                g_session.nativePostPending = false;
            }
            Logger::LogWarning("TradeManager: native post snapshot dropped request=%llu reason=%s",
                static_cast<unsigned long long>(request.requestId), reason ? reason : "unknown");
        })) {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_session.request.requestId == request.requestId) g_session.nativePostPending = false;
    }
}

void MarkClosedIfNeeded(bool tradeMenuOpen, const std::chrono::steady_clock::time_point& now) {
    TradeSessionRequest request;
    bool queueNative = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_session.state == SessionState::Open && !tradeMenuOpen && g_session.lastTradeMenuOpen) {
            g_session.state = SessionState::AwaitingSnapshot;
            g_session.closedAt = now;
            g_session.lastTradeMenuOpen = false;
            request = g_session.request;
            queueNative = g_session.nativePreCaptured;
            g_session.nativePostPending = queueNative;
            Logger::LogInfo("TradeManager: %s menu closed request=%llu; waiting for post snapshot",
                g_session.request.tradeMode.c_str(),
                static_cast<unsigned long long>(g_session.request.requestId));
        }
    }
    if (queueNative) QueueNativePostSnapshot(request);
}

void ExpireStaleSession(const std::chrono::steady_clock::time_point& now) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_session.state == SessionState::PendingOpen &&
        now - g_session.createdAt > kPendingOpenTimeout) {
        Logger::LogInfo("TradeManager: pending %s session request=%llu expired before menu opened",
            g_session.request.tradeMode.c_str(),
            static_cast<unsigned long long>(g_session.request.requestId));
        g_session = {};
    } else if ((g_session.state == SessionState::Open || g_session.state == SessionState::AwaitingSnapshot) &&
               now - g_session.openedAt > kOpenSessionTimeout) {
        Logger::LogInfo("TradeManager: %s session request=%llu expired",
            g_session.request.tradeMode.c_str(),
            static_cast<unsigned long long>(g_session.request.requestId));
        g_session = {};
    }
}

} // namespace

void Initialize() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_session = {};
    g_lastObservedTradeMenuOpen = false;
    g_lastManagerUpdate = {};
    Logger::LogInfo("TradeManager: initialized with native inventory snapshots");
}

void Shutdown() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_session = {};
}

void BeginPendingSession(const TradeSessionRequest& request) {
    if (request.requestId == 0) {
        return;
    }

    std::vector<XNVSEAdapter::NativeInventoryItem> playerPre;
    std::vector<XNVSEAdapter::NativeInventoryItem> counterpartyPre;
    bool nativePreCaptured = false;
    if (GameThreadDispatcher::IsGameThread()) {
        const std::uint32_t playerFormId = Misc::GetPlayerFormId() != 0
            ? Misc::GetPlayerFormId() : 0x00000014;
        const bool playerOk = XNVSEAdapter::CaptureNativeInventory(playerFormId, playerPre);
        const bool counterpartyOk = XNVSEAdapter::CaptureNativeInventory(
            request.inventoryOwnerFormId != 0 ? request.inventoryOwnerFormId : request.speakerFormId,
            counterpartyPre);
        nativePreCaptured = playerOk && counterpartyOk;
        Logger::LogInfo("TradeManager: native pre snapshot request=%llu player=%d counterparty=%d",
            static_cast<unsigned long long>(request.requestId), playerOk ? 1 : 0,
            counterpartyOk ? 1 : 0);
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    g_session = {};
    if (!nativePreCaptured) {
        Logger::LogWarning("TradeManager: native pre snapshot unavailable request=%llu; trade delta tracking disabled for this session",
            static_cast<unsigned long long>(request.requestId));
        return;
    }
    g_session.request = request;
    if (g_session.request.tradeMode.empty()) {
        g_session.request.tradeMode = request.action == "Barter" ? "barter" : "trade";
    }
    g_session.state = SessionState::PendingOpen;
    g_session.createdAt = std::chrono::steady_clock::now();
    g_session.lastTradeMenuOpen = false;
    g_session.nativePreCaptured = nativePreCaptured;
    g_session.nativePlayerPre = std::move(playerPre);
    g_session.nativeCounterpartyPre = std::move(counterpartyPre);

    Logger::LogInfo("TradeManager: pending %s session request=%llu speaker=%s form=0x%08X",
        g_session.request.tradeMode.c_str(),
        static_cast<unsigned long long>(g_session.request.requestId),
        g_session.request.speakerName.c_str(),
        g_session.request.speakerFormId);
}

void CancelAll(const char* reason) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_session.state != SessionState::Idle) {
        Logger::LogInfo("TradeManager: clearing session request=%llu reason=%s",
            static_cast<unsigned long long>(g_session.request.requestId),
            reason ? reason : "unknown");
    }
    g_session = {};
}

void Update() {
    const auto now = std::chrono::steady_clock::now();
    bool active = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        active = g_session.state != SessionState::Idle;
    }
    const auto interval = active
        ? std::chrono::milliseconds(100)
        : std::chrono::milliseconds(1000);
    if (g_lastManagerUpdate.time_since_epoch().count() != 0 &&
        now - g_lastManagerUpdate < interval) {
        return;
    }
    g_lastManagerUpdate = now;

    const bool tradeMenuOpen = IsTradeMenuOpen();

    MarkOpenIfNeeded(tradeMenuOpen, now);
    MarkClosedIfNeeded(tradeMenuOpen, now);
    ProcessNativeSnapshot();
    ExpireStaleSession(now);
}

} // namespace TradeManager
