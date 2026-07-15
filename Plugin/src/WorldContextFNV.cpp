// WorldContextFNV.cpp - FNV world context bridge for Dialectic prompts

#include "WorldContextFNV.h"

#include "Config.h"
#include "HTTPManager.h"
#include "Logger.h"
#include "Misc.h"
#include "RuntimeGeneration.h"
#include "RuntimeSnapshot.h"
#include "TaskManager.h"

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

namespace WorldContextFNV {
namespace {

static constexpr const char* kWorldContextPath = "Data\\NVSE\\Plugins\\dialectic_world_context.tmp";
static std::mutex g_contextMutex;
static Context g_context;
static std::chrono::steady_clock::time_point g_lastBridgeReadTime;
static std::chrono::steady_clock::time_point g_lastSendTime;
static std::string g_lastSentSignature;
static constexpr auto kBridgeReadInterval = std::chrono::milliseconds(500);

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

std::string ToLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool IsUnknown(const std::string& value) {
    const std::string lower = ToLower(Trim(value));
    return lower.empty() || lower == "unknown" || lower == "none" || lower == "null";
}

bool ParseBool(const std::string& value, bool fallback = false) {
    const std::string cleaned = ToLower(Trim(value));
    if (cleaned == "1" || cleaned == "true" || cleaned == "yes") {
        return true;
    }
    if (cleaned == "0" || cleaned == "false" || cleaned == "no") {
        return false;
    }
    return fallback;
}

long long DaysFromCivil(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(year - era * 400);
    const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<long long>(era) * 146097LL + static_cast<long long>(doe) - 719468LL;
}

long long BuildGametsFromCalendar(int year, int month, int day, float hour) {
    if (year <= 0 || month < 1 || month > 12 || day < 1 || day > 31) {
        return 0;
    }

    const long long startDay = DaysFromCivil(2281, 10, 19);
    const long long currentDay = DaysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
    const double days = static_cast<double>(currentDay - startDay) + (static_cast<double>(hour) / 24.0);
    if (days <= 0.0) {
        return 0;
    }
    return static_cast<long long>(std::llround(days * 10000000.0));
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

long long ParseLongLong(const std::string& value, long long fallback = 0) {
    try {
        return std::stoll(Trim(value));
    } catch (...) {
        return fallback;
    }
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

std::string NormalizeWeather(const Context& context) {
    const std::string raw = ToLower(context.weatherEditorId + " " + context.weatherName);
    if (raw.find("rain") != std::string::npos || raw.find("storm") != std::string::npos) {
        return context.isInterior ? "outdoors it is Raining" : "Raining";
    }
    if (raw.find("snow") != std::string::npos) {
        return context.isInterior ? "outdoors it is Snowing" : "Snowing";
    }
    if (raw.find("fog") != std::string::npos || raw.find("dust") != std::string::npos) {
        return context.isInterior ? "outdoors it is Foggy" : "Foggy";
    }
    if (raw.find("cloud") != std::string::npos || raw.find("overcast") != std::string::npos) {
        return context.isInterior ? "outdoors it is Cloudy" : "Cloudy";
    }
    if (raw.find("clear") != std::string::npos || raw.find("pleasant") != std::string::npos ||
        raw.find("wasteland") != std::string::npos || raw.find("default") != std::string::npos) {
        return context.isInterior ? "outdoors it is Pleasant" : "Pleasant";
    }

    if (!IsUnknown(context.weatherName)) {
        return context.isInterior
            ? "outdoors it is " + context.weatherName
            : context.weatherName;
    }
    if (!IsUnknown(context.weatherEditorId)) {
        return context.isInterior
            ? "outdoors it is " + context.weatherEditorId
            : context.weatherEditorId;
    }
    return "";
}

void ApplyKeyValue(Context& context, const std::string& key, const std::string& value) {
    if (key == "location" && Config::worldContextIncludeCell) {
        context.location = Trim(value);
    } else if (key == "cell_formid" && Config::worldContextIncludeCell) {
        context.cellFormId = Trim(value);
    } else if (key == "worldspace" && Config::worldContextIncludeWorldspace) {
        context.worldspace = Trim(value);
    } else if (key == "worldspace_formid" && Config::worldContextIncludeWorldspace) {
        context.worldspaceFormId = Trim(value);
    } else if (key == "is_interior") {
        context.isInterior = ParseBool(value);
        context.interiorKnown = true;
    } else if (key == "weather_name" && Config::worldContextIncludeWeather) {
        context.weatherName = Trim(value);
    } else if (key == "weather_editorid" && Config::worldContextIncludeWeather) {
        context.weatherEditorId = Trim(value);
    } else if (key == "weather_formid" && Config::worldContextIncludeWeather) {
        context.weatherFormId = Trim(value);
    } else if (key == "game_year") {
        context.gameYear = ParseInt(value);
    } else if (key == "game_month") {
        context.gameMonth = ParseInt(value);
    } else if (key == "game_day") {
        context.gameDay = ParseInt(value);
    } else if (key == "game_hour") {
        context.gameHour = ParseFloat(value);
    } else if (key == "game_days_passed") {
        context.gameDaysPassed = ParseFloat(value);
    } else if (key == "gamets") {
        context.gamets = ParseLongLong(value);
    } else if (key == "player_x") {
        context.playerX = ParseFloat(value);
        context.playerPositionKnown = true;
    } else if (key == "player_y") {
        context.playerY = ParseFloat(value);
        context.playerPositionKnown = true;
    } else if (key == "player_z") {
        context.playerZ = ParseFloat(value);
        context.playerPositionKnown = true;
    }
}

bool RefreshFromBridge() {
    const auto now = std::chrono::steady_clock::now();
    if (g_lastBridgeReadTime.time_since_epoch().count() != 0 &&
        now - g_lastBridgeReadTime < kBridgeReadInterval) {
        return false;
    }
    g_lastBridgeReadTime = now;

    uint64_t ageMs = 0;
    if (!GetFileModifiedAgeMs(kWorldContextPath, ageMs) || ageMs > 5000) {
        return false;
    }

    std::ifstream input(kWorldContextPath, std::ios::binary);
    if (!input.is_open()) {
        return false;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    std::istringstream lines(DecodeEscapedLineBreaks(buffer.str()));

    Context next;
    std::string line;
    while (std::getline(lines, line)) {
        line = Trim(line);
        if (line.empty()) {
            continue;
        }
        const size_t equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        ApplyKeyValue(next, Trim(line.substr(0, equals)), line.substr(equals + 1));
    }

    if (IsUnknown(next.location)) {
        next.location = !IsUnknown(next.worldspace) ? next.worldspace : "Unknown Location";
    }
    next.weather = NormalizeWeather(next);
    const long long calendarGamets = BuildGametsFromCalendar(
        next.gameYear,
        next.gameMonth,
        next.gameDay,
        next.gameHour);
    if (calendarGamets > 0) {
        next.gamets = calendarGamets;
    } else if (next.gamets <= 0 && next.gameDaysPassed > 0.0f) {
        next.gamets = static_cast<long long>(std::llround(next.gameDaysPassed * 10000000.0f));
    }
    next.resolved = !IsUnknown(next.location) || next.gamets > 0;

    std::lock_guard<std::mutex> lock(g_contextMutex);
    g_context = next;
    return next.resolved;
}

std::string FormatNativeFormId(std::uint32_t formId) {
    if (formId == 0) return {};
    std::ostringstream stream;
    stream << "0x" << std::uppercase << std::hex << std::setw(8)
           << std::setfill('0') << formId;
    return stream.str();
}

bool ApplyNativeSnapshot() {
    RuntimeSnapshot::GameState native;
    if (!RuntimeSnapshot::TryGetFreshGameState(native, std::chrono::milliseconds(500))) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_contextMutex);
    if (Config::worldContextIncludeCell) {
        g_context.cellFormId = FormatNativeFormId(native.cellFormId);
        if (!native.cellName.empty()) {
            g_context.location = native.cellName;
        } else if (!native.worldspaceName.empty()) {
            g_context.location = native.worldspaceName;
        }
    }
    if (Config::worldContextIncludeWorldspace) {
        g_context.worldspaceFormId = FormatNativeFormId(native.worldspaceFormId);
        g_context.worldspace = native.worldspaceName;
    }
    g_context.isInterior = native.cellFormId != 0 && native.worldspaceFormId == 0;
    g_context.interiorKnown = native.cellFormId != 0;
    g_context.playerX = native.playerX;
    g_context.playerY = native.playerY;
    g_context.playerZ = native.playerZ;
    g_context.playerPositionKnown = native.player3DLoaded;
    g_context.resolved = native.inGame && (native.cellFormId != 0 || native.worldspaceFormId != 0);
    g_context.weather = NormalizeWeather(g_context);
    return true;
}

std::string BuildSignature(const Context& context) {
    std::ostringstream signature;
    signature << context.location << "|"
              << context.cellFormId << "|"
              << context.worldspace << "|"
              << context.worldspaceFormId << "|"
              << (context.isInterior ? 1 : 0) << "|"
              << context.weather << "|"
              << context.gameYear << "|"
              << context.gameMonth << "|"
              << context.gameDay;
    return signature.str();
}

std::string BuildJson(const Context& context) {
    const long long localTs = Misc::GetCurrentTimeMillis() / 1000;
    std::ostringstream json;
    json << "{";
    json << "\"schema\":\"dialectic.world_context.v1\",";
    json << "\"type\":\"world_context\",";
    json << "\"game\":\"fnv\",";
    json << "\"location\":\"" << HTTPManager::EscapeJson(context.location) << "\",";
    json << "\"cell_formid\":\"" << HTTPManager::EscapeJson(context.cellFormId) << "\",";
    json << "\"worldspace\":\"" << HTTPManager::EscapeJson(context.worldspace) << "\",";
    json << "\"worldspace_formid\":\"" << HTTPManager::EscapeJson(context.worldspaceFormId) << "\",";
    json << "\"is_interior\":" << (context.isInterior ? "true" : "false") << ",";
    json << "\"weather\":\"" << HTTPManager::EscapeJson(context.weather) << "\",";
    json << "\"weather_name\":\"" << HTTPManager::EscapeJson(context.weatherName) << "\",";
    json << "\"weather_editorid\":\"" << HTTPManager::EscapeJson(context.weatherEditorId) << "\",";
    json << "\"weather_formid\":\"" << HTTPManager::EscapeJson(context.weatherFormId) << "\",";
    json << "\"gamets\":" << context.gamets << ",";
    json << "\"ts\":" << localTs << ",";
    json << "\"player_position\":{";
    json << "\"x\":" << context.playerX << ",";
    json << "\"y\":" << context.playerY << ",";
    json << "\"z\":" << context.playerZ << ",";
    json << "\"known\":" << (context.playerPositionKnown ? "true" : "false");
    json << "},";
    json << "\"game_time\":{";
    json << "\"year\":" << context.gameYear << ",";
    json << "\"month\":" << context.gameMonth << ",";
    json << "\"day\":" << context.gameDay << ",";
    json << "\"hour\":" << context.gameHour << ",";
    json << "\"days_passed\":" << context.gameDaysPassed;
    json << "}";
    json << "}";
    return json.str();
}

void SendContext(Context context) {
    const std::string json = BuildJson(context);
    TaskManager::Enqueue("gamedata", "world_context", RuntimeGeneration::Current(), false,
        std::chrono::seconds(30), [json](const TaskManager::CancellationToken& token) {
        if (token.IsCancellationRequested()) return;
        const auto start = std::chrono::steady_clock::now();
        const std::string response = HTTPManager::SendJson("gamedata.php", json);
        const long long elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        Logger::LogDebug("[PERF] WorldContextFNV send elapsed_ms=%lld bytes=%zu response_empty=%d",
            elapsedMs,
            json.size(),
            response.empty() ? 1 : 0);
        if (response.empty()) {
            Logger::LogWarning("WorldContextFNV: world_context update returned empty response");
        }
    });
}

} // namespace

Context GetCurrent() {
    RefreshFromBridge();
    ApplyNativeSnapshot();
    std::lock_guard<std::mutex> lock(g_contextMutex);
    return g_context;
}

std::string GetPlayerLocation() {
    const Context context = GetCurrent();
    return context.location;
}

long long GetGameTimestamp() {
    const Context context = GetCurrent();
    return context.gamets;
}

void SendNow(bool force) {
    if (!Config::worldContextEnabled) {
        return;
    }

    RefreshFromBridge();
    ApplyNativeSnapshot();
    Context context;
    {
        std::lock_guard<std::mutex> lock(g_contextMutex);
        context = g_context;
    }
    if (!context.resolved) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const std::string signature = BuildSignature(context);
    const bool changed = signature != g_lastSentSignature;
    if (!force && Config::worldContextSendOnChange && !changed) {
        return;
    }

    g_lastSendTime = now;
    g_lastSentSignature = signature;
    SendContext(context);
}

void Update() {
    if (!Config::worldContextEnabled) {
        return;
    }

    RefreshFromBridge();
    ApplyNativeSnapshot();
    const auto now = std::chrono::steady_clock::now();
    const float configuredSeconds = Config::worldContextUpdateSeconds > 0.5f
        ? Config::worldContextUpdateSeconds
        : 10.0f;
    const auto heartbeat = std::chrono::milliseconds(static_cast<int>(configuredSeconds * 1000.0f));
    const bool due = g_lastSendTime.time_since_epoch().count() == 0 ||
        now - g_lastSendTime >= heartbeat;

    Context context;
    {
        std::lock_guard<std::mutex> lock(g_contextMutex);
        context = g_context;
    }
    if (!context.resolved) {
        return;
    }

    const std::string signature = BuildSignature(context);
    const bool changed = signature != g_lastSentSignature;
    if (due || (Config::worldContextSendOnChange && changed)) {
        g_lastSendTime = now;
        g_lastSentSignature = signature;
        SendContext(context);
    }
}

} // namespace WorldContextFNV
