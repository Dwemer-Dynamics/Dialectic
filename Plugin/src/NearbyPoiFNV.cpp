// NearbyPoiFNV.cpp - Structured points of interest snapshots for DialecticServer prompts

#include "NearbyPoiFNV.h"

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

namespace NearbyPoiFNV {
namespace {

struct Poi {
    std::string refId;
    std::string baseId;
    std::string name;
    std::string destinationCellId;
    std::string destinationName;
    std::string cellFormId;
    float distance = 0.0f;
    int openState = 0;
    bool lookingAt = false;
    bool locked = false;
};

static constexpr const char* kPoiPath = "Data\\NVSE\\Plugins\\dialectic_nearby_pois.tmp";
static constexpr auto kBridgeReadInterval = std::chrono::milliseconds(500);
static constexpr auto kChangeCheckInterval = std::chrono::milliseconds(500);
static std::chrono::steady_clock::time_point g_lastBridgeReadTime;
static std::chrono::steady_clock::time_point g_lastSendTime;
static std::chrono::steady_clock::time_point g_lastCheckTime;
static std::vector<Poi> g_pois;
static std::string g_lastSentSignature;
static std::mutex g_mutex;

std::string Trim(const std::string& value) {
    const char* whitespace = " \t\r\n";
    const size_t start = value.find_first_not_of(whitespace);
    if (start == std::string::npos) {
        return "";
    }
    const size_t end = value.find_last_not_of(whitespace);
    std::string trimmed = value.substr(start, end - start + 1);
    while (trimmed.size() >= 2) {
        const std::string suffix = trimmed.substr(trimmed.size() - 2);
        if (suffix != "\\n" && suffix != "\\r") {
            break;
        }
        trimmed.erase(trimmed.size() - 2);
        const size_t nextEnd = trimmed.find_last_not_of(whitespace);
        if (nextEnd == std::string::npos) {
            return "";
        }
        trimmed.erase(nextEnd + 1);
    }
    return trimmed;
}

std::string DecodeEscapedLineBreaks(std::string value) {
    size_t pos = 0;
    while ((pos = value.find("\\r", pos)) != std::string::npos) {
        value.replace(pos, 2, "\r");
        ++pos;
    }

    pos = 0;
    while ((pos = value.find("\\n", pos)) != std::string::npos) {
        value.replace(pos, 2, "\n");
        ++pos;
    }

    return value;
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

bool IsUsablePoi(const Poi& poi) {
    return !poi.refId.empty() && (!poi.destinationName.empty() || !poi.name.empty());
}

std::string DisplayName(const Poi& poi) {
    if (!poi.destinationName.empty() && poi.destinationName != "Unknown") {
        return poi.destinationName;
    }
    if (!poi.name.empty() && poi.name != "Unknown") {
        return poi.name;
    }
    return poi.refId;
}

std::string FormatHex8(uint32_t value) {
    std::ostringstream out;
    out << "0x" << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << value;
    return out.str();
}

std::vector<Poi> CollectNativePois() {
    std::vector<Poi> pois;
    const float maxDistance = Config::pointsOfInterestMaxDistance > 0.0f
        ? Config::pointsOfInterestMaxDistance : 1600.0f;
    for (const RuntimeSnapshot::ReferenceState& reference : RuntimeSnapshot::GetReferences()) {
        if (reference.baseType != 0x1C || reference.formId == 0 || reference.baseFormId == 0 ||
            reference.deleted || reference.taken || !reference.loaded3D ||
            reference.distanceToPlayer > maxDistance) {
            continue;
        }
        Poi poi;
        poi.refId = FormatHex8(reference.formId);
        poi.baseId = FormatHex8(reference.baseFormId);
        poi.name = reference.name.empty() ? "Door" : reference.name;
        poi.destinationCellId = reference.destinationCellFormId != 0
            ? FormatHex8(reference.destinationCellFormId) : "";
        poi.destinationName = reference.destinationName;
        poi.cellFormId = reference.cellFormId != 0 ? FormatHex8(reference.cellFormId) : "";
        poi.distance = reference.distanceToPlayer;
        poi.openState = reference.openStateKnown ? reference.openState : 0;
        poi.locked = Config::pointsOfInterestIncludeLocked && reference.locked;
        poi.lookingAt = Config::pointsOfInterestIncludeLookingAt && reference.crosshair;
        pois.push_back(std::move(poi));
    }
    return pois;
}

void MergePoi(std::vector<Poi>& pois, Poi incoming) {
    for (Poi& existing : pois) {
        if (existing.refId == incoming.refId) {
            if (!incoming.name.empty() && incoming.name != "Unknown") existing.name = incoming.name;
            if (!incoming.destinationCellId.empty() && incoming.destinationCellId != "Unknown") {
                existing.destinationCellId = incoming.destinationCellId;
            }
            if (!incoming.destinationName.empty() && incoming.destinationName != "Unknown") {
                existing.destinationName = incoming.destinationName;
            }
            existing.openState = incoming.openState;
            existing.locked = incoming.locked;
            existing.lookingAt = existing.lookingAt || incoming.lookingAt;
            return;
        }
    }
    pois.push_back(std::move(incoming));
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
    std::vector<Poi> pois = CollectNativePois();
    std::vector<Poi> bridgePois;
    uint64_t ageMs = 0;
    std::ifstream input;
    if (GetFileModifiedAgeMs(kPoiPath, ageMs) && ageMs <= 5000) {
        input.open(kPoiPath, std::ios::binary);
    }
    std::ostringstream buffer;
    if (input.is_open()) {
        buffer << input.rdbuf();
    }

    std::string line;
    std::istringstream lines(DecodeEscapedLineBreaks(buffer.str()));
    while (input.is_open() && std::getline(lines, line)) {
        line = Trim(line);
        if (line.rfind("poi=", 0) != 0) {
            continue;
        }

        const std::vector<std::string> parts = Split(line.substr(4), '^');
        if (parts.size() < 10) {
            continue;
        }

        Poi poi;
        poi.refId = Trim(parts[0]);
        poi.baseId = Trim(parts[1]);
        poi.name = Trim(parts[2]);
        poi.destinationCellId = Trim(parts[3]);
        poi.destinationName = Trim(parts[4]);
        poi.distance = ParseFloat(parts[5]);
        poi.cellFormId = Trim(parts[6]);
        poi.openState = ParseInt(parts[7]);
        poi.lookingAt = Config::pointsOfInterestIncludeLookingAt && ParseBoolFlag(parts[8]);
        poi.locked = Config::pointsOfInterestIncludeLocked && ParseBoolFlag(parts[9]);

        if (IsUsablePoi(poi)) {
            MergePoi(bridgePois, std::move(poi));
        }
    }

    if (nativeFresh) {
        if (input.is_open()) {
            auto normalizedRef = [](std::string value) {
                value = Trim(value);
                std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
                    return static_cast<char>(std::tolower(ch));
                });
                return value;
            };
            std::unordered_set<std::string> nativeRefs;
            std::unordered_set<std::string> bridgeRefs;
            for (const Poi& poi : pois) {
                const std::string ref = normalizedRef(poi.refId);
                if (!ref.empty()) nativeRefs.insert(ref);
            }
            for (const Poi& poi : bridgePois) {
                const std::string ref = normalizedRef(poi.refId);
                if (!ref.empty()) bridgeRefs.insert(ref);
            }
            std::size_t nativeOnly = 0;
            std::size_t bridgeOnly = 0;
            for (const auto& ref : nativeRefs) if (!bridgeRefs.contains(ref)) ++nativeOnly;
            for (const auto& ref : bridgeRefs) if (!nativeRefs.contains(ref)) ++bridgeOnly;
            std::ostringstream detail;
            detail << "native=" << nativeRefs.size() << " bridge=" << bridgeRefs.size()
                   << " native_only=" << nativeOnly << " bridge_only=" << bridgeOnly;
            NativeComparisonTelemetry::Record("points_of_interest",
                nativeOnly == 0 && bridgeOnly == 0
                    ? NativeComparisonTelemetry::Result::Match
                    : NativeComparisonTelemetry::Result::Mismatch,
                detail.str());
        } else {
            NativeComparisonTelemetry::Record(
                "points_of_interest", NativeComparisonTelemetry::Result::Unavailable,
                "bridge snapshot unavailable");
        }
    } else {
        for (Poi& poi : bridgePois) MergePoi(pois, std::move(poi));
    }

