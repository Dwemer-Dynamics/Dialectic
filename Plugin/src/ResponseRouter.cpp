#include "ResponseRouter.h"

#include "ActionManager.h"
#include "ActorPositionResolverFNV.h"
#include "AgentManager.h"
#include "Config.h"
#include "Console.h"
#include "GameLoop.h"
#include "HTTPManager.h"
#include "Logger.h"
#include "Misc.h"
#include "ResponseQueueFNV.h"
#include "TargetManager.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace ResponseRouter {
namespace {

std::string Trim(std::string value) {
    const char* whitespace = " \t\r\n";
    const size_t start = value.find_first_not_of(whitespace);
    if (start == std::string::npos) {
        return "";
    }
    const size_t end = value.find_last_not_of(whitespace);
    return value.substr(start, end - start + 1);
}

uint32_t ParseFormIdString(const std::string& rawValue) {
    std::string value = Trim(rawValue);
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

bool EqualsIgnoreCase(const std::string& left, const std::string& right) {
    if (left.size() != right.size()) {
        return false;
    }

    for (size_t i = 0; i < left.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(left[i])) !=
            std::tolower(static_cast<unsigned char>(right[i]))) {
            return false;
        }
    }
    return true;
}

std::string ExtractJsonStringValue(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    size_t keyPos = json.find(needle);
    if (keyPos == std::string::npos) {
        return "";
    }

    size_t colonPos = json.find(':', keyPos + needle.size());
    if (colonPos == std::string::npos) {
        return "";
    }

    size_t quotePos = json.find('"', colonPos + 1);
    if (quotePos == std::string::npos) {
        return "";
    }

    std::string value;
    bool escaped = false;
    for (size_t i = quotePos + 1; i < json.size(); ++i) {
        const char c = json[i];
        if (escaped) {
            switch (c) {
                case '"': value.push_back('"'); break;
                case '\\': value.push_back('\\'); break;
                case 'n': value.push_back('\n'); break;
                case 'r': value.push_back('\r'); break;
                case 't': value.push_back('\t'); break;
                default: value.push_back(c); break;
            }
            escaped = false;
            continue;
        }

        if (c == '\\') {
            escaped = true;
            continue;
        }

        if (c == '"') {
            break;
        }

        value.push_back(c);
    }

    return value;
}

bool ExtractJsonBoolValue(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    size_t keyPos = json.find(needle);
    if (keyPos == std::string::npos) {
        return false;
    }

    size_t colonPos = json.find(':', keyPos + needle.size());
    if (colonPos == std::string::npos) {
        return false;
    }

    size_t valueStart = json.find_first_not_of(" \t\r\n", colonPos + 1);
    return valueStart != std::string::npos && json.compare(valueStart, 4, "true") == 0;
}

int ExtractJsonIntValue(const std::string& json, const std::string& key, int fallback = 0) {
    const std::string needle = "\"" + key + "\"";
    size_t keyPos = json.find(needle);
    if (keyPos == std::string::npos) {
        return fallback;
    }

    size_t colonPos = json.find(':', keyPos + needle.size());
    if (colonPos == std::string::npos) {
        return fallback;
    }

    size_t valueStart = json.find_first_of("-0123456789", colonPos + 1);
    if (valueStart == std::string::npos) {
        return fallback;
    }

    size_t valueEnd = json.find_first_not_of("-0123456789", valueStart);
    try {
        return std::stoi(json.substr(valueStart, valueEnd - valueStart));
    } catch (...) {
        return fallback;
    }
}

