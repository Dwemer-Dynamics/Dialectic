// WorldContextFNV.cpp - FNV world context bridge for Dialectic prompts

#include "WorldContextFNV.h"

#include "Config.h"
#include "GameLoop.h"
#include "HTTPManager.h"
#include "Logger.h"
#include "Misc.h"
#include "RuntimeGeneration.h"
#include "RuntimeSnapshot.h"
#include "TaskManager.h"
#include "XNVSEAdapter.h"

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
#include <utility>

namespace WorldContextFNV {
namespace {

static constexpr const char* kWorldContextPath = "Data\\NVSE\\Plugins\\dialectic_world_context.tmp";
static std::mutex g_contextMutex;
static Context g_context;
static std::chrono::steady_clock::time_point g_lastBridgeReadTime;
static std::chrono::steady_clock::time_point g_lastRadioReadTime;
static std::chrono::steady_clock::time_point g_lastSendTime;
static std::string g_lastSentSignature;
static std::string g_lastCommentLocation;
static constexpr auto kBridgeReadInterval = std::chrono::milliseconds(500);
static constexpr auto kRadioReadInterval = std::chrono::milliseconds(500);
static bool g_saveLoadPending = false;
static bool g_saveLoadCompleted = false;
static uint64_t g_bridgeWriteFloor = 0;

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

std::string SongTitleFromPath(const std::string& path) {
    const size_t separator = path.find_last_of("\\/");
    std::string title = separator == std::string::npos ? path : path.substr(separator + 1);
    const size_t extension = title.find_last_of('.');
    if (extension != std::string::npos) {
        title.erase(extension);
    }
    if (title.size() >= 4 && ToLower(title.substr(0, 4)) == "mus_") {
        title.erase(0, 4);
    }

    for (char& character : title) {
        if (character == '_' || character == '-') {
            character = ' ';
        }
    }

    std::string normalized;
    normalized.reserve(title.size());
    bool previousWhitespace = false;
    for (unsigned char character : title) {
        const bool whitespace = std::isspace(character) != 0;
        if (!whitespace || !previousWhitespace) {
            normalized.push_back(whitespace ? ' ' : static_cast<char>(character));
        }
        previousWhitespace = whitespace;
    }
    normalized = Trim(normalized);
    if (normalized.size() > 160) {
        normalized.resize(160);
        normalized = Trim(normalized);
    }
    return normalized;
}

bool IsUnknown(const std::string& value) {
    const std::string lower = ToLower(Trim(value));
    return lower.empty() || lower == "unknown" || lower == "none" || lower == "null";
}

bool IsCommentableLocation(const std::string& value) {
    return !IsUnknown(value) && ToLower(Trim(value)) != "unknown location";
}

void QueueLocationChangedComment(const Context& context) {
    if (!IsCommentableLocation(context.location)) {
        return;
    }
    if (g_lastCommentLocation.empty()) {
        g_lastCommentLocation = context.location;
        return;
    }
    if (context.location == g_lastCommentLocation) {
        return;
    }
    const std::string previousLocation = g_lastCommentLocation;
    g_lastCommentLocation = context.location;
    GameLoop::QueueRpgCommentEvent("location_changed",
        "The group entered " + context.location + " after leaving " + previousLocation);
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

bool GetFileModifiedInfo(const char* path, uint64_t& ageMs, uint64_t& modifiedTicks) {
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
    modifiedTicks = modified.QuadPart;

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
    uint64_t modifiedTicks = 0;
    if (!GetFileModifiedInfo(kWorldContextPath, ageMs, modifiedTicks) || ageMs > 5000) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(g_contextMutex);
        if (g_saveLoadPending &&
            (!g_saveLoadCompleted || modifiedTicks <= g_bridgeWriteFloor)) {
            return false;
        }
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
    // The script bridge has no radio fields, so retain native telemetry until its next sample.
    next.radioActive = g_context.radioActive;
    next.radioStation = g_context.radioStation;
    next.radioStationFormId = g_context.radioStationFormId;
    next.radioSong = g_context.radioSong;
    g_context = std::move(next);
    if (g_saveLoadPending) {
        g_saveLoadPending = false;
        g_saveLoadCompleted = false;
        g_bridgeWriteFloor = 0;
        Logger::LogInfo("WorldContextFNV: accepted fresh post-load bridge gamets=%lld", next.gamets);
    }
    return next.resolved;
}

std::string FormatNativeFormId(std::uint32_t formId) {
    if (formId == 0) return {};
    std::ostringstream stream;
    stream << "0x" << std::uppercase << std::hex << std::setw(8)
           << std::setfill('0') << formId;
    return stream.str();
}

bool ApplyNativeSnapshot(bool forceRadio = false) {
    RuntimeSnapshot::GameState native;
    if (!RuntimeSnapshot::TryGetFreshGameState(native, std::chrono::milliseconds(500))) {
        return false;
    }

    XNVSEAdapter::NativeRadioState radio;
    bool radioCaptured = false;
    const auto now = std::chrono::steady_clock::now();
    if (forceRadio || g_lastRadioReadTime.time_since_epoch().count() == 0 ||
        now - g_lastRadioReadTime >= kRadioReadInterval) {
        g_lastRadioReadTime = now;
        radioCaptured = XNVSEAdapter::CaptureNativeRadioState(radio);
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
    if (radioCaptured && radio.valid) {
        g_context.radioActive = radio.active;
        g_context.radioStation = radio.active ? Trim(radio.stationName) : "";
        g_context.radioStationFormId = radio.active ? FormatNativeFormId(radio.stationFormId) : "";
        g_context.radioSong = radio.active ? SongTitleFromPath(radio.trackPath) : "";
    }
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
              << context.gameDay << "|"
              << (context.radioActive ? 1 : 0) << "|"
              << context.radioStation << "|"
              << context.radioStationFormId << "|"
              << context.radioSong;
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
    json << "\"radio\":{";
    json << "\"active\":" << (context.radioActive ? "true" : "false") << ",";
    json << "\"station\":\"" << HTTPManager::EscapeJson(context.radioStation) << "\",";
    json << "\"station_formid\":\"" << HTTPManager::EscapeJson(context.radioStationFormId) << "\",";
    json << "\"song\":\"" << HTTPManager::EscapeJson(context.radioSong) << "\"";
    json << "},";
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

void BeginSaveLoad() {
    uint64_t ageMs = 0;
    uint64_t modifiedTicks = 0;
    GetFileModifiedInfo(kWorldContextPath, ageMs, modifiedTicks);

    std::lock_guard<std::mutex> lock(g_contextMutex);
    g_context = {};
    g_saveLoadPending = true;
    g_saveLoadCompleted = false;
    g_bridgeWriteFloor = modifiedTicks;
    g_lastBridgeReadTime = {};
    g_lastRadioReadTime = {};
    g_lastSendTime = {};
    g_lastSentSignature.clear();
    g_lastCommentLocation.clear();
    Logger::LogInfo("WorldContextFNV: waiting for fresh bridge after save load");
}

void CompleteSaveLoad(bool succeeded) {
    uint64_t ageMs = 0;
    uint64_t modifiedTicks = 0;
    GetFileModifiedInfo(kWorldContextPath, ageMs, modifiedTicks);

    std::lock_guard<std::mutex> lock(g_contextMutex);
    if (!succeeded) {
        g_saveLoadPending = false;
        g_saveLoadCompleted = false;
        g_bridgeWriteFloor = 0;
        g_lastBridgeReadTime = {};
        g_lastRadioReadTime = {};
        Logger::LogWarning("WorldContextFNV: save load failed; cancelled post-load bridge gate");
        return;
    }

    g_context = {};
    g_saveLoadPending = true;
    g_saveLoadCompleted = true;
    g_bridgeWriteFloor = (std::max)(g_bridgeWriteFloor, modifiedTicks);
    g_lastBridgeReadTime = {};
    g_lastRadioReadTime = {};
    Logger::LogInfo("WorldContextFNV: save load completed; awaiting post-load bridge write");
}

void SendNow(bool force) {
    if (!Config::worldContextEnabled) {
        return;
    }

    RefreshFromBridge();
    ApplyNativeSnapshot(true);
    Context context;
    {
        std::lock_guard<std::mutex> lock(g_contextMutex);
        context = g_context;
    }
    if (!context.resolved || context.gamets <= 0) {
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
    QueueLocationChangedComment(context);
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
    if (!context.resolved || context.gamets <= 0) {
        return;
    }

    const std::string signature = BuildSignature(context);
    const bool changed = signature != g_lastSentSignature;
    if (due || (Config::worldContextSendOnChange && changed)) {
        g_lastSendTime = now;
        g_lastSentSignature = signature;
        SendContext(context);
        QueueLocationChangedComment(context);
    }
}

} // namespace WorldContextFNV