    if (!nativeFresh && pois.empty()) {
        return false;
    }

    std::sort(pois.begin(), pois.end(), [](const Poi& a, const Poi& b) {
        if (a.lookingAt != b.lookingAt) {
            return a.lookingAt;
        }
        return a.distance < b.distance;
    });

    const float maxDistance = Config::pointsOfInterestMaxDistance > 0.0f
        ? Config::pointsOfInterestMaxDistance
        : 1600.0f;
    pois.erase(std::remove_if(pois.begin(), pois.end(), [maxDistance](const Poi& poi) {
        return poi.distance > maxDistance;
    }), pois.end());

    std::vector<Poi> deduped;
    std::vector<std::string> seen;
    for (const auto& poi : pois) {
        std::string key = DisplayName(poi);
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (std::find(seen.begin(), seen.end(), key) != seen.end()) {
            continue;
        }
        seen.push_back(key);
        deduped.push_back(poi);
    }

    if (Config::pointsOfInterestMaxPois > 0 &&
        deduped.size() > static_cast<size_t>(Config::pointsOfInterestMaxPois)) {
        deduped.resize(static_cast<size_t>(Config::pointsOfInterestMaxPois));
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    g_pois = deduped;
    return true;
}

std::string BuildSignature(const std::vector<Poi>& pois) {
    std::ostringstream signature;
    for (const auto& poi : pois) {
        signature << poi.refId << ":"
                  << DisplayName(poi) << ":"
                  << static_cast<int>(std::floor(poi.distance / 25.0f)) << ":"
                  << (poi.lookingAt ? 1 : 0) << ":"
                  << (poi.locked ? 1 : 0) << "|";
    }
    return signature.str();
}

std::string BuildJson(const std::vector<Poi>& pois) {
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
    json << "\"schema\":\"dialectic.points_of_interest.v1\",";
    json << "\"type\":\"points_of_interest\",";
    json << "\"game\":\"fnv\",";
    json << "\"ts\":" << localTs << ",";
    json << "\"gamets\":" << (gameTs > 0 ? gameTs : 0) << ",";
    json << "\"player\":\"" << HTTPManager::EscapeJson(playerName) << "\",";
    json << "\"pois\":[";

    bool first = true;
    for (const auto& poi : pois) {
        if (!first) {
            json << ",";
        }
        first = false;

        json << "{";
        json << "\"refid\":\"" << HTTPManager::EscapeJson(poi.refId) << "\",";
        json << "\"baseid\":\"" << HTTPManager::EscapeJson(poi.baseId) << "\",";
        json << "\"name\":\"" << HTTPManager::EscapeJson(poi.name) << "\",";
        json << "\"destination_cellid\":\"" << HTTPManager::EscapeJson(poi.destinationCellId) << "\",";
        json << "\"destination_name\":\"" << HTTPManager::EscapeJson(poi.destinationName) << "\",";
        json << "\"display_name\":\"" << HTTPManager::EscapeJson(DisplayName(poi)) << "\",";
        json << "\"distance\":" << poi.distance << ",";
        json << "\"cell_formid\":\"" << HTTPManager::EscapeJson(poi.cellFormId) << "\",";
        json << "\"open_state\":" << poi.openState << ",";
        json << "\"is_door\":true,";
        json << "\"looking_at\":" << (poi.lookingAt ? "true" : "false") << ",";
        json << "\"locked\":" << (poi.locked ? "true" : "false");
        json << "}";
    }

    json << "]";
    json << "}";
    return json.str();
}

void SendPois(std::vector<Poi> pois) {
    const std::string json = BuildJson(pois);
    TaskManager::Enqueue("gamedata", "points_of_interest", RuntimeGeneration::Current(), false,
        std::chrono::seconds(30), [json, count = pois.size()](const TaskManager::CancellationToken& token) {
        if (token.IsCancellationRequested()) return;
        const auto start = std::chrono::steady_clock::now();
        const std::string response = HTTPManager::SendJson("gamedata.php", json);
        const long long elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        Logger::LogInfo("[PERF] NearbyPoiFNV send elapsed_ms=%lld pois=%zu bytes=%zu response_empty=%d",
            elapsedMs,
            count,
            json.size(),
            response.empty() ? 1 : 0);
        if (response.empty()) {
            Logger::LogWarning("NearbyPoiFNV: points_of_interest update for %zu POIs returned empty response", count);
        }
    });
}

} // namespace

void SendNow(bool force) {
    if (!Config::pointsOfInterestEnabled || !Config::pointsOfInterestIncludeDoors) {
        return;
    }

    RefreshFromBridge();
    std::vector<Poi> pois;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        pois = g_pois;
    }

    const std::string signature = BuildSignature(pois);
    std::lock_guard<std::mutex> lock(g_mutex);
    const bool changed = signature != g_lastSentSignature;
    if (!force && Config::pointsOfInterestSendOnChange && !changed) {
        return;
    }

    g_lastSendTime = std::chrono::steady_clock::now();
    g_lastSentSignature = signature;
    SendPois(pois);
}

void Update() {
    if (!Config::pointsOfInterestEnabled || !Config::pointsOfInterestIncludeDoors) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const float configuredSeconds = Config::pointsOfInterestUpdateSeconds > 0.5f
        ? Config::pointsOfInterestUpdateSeconds
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

} // namespace NearbyPoiFNV