std::vector<std::string> ExtractJsonArrayObjects(const std::string& json, const std::string& key) {
    std::vector<std::string> objects;
    const std::string needle = "\"" + key + "\"";
    size_t keyPos = json.find(needle);
    if (keyPos == std::string::npos) {
        return objects;
    }

    size_t arrayStart = json.find('[', keyPos + needle.size());
    if (arrayStart == std::string::npos) {
        return objects;
    }

    bool inString = false;
    bool escaped = false;
    int depth = 0;
    size_t objectStart = std::string::npos;
    for (size_t i = arrayStart + 1; i < json.size(); ++i) {
        const char c = json[i];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (c == '\\' && inString) {
            escaped = true;
            continue;
        }
        if (c == '"') {
            inString = !inString;
            continue;
        }
        if (inString) {
            continue;
        }
        if (c == '{') {
            if (depth == 0) {
                objectStart = i;
            }
            ++depth;
            continue;
        }
        if (c == '}') {
            --depth;
            if (depth == 0 && objectStart != std::string::npos) {
                objects.push_back(json.substr(objectStart, i - objectStart + 1));
                objectStart = std::string::npos;
            }
            continue;
        }
        if (c == ']' && depth == 0) {
            break;
        }
    }
    return objects;
}

bool LooksLikeMetadataToken(const std::string& value) {
    const std::string token = Trim(value);
    if (token.size() > 80) {
        return false;
    }

    for (char ch : token) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (!std::isalnum(c) && ch != ' ' && ch != '_' && ch != '-' && ch != '.' && ch != '#') {
            return false;
        }
    }

    return true;
}

