// NearbyItemsFNV.cpp - Structured nearby item snapshots for DialecticServer prompts

#include "NearbyItemsFNV.h"

#include "Config.h"
#include "HTTPManager.h"
#include "Logger.h"
#include "Misc.h"
#include "NativeComparisonTelemetry.h"
#include "RuntimeGeneration.h"
#include "RuntimeSnapshot.h"
#include "TaskManager.h"
#include "WorldContextFNV.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace NearbyItemsFNV {
namespace {

struct NearbyItem {
    std::string refId;
    std::string baseId;
    std::string name;
    int type = 0;
    float distance = 0.0f;
    std::string cellFormId;
    bool lookingAt = false;
    bool stealing = false;
    bool holding = false;
};

static constexpr const char* kNearbyItemsPath = "Data\\NVSE\\Plugins\\dialectic_nearby_items.tmp";
static constexpr auto kBridgeReadInterval = std::chrono::milliseconds(500);
static constexpr auto kChangeCheckInterval = std::chrono::milliseconds(500);
static std::chrono::steady_clock::time_point g_lastBridgeReadTime;
static std::chrono::steady_clock::time_point g_lastSendTime;
static std::chrono::steady_clock::time_point g_lastCheckTime;
static std::vector<NearbyItem> g_items;
static std::string g_lastSentSignature;
static std::mutex g_mutex;

std::string Trim(const std::string& value) {
    const char* whitespace = " \t\r\n";
    const size_t start = value.find_first_not_of(whitespace);
    if (start == std::string::npos) {
        return "";
    }
    const size_t end = value.find_last_not_of(whitespace);
    return value.substr(start, end - start + 1);
}

std::vector<std::string> Split(const std::string& value, char delimiter) {
    std::vector<std::string> parts;
    std::string part;
    std::istringstream stream(value);
    while (std::getline(stream, part, delimiter)) {
        parts.push_back(part);
    }
    return parts;
}

int ParseInt(const std::string& value, int fallback = 0) {
    try {
        return std::stoi(Trim(value));
    } catch (...) {
        return fallback;
    }
}

float ParseFloat(const std::string& value, float fallback = 0.0f) {
    try {
        return std::stof(Trim(value));
    } catch (...) {
        return fallback;
    }
}

bool ParseBoolFlag(const std::string& value) {
    const std::string cleaned = Trim(value);
    return cleaned == "1" || cleaned == "true" || cleaned == "True" || cleaned == "TRUE";
}

bool GetFileModifiedAgeMs(const char* path, uint64_t& ageMs) {
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
        : static_cast<uint64_t>((current.QuadPart - modified.QuadPart) / 10000);
    return true;
}

bool IsUsableItemName(const std::string& name) {
    const std::string trimmed = Trim(name);
    return !trimmed.empty() && trimmed != "<no name>";
}

std::string FormatHex8(uint32_t value) {
    std::ostringstream out;
    out << "0x" << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << value;
    return out.str();
}

bool SameRefId(const std::string& a, const std::string& b) {
    std::string left = Trim(a);
    std::string right = Trim(b);
    std::transform(left.begin(), left.end(), left.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    std::transform(right.begin(), right.end(), right.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return !left.empty() && left == right;
}

void AppendUnique(std::vector<NearbyItem>& items, const NearbyItem& item) {
    if (item.refId.empty() || !IsUsableItemName(item.name)) {
        return;
    }
    for (NearbyItem& existing : items) {
        if (SameRefId(existing.refId, item.refId)) {
            existing.lookingAt = existing.lookingAt || item.lookingAt;
            existing.stealing = existing.stealing || item.stealing;
            existing.holding = existing.holding || item.holding;
            if (existing.baseId.empty() && !item.baseId.empty()) {
                existing.baseId = item.baseId;
            }
            if (existing.cellFormId.empty() && !item.cellFormId.empty()) {
                existing.cellFormId = item.cellFormId;
            }
            if (existing.distance <= 0.0f || (item.distance > 0.0f && item.distance < existing.distance)) {
                existing.distance = item.distance;
            }
            return;
        }
    }
    items.push_back(item);
}

bool IsInventoryItemType(uint8_t typeId) {
    switch (typeId) {
        case 0x18: // TESObjectARMO
        case 0x19: // TESObjectBOOK
        case 0x1A: // TESObjectCLOT
        case 0x1D: // IngredientItem
        case 0x1E: // TESObjectLIGH
        case 0x1F: // TESObjectMISC
        case 0x28: // TESObjectWEAP
        case 0x29: // TESAmmo
        case 0x2E: // TESKey
        case 0x2F: // AlchemyItem
        case 0x31: // BGSNote
        case 0x32: // BGSConstructibleObject
        case 0x67: // TESObjectIMOD
        case 0x6C: // TESCasinoChips
        case 0x73: // TESCaravanCard
        case 0x74: // TESCaravanMoney
            return true;
        default:
            return false;
    }
}

std::vector<NearbyItem> CollectNativeNearbyItems() {
    std::vector<NearbyItem> items;
    const float maxDistance = Config::nearbyItemsMaxDistance > 0.0f
        ? Config::nearbyItemsMaxDistance
        : 1000.0f;

    for (const RuntimeSnapshot::ReferenceState& reference : RuntimeSnapshot::GetReferences()) {
        if (reference.formId == 0 || reference.baseFormId == 0 || reference.deleted ||
            reference.taken || !reference.loaded3D || !IsInventoryItemType(reference.baseType) ||
            reference.distanceToPlayer > maxDistance || !IsUsableItemName(reference.name)) {
            continue;
        }
        NearbyItem item;
        item.refId = FormatHex8(reference.formId);
        item.baseId = FormatHex8(reference.baseFormId);
        item.name = reference.name;
        item.type = static_cast<int>(reference.baseType);
        item.distance = reference.distanceToPlayer;
        item.cellFormId = reference.cellFormId != 0 ? FormatHex8(reference.cellFormId) : "";
        item.lookingAt = Config::nearbyItemsIncludeLookingAt && reference.crosshair;
        AppendUnique(items, item);
    }

    return items;
}

NearbyItem ParseBridgeItem(const std::vector<std::string>& parts) {
    NearbyItem item;
    item.refId = Trim(parts[0]);
    item.baseId = Trim(parts[1]);
    item.name = Trim(parts[2]);
    item.type = ParseInt(parts[3]);
    item.distance = ParseFloat(parts[4]);
    item.cellFormId = Trim(parts[5]);
    return item;
}

bool LoadBridgeItems(std::vector<NearbyItem>& items) {
    uint64_t ageMs = 0;
    if (!GetFileModifiedAgeMs(kNearbyItemsPath, ageMs) || ageMs > 5000) {
        return false;
    }

    std::ifstream input(kNearbyItemsPath, std::ios::binary);
    if (!input.is_open()) {
        return false;
    }

    NearbyItem heldItem;
    bool hasHeldItem = false;
    std::string line;
    while (std::getline(input, line)) {
        line = Trim(line);

        if (Config::nearbyItemsIncludeHeldItem && line.rfind("held=", 0) == 0) {
            const std::vector<std::string> parts = Split(line.substr(5), '^');
            if (parts.size() >= 6) {
                heldItem = ParseBridgeItem(parts);
                heldItem.holding = true;
                if (!heldItem.refId.empty() && !heldItem.baseId.empty() && IsUsableItemName(heldItem.name)) {
                    hasHeldItem = true;
                }
            }
            continue;
        }

        if (line.rfind("item=", 0) != 0) {
            continue;
        }

        const std::vector<std::string> parts = Split(line.substr(5), '^');
        if (parts.size() < 7) {
            continue;
        }

        NearbyItem item = ParseBridgeItem(parts);
        item.lookingAt = Config::nearbyItemsIncludeLookingAt && ParseBoolFlag(parts[6]);
        if (parts.size() >= 8) {
            item.stealing = Config::nearbyItemsIncludeStealing && ParseBoolFlag(parts[7]);
        }
        if (parts.size() >= 9) {
            item.holding = Config::nearbyItemsIncludeHeldItem && ParseBoolFlag(parts[8]);
        }
        if (hasHeldItem && SameRefId(item.refId, heldItem.refId)) {
            item.holding = Config::nearbyItemsIncludeHeldItem;
            heldItem = item;
        }

        if (!item.refId.empty() && !item.baseId.empty() && IsUsableItemName(item.name)) {
            AppendUnique(items, item);
        }
    }

    if (Config::nearbyItemsIncludeHeldItem && hasHeldItem) {
        bool foundHeld = false;
        for (auto& item : items) {
            if (SameRefId(item.refId, heldItem.refId)) {
                item.holding = true;
                foundHeld = true;
                break;
            }
        }
        if (!foundHeld) {
            AppendUnique(items, heldItem);
        }
    }

    return true;
}

bool RefreshFromBridge() {
    const auto now = std::chrono::steady_clock::now();
    if (g_lastBridgeReadTime.time_since_epoch().count() != 0 &&
        now - g_lastBridgeReadTime < kBridgeReadInterval) {
        return false;
    }
    g_lastBridgeReadTime = now;

    RuntimeSnapshot::GameState nativeState;
    const bool nativeFresh = RuntimeSnapshot::TryGetFreshGameState(
        nativeState, std::chrono::milliseconds(500));
    std::vector<NearbyItem> items = CollectNativeNearbyItems();
    const size_t nativeCount = items.size();
    std::vector<NearbyItem> bridgeItems;
    const bool loadedBridge = LoadBridgeItems(bridgeItems);
    if (nativeFresh && loadedBridge) {
        auto normalizedRef = [](std::string value) {
            value = Trim(value);
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });
            return value;
        };
        std::unordered_set<std::string> nativeRefs;
        std::unordered_set<std::string> bridgeRefs;
        for (const NearbyItem& nativeItem : items) {
            const std::string ref = normalizedRef(nativeItem.refId);
            if (!ref.empty()) nativeRefs.insert(ref);
        }
        for (const NearbyItem& bridgeItem : bridgeItems) {
            if (bridgeItem.holding) continue;
            const std::string ref = normalizedRef(bridgeItem.refId);
            if (!ref.empty()) bridgeRefs.insert(ref);
        }
        std::size_t nativeOnly = 0;
        std::size_t bridgeOnly = 0;
        for (const auto& ref : nativeRefs) if (!bridgeRefs.contains(ref)) ++nativeOnly;
        for (const auto& ref : bridgeRefs) if (!nativeRefs.contains(ref)) ++bridgeOnly;
        std::ostringstream detail;
        detail << "native=" << nativeRefs.size() << " bridge=" << bridgeRefs.size()
               << " native_only=" << nativeOnly << " bridge_only=" << bridgeOnly;
        NativeComparisonTelemetry::Record("nearby_items",
            nativeOnly == 0 && bridgeOnly == 0
                ? NativeComparisonTelemetry::Result::Match
                : NativeComparisonTelemetry::Result::Mismatch,
            detail.str());
    } else if (nativeFresh) {
        NativeComparisonTelemetry::Record(
            "nearby_items", NativeComparisonTelemetry::Result::Unavailable,
            "bridge snapshot unavailable");
    }
    if (nativeFresh) {
        for (const NearbyItem& bridgeItem : bridgeItems) {
            const bool matchesNative = std::any_of(items.begin(), items.end(),
                [&bridgeItem](const NearbyItem& nativeItem) {
                    return SameRefId(nativeItem.refId, bridgeItem.refId);
                });
            if (bridgeItem.holding || matchesNative) {
                AppendUnique(items, bridgeItem);
            }
        }
    } else {
        for (const NearbyItem& bridgeItem : bridgeItems) {
            AppendUnique(items, bridgeItem);
        }
    }
    if (!nativeFresh && !loadedBridge && nativeCount == 0) {
        return false;
    }

    std::sort(items.begin(), items.end(), [](const NearbyItem& a, const NearbyItem& b) {
        if (Config::nearbyItemsHeldItemPriority && a.holding != b.holding) {
            return a.holding;
        }
        return a.distance < b.distance;
    });

    const float maxDistance = Config::nearbyItemsMaxDistance > 0.0f
        ? Config::nearbyItemsMaxDistance
        : 1000.0f;
    items.erase(std::remove_if(items.begin(), items.end(), [maxDistance](const NearbyItem& item) {
        return item.distance > maxDistance;
    }), items.end());

    if (Config::nearbyItemsMaxItems > 0 &&
        items.size() > static_cast<size_t>(Config::nearbyItemsMaxItems)) {
        items.resize(static_cast<size_t>(Config::nearbyItemsMaxItems));
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    g_items = items;
    Logger::LogDebug("NearbyItemsFNV: refreshed items native=%zu bridge=%d final=%zu",
        nativeCount,
        loadedBridge ? 1 : 0,
        items.size());
    return true;
}

std::string BuildSignature(const std::vector<NearbyItem>& items) {
    std::ostringstream signature;
    for (const auto& item : items) {
        signature << item.refId << ":"
                  << static_cast<int>(std::floor(item.distance / 25.0f)) << ":"
                  << (item.lookingAt ? 1 : 0) << ":"
                  << (item.stealing ? 1 : 0) << ":"
                  << (item.holding ? 1 : 0) << "|";
    }
    return signature.str();
}

std::string BuildJson(const std::vector<NearbyItem>& items) {
    const long long localTs = Misc::GetCurrentTimeMillis() / 1000;
    const long long gameTs = WorldContextFNV::GetGameTimestamp();
    std::string playerName = Trim(Misc::GetPlayerName());
    if (playerName.empty()) {
        playerName = Trim(Config::playerName);
    }
    if (playerName.empty()) {
        playerName = "Player";
    }

    std::ostringstream json;
    json << "{";
    json << "\"schema\":\"dialectic.nearby_items.v1\",";
    json << "\"type\":\"nearby_items\",";
    json << "\"game\":\"fnv\",";
    json << "\"ts\":" << localTs << ",";
    json << "\"gamets\":" << (gameTs > 0 ? gameTs : 0) << ",";
    json << "\"player\":\"" << HTTPManager::EscapeJson(playerName) << "\",";
    json << "\"items\":[";

    bool first = true;
    const NearbyItem* heldItem = nullptr;
    for (const auto& item : items) {
        if (item.holding && heldItem == nullptr) {
            heldItem = &item;
        }
        if (!first) {
            json << ",";
        }
        first = false;

        json << "{";
        json << "\"refid\":\"" << HTTPManager::EscapeJson(item.refId) << "\",";
        json << "\"baseid\":\"" << HTTPManager::EscapeJson(item.baseId) << "\",";
        json << "\"name\":\"" << HTTPManager::EscapeJson(item.name) << "\",";
        json << "\"type\":" << item.type << ",";
        json << "\"distance\":" << item.distance << ",";
        json << "\"cell_formid\":\"" << HTTPManager::EscapeJson(item.cellFormId) << "\",";
        json << "\"looking_at\":" << (item.lookingAt ? "true" : "false") << ",";
        json << "\"stealing\":" << (item.stealing ? "true" : "false") << ",";
        json << "\"holding\":" << (item.holding ? "true" : "false");
        json << "}";
    }

    json << "]";
    if (heldItem != nullptr) {
        json << ",\"held_item\":{";
        json << "\"refid\":\"" << HTTPManager::EscapeJson(heldItem->refId) << "\",";
        json << "\"baseid\":\"" << HTTPManager::EscapeJson(heldItem->baseId) << "\",";
        json << "\"name\":\"" << HTTPManager::EscapeJson(heldItem->name) << "\",";
        json << "\"type\":" << heldItem->type << ",";
        json << "\"distance\":" << heldItem->distance << ",";
        json << "\"cell_formid\":\"" << HTTPManager::EscapeJson(heldItem->cellFormId) << "\"";
        json << "}";
    }
    json << "}";
    return json.str();
}

void SendItems(std::vector<NearbyItem> items) {
    const std::string json = BuildJson(items);
    TaskManager::Enqueue("gamedata", "nearby_items", RuntimeGeneration::Current(), false,
        std::chrono::seconds(30), [json, count = items.size()](const TaskManager::CancellationToken& token) {
        if (token.IsCancellationRequested()) return;
        const auto start = std::chrono::steady_clock::now();
        const std::string response = HTTPManager::SendJson("gamedata.php", json);
        const long long elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        Logger::LogInfo("[PERF] NearbyItemsFNV send elapsed_ms=%lld items=%zu bytes=%zu response_empty=%d",
            elapsedMs,
            count,
            json.size(),
            response.empty() ? 1 : 0);
        if (response.empty()) {
            Logger::LogWarning("NearbyItemsFNV: nearby_items update for %zu items returned empty response", count);
        }
    });
}

} // namespace

void SendNow(bool force) {
    if (!Config::nearbyItemsEnabled) {
        return;
    }

    RefreshFromBridge();
    std::vector<NearbyItem> items;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        items = g_items;
    }

    const std::string signature = BuildSignature(items);
    std::lock_guard<std::mutex> lock(g_mutex);
    const bool changed = signature != g_lastSentSignature;
    if (!force && Config::nearbyItemsSendOnChange && !changed) {
        return;
    }

    g_lastSendTime = std::chrono::steady_clock::now();
    g_lastSentSignature = signature;
    SendItems(items);
}

void Update() {
    if (!Config::nearbyItemsEnabled) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const float configuredSeconds = Config::nearbyItemsUpdateSeconds > 0.5f
        ? Config::nearbyItemsUpdateSeconds
        : 5.0f;
    const auto heartbeat = std::chrono::milliseconds(static_cast<int>(configuredSeconds * 1000.0f));

    bool due = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        due = g_lastSendTime.time_since_epoch().count() == 0 ||
            now - g_lastSendTime >= heartbeat;
    }

    if (due) {
        SendNow(false);
    }
}

} // namespace NearbyItemsFNV
