#include "TradeManager.h"

#include "Config.h"
#include "GameThreadDispatcher.h"
#include "HTTPManager.h"
#include "Logger.h"
#include "Misc.h"
#include "NativeComparisonTelemetry.h"
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
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace TradeManager {
namespace {

constexpr const char* kGameStateBridgePath = "Data\\NVSE\\Plugins\\dialectic_game_state.tmp";
constexpr const char* kTradeSessionPath = "Data\\NVSE\\Plugins\\dialectic_trade_session.tmp";
constexpr const char* kTradeDonePath = "Data\\NVSE\\Plugins\\dialectic_trade_done.tmp";
constexpr const char* kTradePrePlayerPath = "Data\\NVSE\\Plugins\\dialectic_trade_pre_player.tmp";
constexpr const char* kTradePostPlayerPath = "Data\\NVSE\\Plugins\\dialectic_trade_post_player.tmp";
constexpr const char* kTradePreCounterpartyPath = "Data\\NVSE\\Plugins\\dialectic_trade_pre_counterparty.tmp";
constexpr const char* kTradePostCounterpartyPath = "Data\\NVSE\\Plugins\\dialectic_trade_post_counterparty.tmp";
constexpr const char* kTradeStatusPath = "Data\\NVSE\\Plugins\\dialectic_trade_status.txt";

constexpr auto kPendingOpenTimeout = std::chrono::seconds(15);
constexpr auto kOpenSessionTimeout = std::chrono::minutes(5);
constexpr auto kTradeComparisonTimeout = std::chrono::seconds(5);

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
struct PendingTradeComparison {
    bool active = false;
    std::uint64_t requestId = 0;
    std::chrono::steady_clock::time_point createdAt{};
    std::unordered_map<std::string, TradeInventoryItem> playerPre;
    std::unordered_map<std::string, TradeInventoryItem> playerPost;
    std::unordered_map<std::string, TradeInventoryItem> counterpartyPre;
    std::unordered_map<std::string, TradeInventoryItem> counterpartyPost;
};
PendingTradeComparison g_tradeComparison;
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

std::string ReadFileIfExists(const char* path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        return "";
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

void DeleteFileIfExists(const char* path) {
    std::remove(path);
}

std::vector<std::string> Split(const std::string& value, char delimiter) {
    std::vector<std::string> parts;
    std::stringstream stream(value);
    std::string part;
    while (std::getline(stream, part, delimiter)) {
        parts.push_back(Trim(part));
    }
    return parts;
}

std::vector<std::string> ReadNonEmptyLines(const std::string& data) {
    std::vector<std::string> lines;
    std::istringstream stream(data);
    std::string line;
    while (std::getline(stream, line)) {
        line = Trim(line);
        if (!line.empty() && line != "%e" && line != "end") {
            lines.push_back(line);
        }
    }
    return lines;
}

std::unordered_map<std::string, std::string> ParseKeyValueData(const std::string& data) {
    std::unordered_map<std::string, std::string> values;
    std::istringstream stream(data);
    std::string line;
    while (std::getline(stream, line)) {
        line = Trim(line);
        if (line.empty()) {
            continue;
        }

        const size_t equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }

        values[Trim(line.substr(0, equals))] = Trim(line.substr(equals + 1));
    }
    return values;
}

bool ParseBoolField(const std::unordered_map<std::string, std::string>& values,
                    const char* key,
                    bool fallback = false) {
    const auto it = values.find(key);
    if (it == values.end()) {
        return fallback;
    }
    const std::string value = ToLower(Trim(it->second));
    return value == "1" || value == "true" || value == "yes";
}

uint32_t ParseFormId(std::string value) {
    value = Trim(value);
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

int ParseInt(const std::string& value, int fallback = 0) {
    try {
        return std::stoi(Trim(value));
    } catch (...) {
        return fallback;
    }
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
        return g_lastObservedTradeMenuOpen;
    }

    const std::string data = ReadFileIfExists(kGameStateBridgePath);
    if (data.empty()) {
        return g_lastObservedTradeMenuOpen;
    }

    const auto values = ParseKeyValueData(data);
    g_lastObservedTradeMenuOpen = ParseBoolField(values, "trade_menu_mode", false);
    return g_lastObservedTradeMenuOpen;
}

bool IsCapsItem(const TradeInventoryItem& item) {
    std::string baseid = ToLower(Trim(item.baseid));
    if (baseid.size() > 2 && baseid[0] == '0' && baseid[1] == 'x') {
        baseid = baseid.substr(2);
    }
    return baseid == "0000000f" || baseid == "f" || ToLower(Trim(item.name)) == "bottle caps";
}

std::unordered_map<std::string, TradeInventoryItem> ParseInventorySnapshot(const std::string& data) {
    std::unordered_map<std::string, TradeInventoryItem> inventory;
    std::istringstream stream(data);
    std::string line;
    while (std::getline(stream, line)) {
        line = Trim(line);
        if (line.rfind("inventory=", 0) != 0) {
            continue;
        }

        const std::vector<std::string> parts = Split(line.substr(10), '^');
        if (parts.size() < 3) {
            continue;
        }

        TradeInventoryItem item;
        item.name = Trim(parts[0]);
        item.baseid = Trim(parts[1]);
        item.count = std::max(0, ParseInt(parts[2], 0));
        item.type = parts.size() >= 4 ? ParseInt(parts[3], -1) : -1;
        if (item.count <= 0) {
            continue;
        }

        std::string key = IsCapsItem(item) ? "0000000f" : (!item.baseid.empty() ? item.baseid : item.name);
        key = ToLower(Trim(key));
        if (key.empty()) {
            continue;
        }

        auto& existing = inventory[key];
        if (existing.name.empty()) {
            existing.name = item.name;
        }
        if (existing.baseid.empty()) {
            existing.baseid = item.baseid;
        }
        if (existing.type < 0) {
            existing.type = item.type;
        }
        if (IsCapsItem(item)) {
            existing.count = std::max(existing.count, item.count);
        } else {
            existing.count += item.count;
        }
    }
    return inventory;
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

void CleanupBridgeFiles() {
    DeleteFileIfExists(kTradeSessionPath);
    DeleteFileIfExists(kTradeDonePath);
    DeleteFileIfExists(kTradePrePlayerPath);
    DeleteFileIfExists(kTradePostPlayerPath);
    DeleteFileIfExists(kTradePreCounterpartyPath);
    DeleteFileIfExists(kTradePostCounterpartyPath);
    DeleteFileIfExists(kTradeStatusPath);
}

TradeSessionRequest ReadScriptSessionFallback() {
    TradeSessionRequest request;
    const std::vector<std::string> lines = ReadNonEmptyLines(ReadFileIfExists(kTradeSessionPath));
    if (lines.size() >= 5) {
        request.requestId = static_cast<uint64_t>(std::strtoull(lines[0].c_str(), nullptr, 10));
        request.action = lines[1];
        request.speakerFormId = ParseFormId(lines[2]);
        request.speakerName = lines[3];
        request.tradeMode = lines[4];
    }
    return request;
}

TradeSessionRequest CurrentRequestOrScriptFallback() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_session.state != SessionState::Idle && g_session.request.requestId != 0) {
        return g_session.request;
    }
    return ReadScriptSessionFallback();
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

std::size_t CountInventoryComparisonMismatches(
    const std::unordered_map<std::string, TradeInventoryItem>& native,
    const std::unordered_map<std::string, TradeInventoryItem>& bridge) {
    std::set<std::string> keys;
    for (const auto& entry : native) keys.insert(entry.first);
    for (const auto& entry : bridge) keys.insert(entry.first);
    std::size_t mismatches = 0;
    for (const std::string& key : keys) {
        const auto nativeItem = native.find(key);
        const auto bridgeItem = bridge.find(key);
        if (nativeItem == native.end() || bridgeItem == bridge.end()) {
            ++mismatches;
            continue;
        }
        if (nativeItem->second.count != bridgeItem->second.count) {
            ++mismatches;
            continue;
        }
        if (nativeItem->second.condition >= 0.0f && bridgeItem->second.condition >= 0.0f &&
            std::fabs(nativeItem->second.condition - bridgeItem->second.condition) > 0.02f) {
            ++mismatches;
        }
    }
    return mismatches;
}

bool ProcessPendingTradeComparison(const std::chrono::steady_clock::time_point& now) {
    PendingTradeComparison comparison;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_tradeComparison.active) return false;
        comparison = g_tradeComparison;
    }

    const std::string doneData = ReadFileIfExists(kTradeDonePath);
    const bool timedOut = now - comparison.createdAt >= kTradeComparisonTimeout;
    if (doneData.empty() && !timedOut) return true;

    if (!doneData.empty()) {
        const auto playerPre = ParseInventorySnapshot(ReadFileIfExists(kTradePrePlayerPath));
        const auto playerPost = ParseInventorySnapshot(ReadFileIfExists(kTradePostPlayerPath));
        const std::size_t mismatches =
            CountInventoryComparisonMismatches(comparison.playerPre, playerPre) +
            CountInventoryComparisonMismatches(comparison.playerPost, playerPost);
        std::ostringstream detail;
        detail << "request=" << comparison.requestId
               << " mismatches=" << mismatches
               << " native_player_pre=" << comparison.playerPre.size()
               << " bridge_player_pre=" << playerPre.size()
               << " native_player_post=" << comparison.playerPost.size()
               << " bridge_player_post=" << playerPost.size()
               << " counterparty_native_conservation_only=1";
        NativeComparisonTelemetry::Record("trade_inventory",
            mismatches == 0 ? NativeComparisonTelemetry::Result::Match
                            : NativeComparisonTelemetry::Result::Mismatch,
            detail.str());
    } else {
        NativeComparisonTelemetry::Record(
            "trade_inventory", NativeComparisonTelemetry::Result::Unavailable,
            "bridge post-close snapshot timed out");
    }

    CleanupBridgeFiles();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_tradeComparison.requestId == comparison.requestId) g_tradeComparison = {};
    }
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
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_tradeComparison.active = true;
        g_tradeComparison.requestId = session.request.requestId;
        g_tradeComparison.createdAt = std::chrono::steady_clock::now();
        g_tradeComparison.playerPre = playerPre;
        g_tradeComparison.playerPost = playerPost;
        g_tradeComparison.counterpartyPre = counterpartyPre;
        g_tradeComparison.counterpartyPost = counterpartyPost;
    }
    return true;
}

