// ActivityStatusFNV.cpp - Live actor activity snapshots for DialecticServer prompts

#include "ActivityStatusFNV.h"

#include "Config.h"
#include "HTTPManager.h"
#include "Logger.h"
#include "Misc.h"
#include "RuntimeGeneration.h"
#include "RuntimeSnapshot.h"
#include "TaskManager.h"
#include "WorldContextFNV.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace ActivityStatusFNV {
namespace {

struct ActivityStatus {
    std::string refId;
    std::string actorName;
    float distance = 0.0f;
    bool isDead = false;
    bool isUnconscious = false;
    bool isInCombat = false;
    bool isMoving = false;
    bool isRunning = false;
    bool isSneaking = false;
    bool isSitting = false;
    bool isSleeping = false;
    bool isWeaponDrawn = false;
};

static constexpr const char* kActivityStatusPath = "Data\\NVSE\\Plugins\\dialectic_activity_status.tmp";
static constexpr auto kBridgeReadInterval = std::chrono::milliseconds(500);
static constexpr auto kChangeCheckInterval = std::chrono::milliseconds(500);
static std::chrono::steady_clock::time_point g_lastBridgeReadTime;
static std::chrono::steady_clock::time_point g_lastSendTime;
static std::chrono::steady_clock::time_point g_lastCheckTime;
static std::vector<ActivityStatus> g_statuses;
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

bool IsUsableActorName(const std::string& name) {
    const std::string trimmed = Trim(name);
    return !trimmed.empty() &&
        trimmed != "<no name>" &&
        trimmed != "Player" &&
        trimmed != "Courier" &&
        trimmed != "Prisoner";
}

bool RefreshFromBridge() {
    const auto now = std::chrono::steady_clock::now();
    if (g_lastBridgeReadTime.time_since_epoch().count() != 0 &&
        now - g_lastBridgeReadTime < kBridgeReadInterval) {
        return false;
    }
    g_lastBridgeReadTime = now;

    uint64_t ageMs = 0;
    if (!GetFileModifiedAgeMs(kActivityStatusPath, ageMs) || ageMs > 5000) {
        return false;
    }

    std::ifstream input(kActivityStatusPath, std::ios::binary);
    if (!input.is_open()) {
        return false;
    }

    std::vector<ActivityStatus> statuses;
    std::string line;
    while (std::getline(input, line)) {
        line = Trim(line);
        if (line.rfind("activity=", 0) != 0) {
            continue;
        }

        const std::vector<std::string> parts = Split(line.substr(9), '^');
        if (parts.size() < 12) {
            continue;
        }

        ActivityStatus status;
        status.refId = Trim(parts[0]);
        status.actorName = Trim(parts[1]);
        status.distance = ParseFloat(parts[2]);
        status.isDead = ParseBoolFlag(parts[3]);
        status.isUnconscious = ParseBoolFlag(parts[4]);
        status.isInCombat = ParseBoolFlag(parts[5]);
        status.isMoving = ParseBoolFlag(parts[6]);
        status.isRunning = ParseBoolFlag(parts[7]);
        status.isSneaking = ParseBoolFlag(parts[8]);
        status.isSitting = ParseBoolFlag(parts[9]);
        status.isSleeping = ParseBoolFlag(parts[10]);
        status.isWeaponDrawn = ParseBoolFlag(parts[11]);

        if (!status.refId.empty() && IsUsableActorName(status.actorName)) {
            statuses.push_back(status);
        }
    }

    std::sort(statuses.begin(), statuses.end(), [](const ActivityStatus& a, const ActivityStatus& b) {
        return a.distance < b.distance;
    });

    const float maxDistance = Config::activityStatusMaxDistance > 0.0f
        ? Config::activityStatusMaxDistance
        : 2400.0f;
    statuses.erase(std::remove_if(statuses.begin(), statuses.end(), [maxDistance](const ActivityStatus& status) {
        return status.distance > maxDistance;
    }), statuses.end());

    if (Config::activityStatusMaxActors > 0 &&
        statuses.size() > static_cast<size_t>(Config::activityStatusMaxActors)) {
        statuses.resize(static_cast<size_t>(Config::activityStatusMaxActors));
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    g_statuses = statuses;
    return true;
}

std::string FormatFormId(std::uint32_t formId) {
    std::ostringstream stream;
    stream << "0x" << std::uppercase << std::hex << std::setw(8)
           << std::setfill('0') << formId;
    return stream.str();
}

bool RefreshFromNativeSnapshot() {
    RuntimeSnapshot::GameState gameState;
    if (!RuntimeSnapshot::TryGetFreshGameState(gameState, std::chrono::milliseconds(500))) {
        return false;
    }
    const std::vector<RuntimeSnapshot::ActorState> actors = RuntimeSnapshot::GetActors();

    std::vector<ActivityStatus> bridgeStatuses;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        bridgeStatuses = g_statuses;
    }

    const float maxDistance = Config::activityStatusMaxDistance > 0.0f
        ? Config::activityStatusMaxDistance : 2400.0f;
    std::vector<ActivityStatus> statuses;
    statuses.reserve(actors.size());
    for (const RuntimeSnapshot::ActorState& actor : actors) {
        if (!RuntimeSnapshot::IsActorInScene(actor, gameState) || actor.name.empty() ||
            actor.distanceToPlayer > maxDistance) {
            continue;
        }
        ActivityStatus status;
        status.refId = FormatFormId(actor.formId);
        status.actorName = actor.name;
        status.distance = actor.distanceToPlayer;
        status.isDead = actor.dead;
        status.isInCombat = actor.inCombat;
        status.isWeaponDrawn = actor.weaponDrawn;
        status.isMoving = actor.moving;
        status.isRunning = actor.running;
        status.isSneaking = actor.sneaking;

        const auto bridge = std::find_if(bridgeStatuses.begin(), bridgeStatuses.end(),
            [&status](const ActivityStatus& candidate) {
                return _stricmp(candidate.refId.c_str(), status.refId.c_str()) == 0;
            });
        if (bridge != bridgeStatuses.end()) {
            status.isUnconscious = bridge->isUnconscious;
            status.isSitting = bridge->isSitting;
            status.isSleeping = bridge->isSleeping;
        }
        statuses.push_back(std::move(status));
    }

    std::sort(statuses.begin(), statuses.end(), [](const ActivityStatus& a, const ActivityStatus& b) {
        return a.distance < b.distance;
    });
    if (Config::activityStatusMaxActors > 0 &&
        statuses.size() > static_cast<std::size_t>(Config::activityStatusMaxActors)) {
        statuses.resize(static_cast<std::size_t>(Config::activityStatusMaxActors));
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_statuses = std::move(statuses);
    return true;
}

std::string BuildSignature(const std::vector<ActivityStatus>& statuses) {
    std::ostringstream signature;
    for (const auto& status : statuses) {
        signature << status.refId << ":"
                  << status.isDead << status.isUnconscious << status.isInCombat
                  << status.isMoving << status.isRunning << status.isSneaking
                  << status.isSitting << status.isSleeping << status.isWeaponDrawn << "|";
    }
    return signature.str();
}

void AppendBool(std::ostringstream& json, const char* key, bool value) {
    json << "\"" << key << "\":" << (value ? "true" : "false");
}

std::string BuildJson(const std::vector<ActivityStatus>& statuses) {
    const long long localMs = Misc::GetCurrentTimeMillis();
    const long long localTs = localMs / 1000;
    const long long gameTs = WorldContextFNV::GetGameTimestamp();

    std::ostringstream json;
    json << "{";
    json << "\"schema\":\"dialectic.activity_status_bulk.v1\",";
    json << "\"type\":\"activity_status_bulk\",";
    json << "\"game\":\"fnv\",";
    json << "\"ts\":" << localTs << ",";
    json << "\"gamets\":" << (gameTs > 0 ? gameTs : 0) << ",";
    json << "\"statuses\":[";

    bool first = true;
    for (const auto& status : statuses) {
        if (!first) {
            json << ",";
        }
        first = false;

        json << "{";
        json << "\"actor_name\":\"" << HTTPManager::EscapeJson(status.actorName) << "\",";
        json << "\"actor_type\":\"npc\",";
        json << "\"refid\":\"" << HTTPManager::EscapeJson(status.refId) << "\",";
        json << "\"timestamp\":" << localMs << ",";
        json << "\"gamets\":" << (gameTs > 0 ? gameTs : 0) << ",";
        json << "\"distance\":" << status.distance << ",";
        AppendBool(json, "is_dead", status.isDead);
        json << ",";
        AppendBool(json, "is_unconscious", status.isUnconscious);
        json << ",";
        AppendBool(json, "is_in_combat", status.isInCombat);
        json << ",";
        AppendBool(json, "is_moving", status.isMoving);
        json << ",";
        AppendBool(json, "is_running", status.isRunning);
        json << ",";
        AppendBool(json, "is_sneaking", status.isSneaking);
        json << ",";
        AppendBool(json, "is_sitting", status.isSitting);
        json << ",";
        AppendBool(json, "is_sleeping", status.isSleeping);
        json << ",";
        AppendBool(json, "is_weapon_drawn", status.isWeaponDrawn);
        json << "}";
    }

    json << "]";
    json << "}";
    return json.str();
}

void SendStatuses(std::vector<ActivityStatus> statuses) {
    const std::string json = BuildJson(statuses);
    TaskManager::Enqueue("gamedata", "activity_status", RuntimeGeneration::Current(), false,
        std::chrono::seconds(30), [json, count = statuses.size()](const TaskManager::CancellationToken& token) {
        if (token.IsCancellationRequested()) return;
        const auto start = std::chrono::steady_clock::now();
        const std::string response = HTTPManager::SendJson("gamedata.php", json);
        const long long elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        Logger::LogInfo("[PERF] ActivityStatusFNV send elapsed_ms=%lld actors=%zu bytes=%zu response_empty=%d",
            elapsedMs,
            count,
            json.size(),
            response.empty() ? 1 : 0);
        if (response.empty()) {
            Logger::LogWarning("ActivityStatusFNV: activity_status update for %zu actors returned empty response", count);
        }
    });
}

} // namespace

void SendNow(bool force) {
    if (!Config::activityStatusEnabled) {
        return;
    }

    RefreshFromBridge();
    RefreshFromNativeSnapshot();
    std::vector<ActivityStatus> statuses;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        statuses = g_statuses;
    }

    const std::string signature = BuildSignature(statuses);
    std::lock_guard<std::mutex> lock(g_mutex);
    const bool changed = signature != g_lastSentSignature;
    if (!force && Config::activityStatusSendOnChange && !changed) {
        return;
    }

    g_lastSendTime = std::chrono::steady_clock::now();
    g_lastSentSignature = signature;
    SendStatuses(statuses);
}

void Update() {
    if (!Config::activityStatusEnabled) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const float configuredSeconds = Config::activityStatusUpdateSeconds > 0.5f
        ? Config::activityStatusUpdateSeconds
        : 2.0f;
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

} // namespace ActivityStatusFNV
