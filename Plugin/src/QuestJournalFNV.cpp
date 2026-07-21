// QuestJournalFNV.cpp - Sync active Fallout quest journal entries to DialecticServer

#include "QuestJournalFNV.h"

#include "Config.h"
#include "GameLoop.h"
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
#include <atomic>
#include <chrono>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace QuestJournalFNV {
namespace {

struct QuestObjective {
    int objectiveId = 0;
    std::string text;
};

struct QuestEntry {
    std::string formId;
    std::string name;
    std::string editorId;
    bool selected = false;
    std::vector<QuestObjective> objectives;
};

static constexpr const char* kQuestStatePath = "Data\\NVSE\\Plugins\\dialectic_quests.tmp";
static constexpr const char* kDirectActiveQuestPath = "Data\\NVSE\\Plugins\\dialectic_active_quest_direct.tmp";
static constexpr auto kBridgeReadInterval = std::chrono::milliseconds(750);
static constexpr auto kChangeCheckInterval = std::chrono::milliseconds(1000);
static constexpr auto kHeartbeatInterval = std::chrono::seconds(30);

static std::chrono::steady_clock::time_point g_lastBridgeReadTime;
static std::chrono::steady_clock::time_point g_lastSendTime;
static std::chrono::steady_clock::time_point g_lastCheckTime;
static std::vector<QuestEntry> g_quests;
static std::string g_lastSentSignature;
static std::mutex g_mutex;
static std::string g_lastBridgeStatus;
static std::chrono::steady_clock::time_point g_lastBridgeStatusLogTime;
static std::atomic<bool> g_hasScriptDirectQuestState{false};

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

void LogBridgeStatus(const std::string& status, bool force = false) {
    const auto now = std::chrono::steady_clock::now();
    const bool changed = status != g_lastBridgeStatus;
    const bool due = g_lastBridgeStatusLogTime.time_since_epoch().count() == 0 ||
        now - g_lastBridgeStatusLogTime >= kHeartbeatInterval;
    if (!force && !changed && !due) {
        return;
    }

    g_lastBridgeStatus = status;
    g_lastBridgeStatusLogTime = now;
    Logger::LogInfo("QuestJournalFNV: bridge status %s", status.c_str());
}

std::string FallbackQuestName(const QuestEntry& quest) {
    if (!quest.name.empty()) {
        return quest.name;
    }
    if (!quest.formId.empty()) {
        return quest.formId;
    }
    return "Quest";
}

bool RefreshFromNative() {
    const RuntimeSnapshot::QuestState native = RuntimeSnapshot::GetQuest();
    if (native.capturedAt.time_since_epoch().count() == 0) {
        return false;
    }

    std::vector<QuestEntry> quests;
    if (native.valid && native.formId != 0) {
        QuestEntry quest;
        std::ostringstream formId;
        formId << "0x" << std::uppercase << std::hex << std::setw(8)
            << std::setfill('0') << native.formId;
        quest.formId = formId.str();
        quest.name = native.name;
        quest.editorId = native.editorId;
        quest.selected = true;
        quest.objectives.reserve(native.objectives.size());
        for (const auto& source : native.objectives) {
            QuestObjective objective;
            objective.objectiveId = source.objectiveId;
            objective.text = source.text;
            quest.objectives.push_back(std::move(objective));
        }
        quests.push_back(std::move(quest));
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_quests = quests;
        g_hasScriptDirectQuestState = false;
    }
    std::ostringstream status;
    status << "native quests=" << quests.size();
    if (!quests.empty()) {
        status << " active=" << FallbackQuestName(quests.front())
            << " objectives=" << quests.front().objectives.size();
    }
    LogBridgeStatus(status.str());
    return true;
}

bool RefreshFromBridge() {
    const auto now = std::chrono::steady_clock::now();
    if (g_lastBridgeReadTime.time_since_epoch().count() != 0 &&
        now - g_lastBridgeReadTime < kBridgeReadInterval) {
        return false;
    }
    g_lastBridgeReadTime = now;

    uint64_t ageMs = 0;
    if (!GetFileModifiedAgeMs(kQuestStatePath, ageMs)) {
        if (!g_hasScriptDirectQuestState.load()) {
            LogBridgeStatus("missing Data\\NVSE\\Plugins\\dialectic_quests.tmp");
        }
        return false;
    }
    if (ageMs > 10000) {
        if (!g_hasScriptDirectQuestState.load()) {
            LogBridgeStatus("stale Data\\NVSE\\Plugins\\dialectic_quests.tmp");
        }
        return false;
    }

    std::ifstream input(kQuestStatePath, std::ios::binary);
    if (!input.is_open()) {
        LogBridgeStatus("open_failed");
        return false;
    }

    std::vector<QuestEntry> quests;
    std::unordered_map<std::string, size_t> questIndexById;

    std::ostringstream buffer;
    buffer << input.rdbuf();

    std::string line;
    std::istringstream lines(DecodeEscapedLineBreaks(buffer.str()));
    while (std::getline(lines, line)) {
        line = Trim(line);
        if (line.empty() || line.rfind("source=", 0) == 0) {
            continue;
        }

        if (line.rfind("quest=", 0) == 0) {
            const std::vector<std::string> parts = Split(line.substr(6), '^');
            if (parts.size() < 2) {
                continue;
            }

            QuestEntry quest;
            quest.formId = Trim(parts[0]);
            quest.name = Trim(parts[1]);
            if (parts.size() >= 3) {
                quest.editorId = Trim(parts[2]);
            }
            if (parts.size() >= 4) {
                quest.selected = ParseInt(parts[3]) != 0;
            }
            if (quest.formId.empty()) {
                continue;
            }

            auto existing = questIndexById.find(quest.formId);
            if (existing == questIndexById.end()) {
                questIndexById[quest.formId] = quests.size();
                quests.push_back(quest);
            } else {
                if (!quest.name.empty()) {
                    quests[existing->second].name = quest.name;
                }
                if (!quest.editorId.empty()) {
                    quests[existing->second].editorId = quest.editorId;
                }
                quests[existing->second].selected = quests[existing->second].selected || quest.selected;
            }
        } else if (line.rfind("objective=", 0) == 0) {
            const std::vector<std::string> parts = Split(line.substr(10), '^');
            if (parts.size() < 3) {
                continue;
            }

            const std::string questId = Trim(parts[0]);
            if (questId.empty()) {
                continue;
            }

            auto existing = questIndexById.find(questId);
            if (existing == questIndexById.end()) {
                QuestEntry quest;
                quest.formId = questId;
                questIndexById[questId] = quests.size();
                quests.push_back(quest);
                existing = questIndexById.find(questId);
            }

            QuestObjective objective;
            objective.objectiveId = ParseInt(parts[1]);
            objective.text = Trim(parts[2]);
            if (!objective.text.empty()) {
                quests[existing->second].objectives.push_back(objective);
            }
        }
    }

    quests.erase(std::remove_if(quests.begin(), quests.end(), [](const QuestEntry& quest) {
        return quest.formId.empty() && quest.name.empty();
    }), quests.end());

    if (quests.size() > 24) {
        quests.resize(24);
    }
    for (auto& quest : quests) {
        if (quest.objectives.size() > 12) {
            quest.objectives.resize(12);
        }
    }
    std::stable_sort(quests.begin(), quests.end(), [](const QuestEntry& left, const QuestEntry& right) {
        return left.selected && !right.selected;
    });

    std::lock_guard<std::mutex> lock(g_mutex);
    g_quests = quests;
    g_hasScriptDirectQuestState = false;
    std::ostringstream status;
    status << "fresh quests=" << quests.size();
    if (!quests.empty()) {
        status << " selected=" << (quests.front().selected ? "1" : "0")
            << " first=" << FallbackQuestName(quests.front());
    }
    LogBridgeStatus(status.str());
    return true;
}

bool RefreshFromDirectActiveQuestBridge() {
    uint64_t ageMs = 0;
    if (!GetFileModifiedAgeMs(kDirectActiveQuestPath, ageMs) || ageMs > 10000) {
        return false;
    }

    std::ifstream input(kDirectActiveQuestPath, std::ios::binary);
    if (!input.is_open()) {
        return false;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();

    std::vector<QuestEntry> quests;
    std::unordered_map<std::string, size_t> questIndexById;
    std::string line;
    std::istringstream lines(DecodeEscapedLineBreaks(buffer.str()));
    while (std::getline(lines, line)) {
        line = Trim(line);
        if (line.empty() || line.rfind("source=", 0) == 0) {
            continue;
        }

        if (line.rfind("active=", 0) == 0) {
            const std::vector<std::string> parts = Split(line.substr(7), '^');
            if (parts.empty()) {
                continue;
            }

            QuestEntry quest;
            quest.formId = Trim(parts[0]);
            if (parts.size() >= 2) {
                quest.name = Trim(parts[1]);
            }
            if (parts.size() >= 3) {
                quest.editorId = Trim(parts[2]);
            }
            quest.selected = true;

            if (!quest.formId.empty() && quest.formId != "0" && quest.formId != "00000000") {
                if (quest.name.empty() || quest.name == "none") {
                    quest.name = !quest.editorId.empty() && quest.editorId != "none" ? quest.editorId : quest.formId;
                }
                questIndexById[quest.formId] = quests.size();
                quests.push_back(quest);
            }
        } else if (line.rfind("objective=", 0) == 0) {
            const std::vector<std::string> parts = Split(line.substr(10), '^');
            if (parts.size() < 3) {
                continue;
            }

            const std::string questId = Trim(parts[0]);
            if (questId.empty()) {
                continue;
            }

            auto existing = questIndexById.find(questId);
            if (existing == questIndexById.end()) {
                QuestEntry quest;
                quest.formId = questId;
                quest.selected = true;
                questIndexById[questId] = quests.size();
                quests.push_back(quest);
                existing = questIndexById.find(questId);
            }

            QuestObjective objective;
            objective.objectiveId = ParseInt(parts[1]);
            objective.text = Trim(parts[2]);
            if (!objective.text.empty()) {
                quests[existing->second].objectives.push_back(objective);
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_quests = quests;
        g_hasScriptDirectQuestState = true;
    }

    std::ostringstream status;
    status << "direct_file quests=" << quests.size();
    if (!quests.empty()) {
        status << " active=" << FallbackQuestName(quests.front())
            << " form=" << quests.front().formId
            << " editor=" << quests.front().editorId;
    }
    LogBridgeStatus(status.str());
    return true;
}

std::string BuildSignature(const std::vector<QuestEntry>& quests) {
    std::ostringstream signature;
    for (const auto& quest : quests) {
        signature << quest.formId << ":" << quest.name << ":" << quest.editorId << ":" << (quest.selected ? "1" : "0") << "{";
        for (const auto& objective : quest.objectives) {
            signature << objective.objectiveId << "=" << objective.text << ";";
        }
        signature << "}|";
    }
    return signature.str();
}

std::string BuildBriefing(const QuestEntry& quest) {
    if (quest.objectives.empty()) {
        return "";
    }

    std::ostringstream text;
    for (size_t i = 0; i < quest.objectives.size(); ++i) {
        if (i > 0) {
            text << "; ";
        }
        text << quest.objectives[i].text;
    }
    return text.str();
}

std::string BuildJson(const std::vector<QuestEntry>& quests) {
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
    json << "\"schema\":\"dialectic.active_quests.v1\",";
    json << "\"type\":\"active_quests\",";
    json << "\"game\":\"fnv\",";
    json << "\"source\":\"quest_state\",";
    json << "\"ts\":" << localTs << ",";
    json << "\"gamets\":" << (gameTs > 0 ? gameTs : 0) << ",";
    json << "\"player\":\"" << HTTPManager::EscapeJson(playerName) << "\",";
    json << "\"quests\":[";

    bool firstQuest = true;
    for (const auto& quest : quests) {
        if (!firstQuest) {
            json << ",";
        }
        firstQuest = false;

        json << "{";
        json << "\"formid\":\"" << HTTPManager::EscapeJson(quest.formId) << "\",";
        json << "\"id_quest\":\"" << HTTPManager::EscapeJson(quest.formId) << "\",";
        json << "\"name\":\"" << HTTPManager::EscapeJson(FallbackQuestName(quest)) << "\",";
        json << "\"editor_id\":\"" << HTTPManager::EscapeJson(quest.editorId) << "\",";
        json << "\"briefing\":\"" << HTTPManager::EscapeJson(BuildBriefing(quest)) << "\",";
        json << "\"selected\":" << (quest.selected ? "true" : "false") << ",";
        json << "\"active_selected\":" << (quest.selected ? "true" : "false") << ",";
        json << "\"status\":\"active\",";
        json << "\"objectives\":[";

        bool firstObjective = true;
        for (const auto& objective : quest.objectives) {
            if (!firstObjective) {
                json << ",";
            }
            firstObjective = false;
            json << "{";
            json << "\"id\":" << objective.objectiveId << ",";
            json << "\"text\":\"" << HTTPManager::EscapeJson(objective.text) << "\"";
            json << "}";
        }

        json << "]";
        json << "}";
    }

    json << "]";
    json << "}";
    return json.str();
}

void SendQuests(std::vector<QuestEntry> quests) {
    const std::string json = BuildJson(quests);
    TaskManager::Enqueue("gamedata", "active_quests", RuntimeGeneration::Current(), false,
        std::chrono::seconds(30), [json, count = quests.size()](const TaskManager::CancellationToken& token) {
        if (token.IsCancellationRequested()) return;
        const auto start = std::chrono::steady_clock::now();
        const std::string response = HTTPManager::SendJson("gamedata.php", json);
        const long long elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        Logger::LogInfo("[PERF] QuestJournalFNV send elapsed_ms=%lld quests=%zu bytes=%zu response_empty=%d",
            elapsedMs,
            count,
            json.size(),
            response.empty() ? 1 : 0);
        if (response.empty()) {
            Logger::LogWarning("QuestJournalFNV: active_quests update for %zu quests returned empty response", count);
        }
    });
}

} // namespace

void SendNow(bool force) {
    if (!RefreshFromNative() && !RefreshFromDirectActiveQuestBridge()) {
        RefreshFromBridge();
    }

    std::vector<QuestEntry> quests;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        quests = g_quests;
    }

    const std::string signature = BuildSignature(quests);
    bool changed = false;
    bool hadPreviousSnapshot = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        changed = signature != g_lastSentSignature;
        hadPreviousSnapshot = !g_lastSentSignature.empty();
        if (!force && !changed) {
            return;
        }

        g_lastSendTime = std::chrono::steady_clock::now();
        g_lastSentSignature = signature;
    }

    SendQuests(quests);
    if (changed && hadPreviousSnapshot) {
        const auto selected = std::find_if(quests.begin(), quests.end(),
            [](const QuestEntry& quest) { return quest.selected; });
        if (selected != quests.end() && !selected->name.empty()) {
            const std::string briefing = BuildBriefing(*selected);
            std::string text = "The active quest changed to " + selected->name;
            if (!briefing.empty() && briefing != selected->name) {
                text += ": " + briefing;
            }
            GameLoop::QueueRpgCommentEvent("quest_updated", text);
        }
    }
}

void UpdateActiveQuestFromScript(const char* formId, const char* name, const char* editorId) {
    QuestEntry quest;
    quest.formId = Trim(formId ? formId : "");
    quest.name = Trim(name ? name : "");
    quest.editorId = Trim(editorId ? editorId : "");
    quest.selected = true;

    std::vector<QuestEntry> quests;
    if (!quest.formId.empty() && quest.formId != "0" && quest.formId != "00000000") {
        if (quest.name.empty()) {
            quest.name = !quest.editorId.empty() ? quest.editorId : quest.formId;
        }
        quests.push_back(quest);
    }

    const std::string signature = BuildSignature(quests);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_quests = quests;
        g_hasScriptDirectQuestState = true;
        g_lastSentSignature = signature;
        g_lastSendTime = std::chrono::steady_clock::now();
    }

    std::ostringstream status;
    status << "script_direct quests=" << quests.size();
    if (!quests.empty()) {
        status << " active=" << FallbackQuestName(quests.front())
            << " form=" << quests.front().formId
            << " editor=" << quests.front().editorId;
    }
    LogBridgeStatus(status.str(), true);
    SendQuests(quests);
}

void Update() {
    const auto now = std::chrono::steady_clock::now();

    bool due = false;
    bool canCheckChange = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        due = g_lastSendTime.time_since_epoch().count() == 0 ||
            now - g_lastSendTime >= kHeartbeatInterval;
        canCheckChange = g_lastCheckTime.time_since_epoch().count() == 0 ||
            now - g_lastCheckTime >= kChangeCheckInterval;
        if (canCheckChange) {
            g_lastCheckTime = now;
        }
    }

    if (due || canCheckChange) {
        SendNow(false);
    }
}

} // namespace QuestJournalFNV