bool ProcessDoneSnapshot() {
    const std::string doneData = ReadFileIfExists(kTradeDonePath);
    if (doneData.empty()) {
        return false;
    }

    const auto doneFields = ParseKeyValueData(doneData);
    const auto getDoneField = [&doneFields](const char* key) -> std::string {
        const auto it = doneFields.find(key);
        return it == doneFields.end() ? "" : it->second;
    };
    if (getDoneField("end") != "1") {
        return false;
    }

    TradeSessionRequest request = CurrentRequestOrScriptFallback();
    if (request.requestId == 0) {
        Logger::LogInfo("TradeManager: done snapshot found without session metadata; cleaning up");
        CleanupBridgeFiles();
        return true;
    }

    const std::string status = getDoneField("status");
    if (!status.empty() && status != "ok") {
        Logger::LogInfo("TradeManager: session %llu ended with status=%s reason=%s",
            static_cast<unsigned long long>(request.requestId),
            status.c_str(),
            getDoneField("reason").c_str());
        CleanupBridgeFiles();
        CancelAll("snapshot_error");
        return true;
    }

    const auto playerPre = ParseInventorySnapshot(ReadFileIfExists(kTradePrePlayerPath));
    const auto playerPost = ParseInventorySnapshot(ReadFileIfExists(kTradePostPlayerPath));

    EmitInventoryDelta(request, playerPre, playerPost, "bridge");
    PlayerInventoryManagerFNV::MarkDirty("trade_closed_bridge", 200);

    CleanupBridgeFiles();
    CancelAll("snapshot_processed");
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
        CleanupBridgeFiles();
        g_session = {};
    }
}

} // namespace

void Initialize() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_session = {};
    g_tradeComparison = {};
    g_lastObservedTradeMenuOpen = false;
    g_lastManagerUpdate = {};
    CleanupBridgeFiles();
    Logger::LogInfo("TradeManager: initialized");
}

void Shutdown() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_session = {};
    g_tradeComparison = {};
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
    if (!reason || std::string(reason) != "native_snapshot_processed") {
        g_tradeComparison = {};
    }
}

void Update() {
    const auto now = std::chrono::steady_clock::now();
    bool active = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        active = g_session.state != SessionState::Idle || g_tradeComparison.active;
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
    const bool nativeProcessed = ProcessNativeSnapshot();
    const bool comparisonActive = ProcessPendingTradeComparison(now);
    bool waitingForNative = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        waitingForNative = g_session.state == SessionState::AwaitingSnapshot &&
            g_session.nativePreCaptured &&
            (g_session.nativePostPending || !g_session.nativePostCaptured);
    }
    if (!nativeProcessed && !waitingForNative && !comparisonActive) {
        ProcessDoneSnapshot();
    }
    ExpireStaleSession(now);
}

} // namespace TradeManager
