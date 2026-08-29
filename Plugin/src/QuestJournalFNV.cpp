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

#include <algorithm>
#include <chrono>
#include <cctype>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
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

static constexpr auto kChangeCheckInterval = std::chrono::milliseconds(1000);
static constexpr auto kHeartbeatInterval = std::chrono::seconds(30);

static std::chrono::steady_clock::time_point g_lastSendTime;
static std::chrono::steady_clock::time_point g_lastCheckTime;
static std::vector<QuestEntry> g_quests;
static std::string g_lastSentSignature;
static std::mutex g_mutex;
static std::string g_lastStatus;
static std::chrono::steady_clock::time_point g_lastStatusLogTime;

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

void LogQuestStatus(const std::string& status, bool force = false) {
    const auto now = std::chrono::steady_clock::now();
    const bool changed = status != g_lastStatus;
    const bool due = g_lastStatusLogTime.time_since_epoch().count() == 0 ||
        now - g_lastStatusLogTime >= kHeartbeatInterval;
    if (!force && !changed && !due) {
        return;
    }

    g_lastStatus = status;
    g_lastStatusLogTime = now;
    Logger::LogInfo("QuestJournalFNV: native status %s", status.c_str());
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
        std::lock_guard<std::mutex> lock(g_mutex);
        g_quests.clear();
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
    }
    std::ostringstream status;
    status << "native quests=" << quests.size();
    if (!quests.empty()) {
        status << " active=" << FallbackQuestName(quests.front())
            << " objectives=" << quests.front().objectives.size();
    }
    LogQuestStatus(status.str());
    return true;
}

bool ContainsIgnoreCase(const std::string& value, const std::string& needle) {
    if (needle.empty()) {
        return true;
    }
    std::string normalizedValue = value;
    std::string normalizedNeedle = needle;
    std::transform(normalizedValue.begin(), normalizedValue.end(), normalizedValue.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    std::transform(normalizedNeedle.begin(), normalizedNeedle.end(), normalizedNeedle.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return normalizedValue.find(normalizedNeedle) != std::string::npos;
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
    RefreshFromNative();

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
    RuntimeSnapshot::QuestState quest;
    std::string normalizedFormId = Trim(formId ? formId : "");
    if (normalizedFormId.rfind("0x", 0) == 0 || normalizedFormId.rfind("0X", 0) == 0) {
        normalizedFormId.erase(0, 2);
    }
    try {
        quest.formId = static_cast<uint32_t>(std::stoul(normalizedFormId, nullptr, 16));
    } catch (...) {
        quest.formId = 0;
    }
    quest.name = Trim(name ? name : "");
    quest.editorId = Trim(editorId ? editorId : "");
    quest.valid = quest.formId != 0;
    quest.generation = RuntimeGeneration::Current();
    quest.capturedAt = std::chrono::steady_clock::now();
    RuntimeSnapshot::UpdateQuest(std::move(quest));
    LogQuestStatus("script command updated native snapshot", true);
    SendNow(true);
}

std::string BuildCurrentQuestResult(const std::string& filterValue) {
    RefreshFromNative();

    std::vector<QuestEntry> quests;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        quests = g_quests;
    }

    const std::string filter = Trim(filterValue);
    std::vector<std::string> lines;
    for (const auto& quest : quests) {
        const std::string questName = FallbackQuestName(quest);
        const bool questMatches = filter.empty() ||
            ContainsIgnoreCase(quest.formId, filter) ||
            ContainsIgnoreCase(questName, filter) ||
            ContainsIgnoreCase(quest.editorId, filter);
        if (questMatches) {
            lines.push_back(questName + " (" + quest.formId + ")");
        }

        for (const auto& objective : quest.objectives) {
            if (filter.empty() || questMatches || ContainsIgnoreCase(objective.text, filter)) {
                lines.push_back(questName + ": " + objective.text);
            }
        }
    }

    if (lines.empty()) {
        return filter.empty()
            ? "No active quest entries were found in the native quest snapshot."
            : "No quest entries matched " + filter + ".";
    }

    std::ostringstream result;
    result << "Quest journal: ";
    for (size_t i = 0; i < lines.size() && i < 16; ++i) {
        if (i > 0) {
            result << "; ";
        }
        result << lines[i];
    }
    return result.str();
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