std::string StripDialogueMetadata(std::string message) {
    message = Trim(message);

    // Script variable names must never become dialogue if a capture bridge is malformed.
    const char* contaminatedTokens[] = { "ssubtitle", "sspeakername" };
    for (const char* token : contaminatedTokens) {
        std::string lower = message;
        std::transform(lower.begin(), lower.end(), lower.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        const std::size_t tokenLength = std::strlen(token);
        std::size_t position = 0;
        while ((position = lower.find(token, position)) != std::string::npos) {
            const bool leftBoundary = position == 0 ||
                !std::isalnum(static_cast<unsigned char>(lower[position - 1]));
            const std::size_t end = position + tokenLength;
            const bool rightBoundary = end >= lower.size() ||
                !std::isalnum(static_cast<unsigned char>(lower[end]));
            if (!leftBoundary || !rightBoundary) {
                position = end;
                continue;
            }
            message.erase(position, tokenLength);
            lower.erase(position, tokenLength);
        }
    }

    std::vector<std::string> parts;
    std::string part;
    std::istringstream stream(message);
    while (std::getline(stream, part, '/')) {
        parts.push_back(Trim(part));
    }

    if (parts.size() >= 4 && !parts[0].empty()) {
        bool metadataTail = true;
        for (size_t i = 1; i < parts.size(); ++i) {
            if (parts[i].empty()) {
                continue;
            }
            if (!LooksLikeMetadataToken(parts[i])) {
                metadataTail = false;
                break;
            }
        }
        if (metadataTail) {
            message = parts[0];
        }
    }

    const char* labels[] = { "mood:", "emotion:", "action:", "animation:", "speaker:", "listener:", "target:" };
    bool changed = true;
    while (changed) {
        changed = false;
        const size_t open = message.find('[');
        const size_t close = open == std::string::npos ? std::string::npos : message.find(']', open + 1);
        if (open == std::string::npos || close == std::string::npos) {
            break;
        }

        std::string tag = message.substr(open + 1, close - open - 1);
        std::transform(tag.begin(), tag.end(), tag.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

        for (const char* label : labels) {
            if (tag.rfind(label, 0) == 0) {
                message.erase(open, close - open + 1);
                changed = true;
                break;
            }
        }
    }

    std::string normalized;
    normalized.reserve(message.size());
    bool previousSpace = false;
    for (char ch : message) {
        const bool space = std::isspace(static_cast<unsigned char>(ch)) != 0;
        if (space && previousSpace) continue;
        normalized.push_back(space ? ' ' : ch);
        previousSpace = space;
    }
    return Trim(normalized);
}

bool IsPlayerSpeakerName(const std::string& speaker) {
    return EqualsIgnoreCase(speaker, "Player") ||
        (!Config::playerName.empty() && EqualsIgnoreCase(speaker, Config::playerName));
}

std::string PlayerDisplayName() {
    std::string name = Trim(Config::playerName);
    if (name.empty() || EqualsIgnoreCase(name, "Player")) {
        name = Trim(Misc::GetPlayerName());
    }
    return name.empty() ? "Player" : name;
}

std::string NormalizePlayerSpeakerForDisplay(const std::string& speaker) {
    if (!IsPlayerSpeakerName(speaker)) {
        return speaker;
    }

    return PlayerDisplayName();
}

bool IsRecentDuplicatePlayerTtsLine(
    const std::string& speaker,
    const std::string& listenerHint,
    const std::string& message,
    const std::string& ttsCacheKey) {
    if (!IsPlayerSpeakerName(speaker)) {
        return false;
    }

    if (!EqualsIgnoreCase(listenerHint, "__player_menu_tts") &&
        !EqualsIgnoreCase(listenerHint, "__player_tts") &&
        !EqualsIgnoreCase(listenerHint, "__player_text_only")) {
        return false;
    }

    const std::string key = !ttsCacheKey.empty() ? ttsCacheKey : message;
    if (key.empty()) {
        return false;
    }

    static std::string lastPlayerTtsKey;
    static std::chrono::steady_clock::time_point lastPlayerTtsTime;
    const auto now = std::chrono::steady_clock::now();
    if (key == lastPlayerTtsKey &&
        now - lastPlayerTtsTime < std::chrono::seconds(3)) {
        return true;
    }

    lastPlayerTtsKey = key;
    lastPlayerTtsTime = now;
    return false;
}

bool IsRecentDuplicateDialogueLine(
    const std::string& requestId,
    const std::string& utteranceId,
    const std::string& speaker,
    const std::string& message) {
    struct RecentDialogueLine {
        std::string requestId;
        std::string utteranceId;
        std::string speaker;
        std::string message;
        std::chrono::steady_clock::time_point timestamp;
    };

    static std::mutex recentMutex;
    static std::deque<RecentDialogueLine> recentLines;

    const std::string cleanRequestId = Trim(requestId);
    const std::string cleanUtteranceId = Trim(utteranceId);
    const std::string cleanSpeaker = Trim(speaker);
    const std::string cleanMessage = Trim(message);
    if (cleanUtteranceId.empty() && cleanMessage.empty()) {
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(recentMutex);
    while (!recentLines.empty() &&
           now - recentLines.front().timestamp > std::chrono::seconds(30)) {
        recentLines.pop_front();
    }

    for (const RecentDialogueLine& line : recentLines) {
        if (!cleanUtteranceId.empty() &&
            !line.utteranceId.empty() &&
            cleanUtteranceId == line.utteranceId &&
            (cleanRequestId.empty() || line.requestId.empty() || cleanRequestId == line.requestId)) {
            return true;
        }

        if (cleanUtteranceId.empty() &&
            !cleanRequestId.empty() &&
            cleanRequestId == line.requestId &&
            EqualsIgnoreCase(cleanSpeaker, line.speaker) &&
            cleanMessage == line.message) {
            return true;
        }
    }

    recentLines.push_back({cleanRequestId, cleanUtteranceId, cleanSpeaker, cleanMessage, now});
    while (recentLines.size() > 256) {
        recentLines.pop_front();
    }
    return false;
}

uint32_t ResolveResponseSpeakerFormId(const std::string& speaker) {
    if (!speaker.empty() && IsPlayerSpeakerName(speaker)) {
        const uint32_t playerFormId = Misc::GetPlayerFormId();
        if (playerFormId != 0) {
            Logger::LogInfo("ResponseRouter: Resolved response speaker [%s] as player form 0x%08X",
                speaker.c_str(),
                playerFormId);
            return playerFormId;
        }
        Logger::LogInfo("ResponseRouter: Could not resolve player form id for speaker [%s]", speaker.c_str());
        return 0;
    }

    if (!speaker.empty() && EqualsIgnoreCase(speaker, "The Narrator")) {
        return 0;
    }

    if (GameLoop::IsConversationActive()) {
        const uint32_t conversationFormId = GameLoop::GetConversationPartnerFormId();
        const std::string registeredName = AgentManager::GetAgentName(conversationFormId);
        const auto conversationPosition = ActorPositionResolverFNV::ResolveActor(conversationFormId);
        if (conversationFormId != 0 &&
            (EqualsIgnoreCase(registeredName, speaker) ||
             (conversationPosition.resolved &&
              EqualsIgnoreCase(conversationPosition.actorName, speaker)))) {
            Logger::LogInfo("ResponseRouter: Resolved response speaker [%s] from active conversation as 0x%08X",
                speaker.c_str(),
                conversationFormId);
            return conversationFormId;
        }
    }

    const auto& target = TargetManager::GetCurrentTarget();
    if (target.formId != 0 && EqualsIgnoreCase(target.name, speaker)) {
        Logger::LogInfo("ResponseRouter: Resolved response speaker [%s] from current target as 0x%08X",
            speaker.c_str(),
            target.formId);
        return target.formId;
    }

    const std::vector<ActorPositionResolverFNV::PositionResult> positions =
        ActorPositionResolverFNV::GetRecentActorPositions();
    for (const auto& position : positions) {
        if (position.resolved &&
            position.formId != 0 &&
            EqualsIgnoreCase(position.actorName, speaker) &&
            ActorPositionResolverFNV::IsPositionInPlayerScene(position) &&
            ActorPositionResolverFNV::IsActorPositionFresh(position.formId, 8000) &&
            ActorPositionResolverFNV::IsActorInLatestScan(position.formId, 8000) &&
            !(position.disabledKnown && position.isDisabled) &&
            !(position.deadKnown && position.isDead)) {
            Logger::LogInfo("ResponseRouter: Resolved response speaker [%s] from spatial cache as 0x%08X",
                speaker.c_str(),
                position.formId);
            return position.formId;
        }
    }

    const uint32_t agentFormId = AgentManager::FindAgentFormIdByName(speaker);
    if (agentFormId != 0) {
        Logger::LogInfo("ResponseRouter: Resolved response speaker [%s] from agent registry fallback as 0x%08X",
            speaker.c_str(),
            agentFormId);
        return agentFormId;
    }

    return 0;
}

// A scene is accepted once across streaming and the final action-only pass.
// Validate the entire cast before queueing speech; ordinary response routing stays unchanged.
bool QueueDirectorScene(const std::string& lineObject, const char* source, uint64_t generation) {
    const std::string payload = ExtractJsonStringValue(lineObject, "payload");
    const std::string id = ExtractJsonStringValue(payload, "id");
    const auto lines = ExtractJsonArrayObjects(payload, "lines");
    const auto actions = ExtractJsonArrayObjects(payload, "actions");
    if (ExtractJsonStringValue(payload, "schema") != "dialectic.director_scene.v1"
        || id.empty() || id.size() > 64 || lines.empty() || lines.size() > 6 || actions.size() > 3
        || !ResponseQueueFNV::IsCurrentGeneration(generation)) {
        Logger::LogWarning("DirectorScene: rejected invalid or stale scene");
        return false;
    }
    std::vector<ResponseQueueFNV::DialogueLine> dialogue;
    std::vector<std::string> closingActions;
    for (const auto& line : lines) {
        ResponseQueueFNV::DialogueLine queued;
        queued.speaker = Trim(ExtractJsonStringValue(line, "speaker"));
        queued.text = Trim(ExtractJsonStringValue(line, "text"));
        queued.listenerHint = Trim(ExtractJsonStringValue(line, "listener"));
        queued.actorFormId = ResolveResponseSpeakerFormId(queued.speaker);
        queued.listenerFormId = ResolveResponseSpeakerFormId(queued.listenerHint);
        queued.ttsCacheKey = ExtractJsonStringValue(line, "tts_cache_key");
        queued.utteranceId = ExtractJsonStringValue(line, "utterance_id");
        queued.requestId = id;
        queued.responseGeneration = generation;
        queued.directorScene = true;
        const auto actor = ActorPositionResolverFNV::ResolveActor(queued.actorFormId);
        if (queued.actorFormId == 0 || IsPlayerSpeakerName(queued.speaker) || queued.text.empty()
            || queued.text.size() > 2400 || queued.ttsCacheKey.empty() || queued.listenerFormId == 0
            || !actor.resolved || !ActorPositionResolverFNV::IsPositionInPlayerScene(actor)) {
            Logger::LogWarning("DirectorScene: unavailable speaker/listener or invalid line in scene %s", id.c_str());
            return false;
        }
        dialogue.push_back(std::move(queued));
    }
    for (const auto& action : actions) {
        const std::string speaker = ExtractJsonStringValue(action, "speaker");
        const std::string command = ExtractJsonStringValue(action, "command_name");
        const std::string target = ExtractJsonStringValue(action, "target");
        if ((command != "Attack" && command != "Follow" && command != "MoveTo"
            && command != "ComeCloser" && command != "StopFollowing" && command != "TakeASeat")
            || ResolveResponseSpeakerFormId(speaker) == 0 || IsPlayerSpeakerName(speaker)) {
            Logger::LogWarning("DirectorScene: rejected invalid closing action in scene %s", id.c_str());
            return false;
        }
        closingActions.push_back("{\"action\":\"rolecommand\",\"action_source\":\"director_scene\",\"speaker\":\""
            + HTTPManager::EscapeJson(speaker) + "\",\"command_name\":\"" + command
            + "\",\"target\":\"" + HTTPManager::EscapeJson(target) + "\"}");
    }
    static std::mutex sceneMutex;
    static std::deque<std::string> acceptedScenes;
    std::lock_guard<std::mutex> lock(sceneMutex);
    if (std::find(acceptedScenes.begin(), acceptedScenes.end(), id) != acceptedScenes.end()) {
        return true;
    }
    acceptedScenes.push_back(id);
    if (acceptedScenes.size() > 64) acceptedScenes.pop_front();
    for (const auto& line : dialogue) {
        ResponseQueueFNV::EnqueueDialogue(line, source);
    }
    for (const auto& action : closingActions) {
        ResponseQueueFNV::EnqueueAction(action, source, generation, true);
    }
    Logger::LogInfo("DirectorScene: queued %s dialogue=%zu closing_actions=%zu", id.c_str(), dialogue.size(), closingActions.size());
    return true;
}

} // namespace

bool ProcessJsonResponse(const std::string& response, const char* source, uint64_t responseGeneration) {
    if (response.empty()) {
        return false;
    }

    Logger::LogInfo("%s: Received response (%zu bytes)",
        source ? source : "ResponseRouter",
        response.length());
    Logger::LogInfo("%s: Response preview: %s",
        source ? source : "ResponseRouter",
        response.length() > 200 ? response.substr(0, 200).c_str() : response.c_str());

    struct QueuedDialogueLine {
        std::string speaker;
        std::string displayName;
        std::string action;
        std::string message;
        std::string ttsCacheKey;
        std::string utteranceId;
        std::string requestId;
        std::string listenerHint;
        std::string rechatTargetHint;
        uint32_t listenerFormId = 0;
        uint32_t rechatTargetFormId = 0;
        int rechatDepth = 0;
        uint32_t actorFormId = 0;
    };

    std::vector<QueuedDialogueLine> dialogueLines;
    std::vector<std::string> actionLines;

    const bool closeRequested = ExtractJsonBoolValue(response, "close");
    const int responseRechatDepth = ExtractJsonIntValue(response, "rechat_depth", 0);
    const std::string responseRequestId = Trim(ExtractJsonStringValue(response, "request_id"));
    if (closeRequested) {
        Logger::LogInfo("%s: Server requested conversation close", source ? source : "ResponseRouter");
    }

    std::vector<std::string> responseLines = ExtractJsonArrayObjects(response, "lines");
    for (size_t lineIndex = 0; lineIndex < responseLines.size(); ++lineIndex) {
        const std::string& lineObject = responseLines[lineIndex];
        if (ExtractJsonStringValue(lineObject, "command_name") == "DirectorScene") {
            QueueDirectorScene(lineObject, source, responseGeneration);
            continue;
        }
        std::string speaker = Trim(ExtractJsonStringValue(lineObject, "speaker"));
        std::string displayName = Trim(ExtractJsonStringValue(lineObject, "display_name"));
        std::string action = Trim(ExtractJsonStringValue(lineObject, "action"));
        std::string message = Trim(ExtractJsonStringValue(lineObject, "text"));
        if (message.empty()) {
            message = Trim(ExtractJsonStringValue(lineObject, "subtitle"));
        }
        std::string ttsCacheKey = Trim(ExtractJsonStringValue(lineObject, "tts_cache_key"));
        std::string utteranceId = Trim(ExtractJsonStringValue(lineObject, "utterance_id"));
        std::string requestId = Trim(ExtractJsonStringValue(lineObject, "request_id"));
        if (requestId.empty()) {
            requestId = responseRequestId;
        }
        std::string listenerHint = Trim(ExtractJsonStringValue(lineObject, "listener"));
        std::string rechatTargetHint = Trim(ExtractJsonStringValue(lineObject, "rechat_target"));
        const uint32_t speakerFormId = ParseFormIdString(ExtractJsonStringValue(lineObject, "speaker_formid"));
        const uint32_t listenerFormId = ParseFormIdString(ExtractJsonStringValue(lineObject, "listener_formid"));
        const uint32_t rechatTargetFormId = ParseFormIdString(ExtractJsonStringValue(lineObject, "rechat_target_formid"));
        message = StripDialogueMetadata(message);

        Logger::LogInfo("%s: JSON line %zu speaker=[%s] action=[%s] message=[%s]",
            source ? source : "ResponseRouter",
            lineIndex + 1,
            speaker.c_str(),
            action.c_str(),
            message.length() > 50 ? message.substr(0, 50).c_str() : message.c_str());

        const bool isActionCommand = action == "rolecommand" || ActionManager::IsActionCommand(action);
        if (speaker.empty() || (message.empty() && !isActionCommand)) {
            Logger::LogInfo("%s: Skipping JSON line with empty speaker/message", source ? source : "ResponseRouter");
            continue;
        }

        if (IsRecentDuplicatePlayerTtsLine(speaker, listenerHint, message, ttsCacheKey)) {
            Logger::LogInfo("%s: Skipping duplicate recent Player TTS line listener=[%s] key=[%s]",
                source ? source : "ResponseRouter",
                listenerHint.c_str(),
                !ttsCacheKey.empty() ? ttsCacheKey.c_str() : message.c_str());
            continue;
        }

        const bool isPlayerTextOnly = EqualsIgnoreCase(listenerHint, "__player_text_only");
        if (IsPlayerSpeakerName(speaker) && ttsCacheKey.empty() && !isPlayerTextOnly) {
            Logger::LogInfo("%s: Player TTS JSON line has no tts_cache_key; audio download will use legacy text hash.",
                source ? source : "ResponseRouter");
        }

        if (action.empty() || action == "say") {
            const std::string queuedSpeaker = NormalizePlayerSpeakerForDisplay(speaker);
            if (displayName.empty()) {
                displayName = queuedSpeaker;
            }
            if (IsRecentDuplicateDialogueLine(requestId, utteranceId, queuedSpeaker, message)) {
                Logger::LogInfo("%s: Skipping duplicate dialogue line speaker=[%s] utterance=[%s] request=[%s]",
                    source ? source : "ResponseRouter",
                    queuedSpeaker.c_str(),
                    utteranceId.c_str(),
                    requestId.c_str());
                continue;
            }
            dialogueLines.push_back({
                queuedSpeaker,
                displayName,
                action.empty() ? "say" : action,
                message,
                ttsCacheKey,
                utteranceId,
                requestId,
                listenerHint,
                rechatTargetHint,
                listenerFormId,
                rechatTargetFormId,
                responseRechatDepth,
                speakerFormId != 0 ? speakerFormId : ResolveResponseSpeakerFormId(speaker)
            });

            if (!isPlayerTextOnly) {
                const std::string subtitle = "[" + displayName + "] " + message;
                Console::Print(subtitle.c_str());
            }
        } else if (isActionCommand) {
            actionLines.push_back(lineObject);
        } else {
            Logger::LogInfo("%s: Ignoring non-dialogue JSON action: [%s]",
                source ? source : "ResponseRouter",
                action.c_str());
        }
    }

    for (size_t i = 0; i < dialogueLines.size(); ++i) {
        const bool isFinalResponseLine = (i + 1 == dialogueLines.size());
        const std::string listenerHint = dialogueLines[i].listenerHint.empty()
            ? (Config::playerName.empty() ? "Player" : Config::playerName)
            : dialogueLines[i].listenerHint;
        ResponseQueueFNV::DialogueLine queuedLine;
        queuedLine.text = dialogueLines[i].message;
        queuedLine.speaker = dialogueLines[i].speaker;
        queuedLine.displayName = dialogueLines[i].displayName;
        queuedLine.actorFormId = dialogueLines[i].actorFormId;
        queuedLine.isFinalResponseLine = isFinalResponseLine;
        queuedLine.listenerHint = listenerHint;
        queuedLine.rechatTargetHint = dialogueLines[i].rechatTargetHint;
        queuedLine.listenerFormId = dialogueLines[i].listenerFormId;
        queuedLine.rechatTargetFormId = dialogueLines[i].rechatTargetFormId;
        queuedLine.rechatDepth = dialogueLines[i].rechatDepth;
        queuedLine.ttsCacheKey = dialogueLines[i].ttsCacheKey;
        queuedLine.utteranceId = dialogueLines[i].utteranceId;
        queuedLine.requestId = dialogueLines[i].requestId;
        queuedLine.responseGeneration = responseGeneration;
        ResponseQueueFNV::EnqueueDialogue(queuedLine, source ? source : "ResponseRouter");
        Logger::LogInfo("%s: Enqueued dialogue final=%d actor=0x%08X",
            source ? source : "ResponseRouter",
            isFinalResponseLine ? 1 : 0,
            dialogueLines[i].actorFormId);
    }

    for (const std::string& actionLine : actionLines) {
        ResponseQueueFNV::EnqueueAction(actionLine, source ? source : "ResponseRouter", responseGeneration);
    }

    Logger::LogInfo("%s: Finished processing JSON response (%zu lines)",
        source ? source : "ResponseRouter",
        responseLines.size());
    return !dialogueLines.empty() || closeRequested || !responseLines.empty();
}

bool ProcessJsonActionsOnly(const std::string& response, const char* source, uint64_t responseGeneration) {
    if (response.empty()) {
        return false;
    }

    std::vector<std::string> responseLines = ExtractJsonArrayObjects(response, "lines");
    if (responseLines.empty()) {
        return false;
    }

    bool processed = false;
    for (const std::string& lineObject : responseLines) {
        if (ExtractJsonStringValue(lineObject, "command_name") == "DirectorScene") {
            processed = QueueDirectorScene(lineObject, source, responseGeneration) || processed;
            continue;
        }
        const std::string action = Trim(ExtractJsonStringValue(lineObject, "action"));
        const bool isActionCommand = action == "rolecommand" || ActionManager::IsActionCommand(action);
        if (!isActionCommand) {
            continue;
        }

        const std::string speaker = Trim(ExtractJsonStringValue(lineObject, "speaker"));
        const std::string commandName = Trim(ExtractJsonStringValue(lineObject, "command_name"));
        Logger::LogInfo("%s: Final action pass routing action=[%s] command=[%s] speaker=[%s]",
            source ? source : "ResponseRouter",
            action.c_str(),
            commandName.c_str(),
            speaker.c_str());

        ResponseQueueFNV::EnqueueAction(lineObject, source ? source : "ResponseRouter", responseGeneration);
        processed = true;
    }

    return processed;
}

} // namespace ResponseRouter
