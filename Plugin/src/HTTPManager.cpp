#include "HTTPManager.h"
#include "ActorPositionResolverFNV.h"
#include "Config.h"
#include "Misc.h"
#include "ResponseQueueFNV.h"
#include "ResponseRouter.h"
#include "SpeakManager.h"
#include "TaskManager.h"
#include "RuntimeGeneration.h"
#include "WorldContextFNV.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <winhttp.h>
#include <sstream>
#include <iomanip>
#include <mutex>
#include <deque>
#include <chrono>
#include <atomic>
#include <vector>
#include <functional>
#include <algorithm>
#include <memory>
#include <unordered_map>

#ifndef DIALECTIC_VERSION
#define DIALECTIC_VERSION "0.5.2"
#endif

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winhttp.lib")

// Forward declare logging function
void Log(const char* fmt, ...);

// Undefine Windows SendMessage macro to avoid conflicts
#ifdef SendMessage
#undef SendMessage
#endif

namespace HTTPManager {

    static std::atomic_bool g_initialized{false};
    static std::atomic<uint64_t> g_responseGeneration{0};
    static std::atomic<uint64_t> g_totalResponsesQueued{0};
    static std::mutex g_streamStateMutex;
    static bool g_streamInProgress = false;
    static bool g_streamCancellationRequested = false;
    static int g_activeStreamTasks = 0;
    static std::string g_activeStreamName;
    static std::atomic<uint64_t> g_telemetrySkippedDuringStream{0};
    static std::atomic<long long> g_hotInputUntilMs{0};

    static std::atomic<uint64_t> g_totalTasksQueued{0};
    static std::atomic<uint64_t> g_totalTasksCompleted{0};
    static std::atomic<uint64_t> g_totalTasksCancelled{0};
    thread_local const TaskManager::CancellationToken* t_currentHttpToken = nullptr;

    static std::string SendJsonRequest(
        const std::string& endpoint,
        const std::string& jsonBody,
        bool streamResponse = false,
        const std::function<void(const std::string&)>& streamCallback = {},
        const std::function<bool()>& cancelRequested = {});

    static bool ProcessJsonResponsePayload(const std::string& response, const char* source, uint64_t generation = 0);

    static bool IsCurrentHttpTaskCancelled() {
        const TaskManager::CancellationToken* token = t_currentHttpToken
            ? t_currentHttpToken
            : TaskManager::CurrentToken();
        return token && token->IsCancellationRequested();
    }

    static void EnsureWorkersStarted() {
        // FNVRuntime owns the shared TaskManager lifetime.
    }

    static uint64_t EnqueueHttpTask(const std::string& type,
                                    std::function<void()> task,
                                    const std::string& key = "",
                                    bool priority = false) {
        if (!task) return 0;
        const std::string taskType = "http:" + type;
        const std::string taskKey = "http:" + key;
        const bool turnScoped = type == "HTTPStream" || type == "HTTPStreamRechat" ||
            type == "HTTPStreamEvent" || type == "PlayerTTS";
        TaskManager::Options options;
        options.type = taskType;
        options.key = taskKey;
        options.generation = RuntimeGeneration::Current();
        options.scope.turnId = turnScoped ? g_responseGeneration.load() : 0;
        options.lane = TaskManager::Lane::Interactive;
        options.priority = priority;
        options.deadlineFromEnqueue = true;
        const bool responseStream = type == "HTTPStream" || type == "HTTPStreamRechat" ||
            type == "HTTPStreamEvent";
        if (responseStream) {
            options.timeout = std::chrono::seconds(std::max(30, Config::aiResponseTimeout + 10));
            options.concurrencyLimit = 1;
        } else if (type == "PlayerTTS") {
            options.timeout = std::chrono::seconds(60);
            options.concurrencyLimit = 1;
        } else {
            options.timeout = std::chrono::seconds(45);
        }
        const TaskManager::TaskHandle handle = TaskManager::Submit(std::move(options),
            [type, key, task = std::move(task)](
                const TaskManager::CancellationToken& token) mutable {
                t_currentHttpToken = &token;
                try {
                    Log("HTTPManager: shared worker running type=%s key=%s", type.c_str(), key.c_str());
                    if (!token.IsCancellationRequested()) task();
                } catch (...) {
                    Log("HTTPManager: shared worker task type=%s key=%s threw exception",
                        type.c_str(), key.c_str());
                }
                t_currentHttpToken = nullptr;
                g_totalTasksCompleted.fetch_add(1);
            });
        if (!handle) {
            Log("HTTPManager: shared task rejected type=%s key=%s", type.c_str(), key.c_str());
            return 0;
        }

        g_totalTasksQueued.fetch_add(1);
        Log("HTTPManager: queued shared task id=%llu type=%s key=%s priority=%d",
            static_cast<unsigned long long>(handle.id),
            type.c_str(), key.c_str(), priority ? 1 : 0);
        return handle.id;
    }

    static int CancelHttpTasksByType(const std::string& type) {
        const int cancelled = static_cast<int>(TaskManager::CancelByType("http:" + type));
        if (cancelled > 0) g_totalTasksCancelled.fetch_add(static_cast<uint64_t>(cancelled));
        Log("HTTPManager: cancelled %d shared task(s) by type=%s", cancelled, type.c_str());
        return cancelled;
    }

    static int CancelHttpTasksByKey(const std::string& key) {
        const int cancelled = static_cast<int>(TaskManager::CancelByKey("http:" + key));
        if (cancelled > 0) g_totalTasksCancelled.fetch_add(static_cast<uint64_t>(cancelled));
        Log("HTTPManager: cancelled %d shared task(s) by key=%s", cancelled, key.c_str());
        return cancelled;
    }

    static void StopWorkers() {
        const std::size_t cancelled = TaskManager::CancelByTypePrefix("http:");
        if (cancelled > 0) g_totalTasksCancelled.fetch_add(cancelled);
    }
    static std::string Trim(const std::string& value) {
        const char* whitespace = " \t\r\n";
        const size_t start = value.find_first_not_of(whitespace);
        if (start == std::string::npos) {
            return "";
        }
        const size_t end = value.find_last_not_of(whitespace);
        return value.substr(start, end - start + 1);
    }

    static bool HasUsableJsonResponseLine(const std::string& response) {
        return response.find("\"lines\"") != std::string::npos &&
               (response.find("\"text\"") != std::string::npos ||
                response.find("\"rolecommand\"") != std::string::npos ||
                response.find("\"command_name\"") != std::string::npos);
    }

    static bool HasJsonActionLine(const std::string& response) {
        return response.find("\"lines\"") != std::string::npos &&
               (response.find("\"rolecommand\"") != std::string::npos ||
                response.find("\"command_name\"") != std::string::npos);
    }

    static bool HasJsonCloseRequested(const std::string& response) {
        return response.find("\"close\":true") != std::string::npos ||
               response.find("\"close\": true") != std::string::npos;
    }

    static bool IsQueueableJsonResponse(const std::string& response) {
        return HasUsableJsonResponseLine(response) || HasJsonCloseRequested(response);
    }

    static bool IsJsonStreamCompletionEnvelope(const std::string& response) {
        const std::string line = Trim(response);
        return !line.empty() &&
               line.find("\"schema\":\"dialectic.response.v1\"") != std::string::npos &&
               line.find("\"lines\":[]") != std::string::npos;
    }

    static bool IsGamedataEndpoint(const std::string& endpoint) {
        return endpoint.find("gamedata.php") != std::string::npos;
    }

    static void ExtendHotInputWindow(int durationMs, const char* reason) {
        const long long until = Misc::GetCurrentTimeMillis() + durationMs;
        long long current = g_hotInputUntilMs.load();
        while (until > current && !g_hotInputUntilMs.compare_exchange_weak(current, until)) {
        }
        Log("HTTPManager: Hot input HTTP window extended for %dms (%s)", durationMs, reason ? reason : "input");
    }

    static bool IsHotInputWindowActive() {
        const long long until = g_hotInputUntilMs.load();
        return until > 0 && Misc::GetCurrentTimeMillis() < until;
    }

    static bool ShouldThrottleTelemetryEndpoint(const std::string& endpoint) {
        return IsGamedataEndpoint(endpoint) && (IsStreamInProgress() || IsHotInputWindowActive());
    }

    static bool IsPromptCriticalGamedataPayload(const std::string& jsonBody) {
        return jsonBody.find("\"type\":\"world_context\"") != std::string::npos ||
               jsonBody.find("\"type\":\"actor_profile\"") != std::string::npos ||
               jsonBody.find("\"type\":\"equipment\"") != std::string::npos ||
               jsonBody.find("\"type\":\"inventory\"") != std::string::npos ||
               jsonBody.find("\"type\":\"active_quests\"") != std::string::npos ||
               jsonBody.find("\"type\":\"nearby_actors\"") != std::string::npos ||
               jsonBody.find("\"type\":\"nearby_items\"") != std::string::npos ||
               jsonBody.find("\"type\":\"points_of_interest\"") != std::string::npos ||
               jsonBody.find("\"type\":\"activity_status_bulk\"") != std::string::npos;
    }

    static bool IsUsableDetectedPlayerName(const std::string& value) {
        const std::string name = Trim(value);
        if (name.empty() || name.size() > 80) {
            return false;
        }

        const std::string lower = Misc::ToLower(name);
        return lower != "player" &&
               lower != "courier" &&
               lower != "prisoner" &&
               lower != "unknown" &&
               lower != "unknown player" &&
               lower != "the narrator";
    }

    static std::string CurrentPlayerName() {
        const std::string detectedName = Trim(Misc::GetPlayerName());
        if (IsUsableDetectedPlayerName(detectedName)) {
            Config::playerName = detectedName;
            return detectedName;
        }

        const std::string configuredName = Trim(Config::playerName);
        return configuredName.empty() ? "Player" : configuredName;
    }

    static std::string ExtractJsonStringValue(const std::string& json, const std::string& key) {
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

    static int ExtractJsonIntValue(const std::string& json, const std::string& key, int fallback = 0) {
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

    static std::string AttachRechatDepthToResponse(const std::string& response, int rechatDepth) {
        if (response.empty() || response.find("\"rechat_depth\"") != std::string::npos) {
            return response;
        }

        std::string annotated = response;
        size_t end = annotated.find_last_not_of(" \t\r\n");
        if (end == std::string::npos || annotated[end] != '}') {
            return response;
        }

        annotated.insert(end, ",\"rechat_depth\":" + std::to_string(rechatDepth));
        return annotated;
    }

    static bool ProcessFinalJsonResponseIfCurrent(const std::string& response, uint64_t generation, const char* source) {
        if (response.empty()) {
            return false;
        }

        if (!ResponseQueueFNV::IsCurrentGeneration(generation) || generation != g_responseGeneration.load()) {
            Log("HTTPManager: Dropping stale final JSON response generation=%llu active=%llu current=%llu source=%s",
                static_cast<unsigned long long>(generation),
                static_cast<unsigned long long>(ResponseQueueFNV::GetStatus().activeGeneration),
                static_cast<unsigned long long>(g_responseGeneration.load()),
                source ? source : "HTTPManager");
            return false;
        }

        if (!IsQueueableJsonResponse(response)) {
            return false;
        }

        g_totalResponsesQueued.fetch_add(1);
        return ProcessJsonResponsePayload(response, source ? source : "HTTPManager:final-json", generation);
    }

    static bool ProcessJsonResponsePayload(const std::string& response, const char* source, uint64_t generation) {
        bool processed = false;
        std::istringstream stream(response);
        std::string line;
        while (std::getline(stream, line)) {
            line = Trim(line);
            if (line.empty()) {
                continue;
            }

            processed = ResponseRouter::ProcessJsonResponse(line, source, generation) || processed;
        }

        if (!processed) {
            processed = ResponseRouter::ProcessJsonResponse(response, source, generation);
        }

        return processed;
    }

    static bool IsCurrentGeneration(uint64_t generation) {
        return generation == g_responseGeneration.load() && ResponseQueueFNV::IsCurrentGeneration(generation);
    }

    static void BeginStreamTask(const std::string& taskName, uint64_t generation) {
        std::lock_guard<std::mutex> lock(g_streamStateMutex);
        g_streamInProgress = true;
        g_streamCancellationRequested = false;
        ++g_activeStreamTasks;
        g_activeStreamName = taskName;
        ResponseQueueFNV::MarkUnfinished(true, taskName.c_str(), generation);
        Log("HTTPManager: Stream task started [%s] generation=%llu active=%d",
            taskName.c_str(),
            static_cast<unsigned long long>(generation),
            g_activeStreamTasks);
    }

    static void FinishStreamTask(const std::string& taskName, uint64_t generation) {
        std::lock_guard<std::mutex> lock(g_streamStateMutex);
        if (g_activeStreamTasks > 0) {
            --g_activeStreamTasks;
        }

        if (generation != g_responseGeneration.load()) {
            Log("HTTPManager: Stale stream task finished [%s] generation=%llu current=%llu active=%d",
                taskName.c_str(),
                static_cast<unsigned long long>(generation),
                static_cast<unsigned long long>(g_responseGeneration.load()),
                g_activeStreamTasks);
            if (g_activeStreamTasks == 0) {
                g_streamInProgress = false;
                g_activeStreamName.clear();
                ResponseQueueFNV::MarkUnfinished(false, taskName.c_str(), generation);
            }
            return;
        }

        if (g_activeStreamTasks == 0) {
            g_streamInProgress = false;
            g_streamCancellationRequested = false;
            g_activeStreamName.clear();
            ResponseQueueFNV::MarkUnfinished(false, taskName.c_str(), generation);
        }
        Log("HTTPManager: Stream task finished [%s] generation=%llu current=%llu active=%d",
            taskName.c_str(),
            static_cast<unsigned long long>(generation),
            static_cast<unsigned long long>(g_responseGeneration.load()),
            g_activeStreamTasks);
    }

    static void MarkLogicalStreamComplete(const std::string& taskName, uint64_t generation) {
        std::lock_guard<std::mutex> lock(g_streamStateMutex);
        if (generation != g_responseGeneration.load()) {
            Log("HTTPManager: Ignored stale logical stream completion [%s] generation=%llu current=%llu active=%d",
                taskName.c_str(),
                static_cast<unsigned long long>(generation),
                static_cast<unsigned long long>(g_responseGeneration.load()),
                g_activeStreamTasks);
            return;
        }

        if (!g_streamInProgress) {
            ResponseQueueFNV::MarkUnfinished(false, taskName.c_str(), generation);
            return;
        }

        g_streamInProgress = false;
        g_activeStreamName.clear();
        ResponseQueueFNV::MarkUnfinished(false, taskName.c_str(), generation);
        Log("HTTPManager: Logical stream complete [%s] generation=%llu active=%d",
            taskName.c_str(),
            static_cast<unsigned long long>(generation),
            g_activeStreamTasks);
    }

    static void MarkStreamCancellationRequested() {
        std::lock_guard<std::mutex> lock(g_streamStateMutex);
        g_streamCancellationRequested = true;
        g_streamInProgress = false;
        g_activeStreamTasks = 0;
        g_activeStreamName.clear();
        ResponseQueueFNV::Clear("http_cancel");
    }

    void Initialize() {
        if (g_initialized) {
            return;
        }

        WSADATA wsaData;
        int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (result != 0) {
            Log("HTTPManager: WSAStartup failed with error: %d", result);
            return;
        }

        g_initialized = true;
        EnsureWorkersStarted();
        Log("HTTPManager: Initialized");
    }

    void Shutdown() {
        if (g_initialized) {
            StopWorkers();
            WSACleanup();
            g_initialized = false;
            Log("HTTPManager: Shutdown");
        }
    }

    std::string EscapeJson(const std::string& input) {
        std::string out;
        out.reserve(input.size());

        for (char c : input) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\t': out += "\\t"; break;
                case '\r': out += "\\r"; break;
                default:   out += c; break;
            }
        }
        return out;
    }

    std::string UrlEncode(const std::string& value) {
        std::ostringstream escaped;
        escaped.fill('0');
        escaped << std::hex;

        for (char c : value) {
            // Keep alphanumeric and other safe characters intact
            if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
                escaped << c;
                continue;
            }

            // Any other characters are percent-encoded
            escaped << std::uppercase;
            escaped << '%' << std::setw(2) << int((unsigned char)c);
            escaped << std::nouppercase;
        }

        return escaped.str();
    }

    static int HexValue(char c) {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return 10 + (c - 'a');
        }
        if (c >= 'A' && c <= 'F') {
            return 10 + (c - 'A');
        }
        return -1;
    }

    static std::string UrlDecode(const std::string& value) {
        std::string decoded;
        decoded.reserve(value.size());

        for (size_t i = 0; i < value.size(); ++i) {
            const char c = value[i];
            if (c == '+') {
                decoded += ' ';
                continue;
            }
            if (c == '%' && i + 2 < value.size()) {
                const int high = HexValue(value[i + 1]);
                const int low = HexValue(value[i + 2]);
                if (high >= 0 && low >= 0) {
                    decoded += static_cast<char>((high << 4) | low);
                    i += 2;
                    continue;
                }
            }
            decoded += c;
        }

        return decoded;
    }

    static std::string ExtractQueryParam(const std::string& payload, const std::string& key) {
        size_t start = 0;
        while (start <= payload.size()) {
            size_t end = payload.find('&', start);
            if (end == std::string::npos) {
                end = payload.size();
            }

            const std::string part = payload.substr(start, end - start);
            const size_t equals = part.find('=');
            if (equals != std::string::npos) {
                const std::string paramName = UrlDecode(part.substr(0, equals));
                if (paramName == key) {
                    return UrlDecode(part.substr(equals + 1));
                }
            }

            if (end == payload.size()) {
                break;
            }
            start = end + 1;
        }

        return "";
    }

    // Get current timestamp in milliseconds
    static int64_t GetTimestamp() {
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    static bool LooksLikeJsonValue(const std::string& value) {
        const std::string trimmed = Trim(value);
        return !trimmed.empty() && (trimmed[0] == '{' || trimmed[0] == '[');
    }

    static std::string FormatEventJson(const std::string& eventType,
                                       const std::string& payload,
                                       const std::string& explicitAudienceSnapshot = "") {
        const int64_t ts = GetTimestamp();
        const long long gameTs = WorldContextFNV::GetGameTimestamp();
        const std::string audienceSnapshot = !explicitAudienceSnapshot.empty()
            ? explicitAudienceSnapshot
            : ExtractQueryParam(payload, "audience_snapshot");
        std::ostringstream json;
        json << "{";
        json << "\"schema\":\"dialectic.event.v1\",";
        json << "\"type\":\"" << EscapeJson(eventType) << "\",";
        json << "\"ts\":" << ts << ",";
        json << "\"gamets\":" << (gameTs > 0 ? gameTs : 0) << ",";
        json << "\"game\":\"fnv\",";
        json << "\"payload\":";
        if (LooksLikeJsonValue(payload)) {
            json << payload;
        } else {
            json << "\"" << EscapeJson(payload) << "\"";
        }
        if (!audienceSnapshot.empty() && LooksLikeJsonValue(audienceSnapshot)) {
            json << ",\"audience_snapshot\":" << audienceSnapshot;
        }
        json << "}";
        return json.str();
    }

    void LogEvent(const std::string& msg) {
        std::string jsonBody = FormatEventJson("log", msg);
        EnqueueHttpTask("log", [jsonBody]() {
            SendJsonRequest("main.php", jsonBody);
        });
    }

    void LogEvent(const std::string& msg, const std::string& forcedActor) {
        std::string jsonBody = FormatEventJson("log", forcedActor + ": " + msg);
        EnqueueHttpTask("log", [jsonBody]() {
            SendJsonRequest("main.php", jsonBody);
        });
    }

    void Stream(const std::string& msg) {
        Stream(msg, 0);
    }

    static bool IsPlayerInputEvent(const std::string& eventType) {
        return eventType == "inputtext" ||
            eventType == "inputtext_s" ||
            eventType == "ginputtext" ||
            eventType == "ginputtext_s" ||
            eventType == "narrator_inputtext";
    }

    void Stream(const std::string& msg, int rechatDepth) {
        std::string jsonBody = FormatEventJson("inputtext", msg);
        const uint64_t generation = g_responseGeneration.load();
        ExtendHotInputWindow(12000, rechatDepth > 0 ? "rechat stream" : "input stream");
        ResponseQueueFNV::SetActiveGeneration(generation, "inputtext");
        EnqueueHttpTask("HTTPStream", [jsonBody, generation]() {
            if (generation != g_responseGeneration.load()) {
                Log("HTTPManager: Skipping stale queued inputtext task generation=%llu current=%llu",
                    static_cast<unsigned long long>(generation),
                    static_cast<unsigned long long>(g_responseGeneration.load()));
                return;
            }
            BeginStreamTask("HTTPStream", generation);
            bool queuedStreamedResponse = false;
            std::string response = SendJsonRequest(
                "main.php",
                jsonBody,
                true,
                [generation, &queuedStreamedResponse](const std::string& envelope) {
                    if (generation != g_responseGeneration.load()) {
                        Log("HTTPManager: Dropping stale streamed inputtext envelope");
                        return;
                    }
                    if (IsJsonStreamCompletionEnvelope(envelope)) {
                        MarkLogicalStreamComplete("HTTPStream", generation);
                        return;
                    }
                    queuedStreamedResponse = ResponseRouter::ProcessJsonResponse(envelope, "HTTPManager:inputtext-stream", generation) ||
                        queuedStreamedResponse;
                },
                [generation]() { return !IsCurrentGeneration(generation) || IsCurrentHttpTaskCancelled(); });

            if (!queuedStreamedResponse && generation == g_responseGeneration.load() && IsQueueableJsonResponse(response)) {
                queuedStreamedResponse = ProcessJsonResponsePayload(response, "HTTPManager:inputtext-final", generation);
                if (!queuedStreamedResponse) {
                    ProcessFinalJsonResponseIfCurrent(response, generation, "HTTPManager:inputtext-final-json");
                }
            } else if (queuedStreamedResponse && generation == g_responseGeneration.load() && HasJsonActionLine(response)) {
                ResponseRouter::ProcessJsonActionsOnly(response, "HTTPManager:inputtext-final-actions", generation);
            }
            FinishStreamTask("HTTPStream", generation);
        }, "inputtext", true);
    }

    // Send a game event to DialecticServer.
    void SendEvent(const std::string& eventType,
                   const std::string& payload,
                   const std::string& audienceSnapshotJson) {
        std::string jsonBody = FormatEventJson(eventType, payload, audienceSnapshotJson);
        Log("HTTPManager: Sending event [%s]: %s", eventType.c_str(), 
            jsonBody.length() > 100 ? jsonBody.substr(0, 100).c_str() : jsonBody.c_str());

        if (IsPlayerInputEvent(eventType)) {
            ExtendHotInputWindow(12000, "input event");
        }

        if (eventType == "rechat") {
            const std::string speaker = ExtractJsonStringValue(payload, "speaker");
            const int rechatDepth = ExtractJsonIntValue(payload, "rechat_depth", 0);
            if (speaker.empty()) {
                Log("HTTPManager: Rechat event missing speaker; sending without attempt tracking");
            }

            const uint64_t generation = g_responseGeneration.load();
                ResponseQueueFNV::SetActiveGeneration(generation, "rechat");
                EnqueueHttpTask("HTTPStreamRechat", [jsonBody, speaker, rechatDepth, generation]() {
                if (generation != g_responseGeneration.load()) {
                    Log("HTTPManager: Skipping stale queued rechat task generation=%llu current=%llu",
                        static_cast<unsigned long long>(generation),
                        static_cast<unsigned long long>(g_responseGeneration.load()));
                    SpeakManager::CompleteRechatAttempt(speaker, false);
                    return;
                }
                BeginStreamTask("HTTPStreamRechat", generation);
                bool queuedUsableLine = false;
                bool rechatAttemptCompleted = false;
                std::string response = SendJsonRequest(
                    "main.php",
                    jsonBody,
                    true,
                    [speaker, rechatDepth, generation, &queuedUsableLine, &rechatAttemptCompleted](const std::string& envelope) {
                        if (generation != g_responseGeneration.load()) {
                            return;
                        }

                        if (IsJsonStreamCompletionEnvelope(envelope)) {
                            MarkLogicalStreamComplete("HTTPStreamRechat", generation);
                            return;
                        }

                        if (!HasUsableJsonResponseLine(envelope)) {
                            return;
                        }

                        std::string annotated = AttachRechatDepthToResponse(envelope, rechatDepth + 1);
                        const bool queuedThisLine =
                            ResponseRouter::ProcessJsonResponse(annotated, "HTTPManager:rechat-stream", generation);
                        queuedUsableLine = queuedThisLine || queuedUsableLine;
                        if (queuedThisLine && !rechatAttemptCompleted) {
                            SpeakManager::CompleteRechatAttempt(speaker, true);
                            rechatAttemptCompleted = true;
                        }
                    },
                    [generation]() { return !IsCurrentGeneration(generation) || IsCurrentHttpTaskCancelled(); });
                const bool stale = generation != g_responseGeneration.load();
                const bool hasUsableLine = !stale && (queuedUsableLine || HasUsableJsonResponseLine(response));
                if (!rechatAttemptCompleted) {
                    SpeakManager::CompleteRechatAttempt(speaker, hasUsableLine);
                    rechatAttemptCompleted = hasUsableLine;
                }
                if (!stale && hasUsableLine && !queuedUsableLine) {
                    response = AttachRechatDepthToResponse(response, rechatDepth + 1);
                    if (!ProcessJsonResponsePayload(response, "HTTPManager:rechat-final", generation)) {
                        ProcessFinalJsonResponseIfCurrent(response, generation, "HTTPManager:rechat-final-json");
                    }
                } else if (!stale && queuedUsableLine && HasJsonActionLine(response)) {
                    response = AttachRechatDepthToResponse(response, rechatDepth + 1);
                    ResponseRouter::ProcessJsonActionsOnly(response, "HTTPManager:rechat-final-actions", generation);
                } else if (stale) {
                    Log("HTTPManager: Dropping stale JSON rechat response");
                } else {
                    Log("HTTPManager: JSON rechat response had no usable dialogue lines");
                }
                FinishStreamTask("HTTPStreamRechat", generation);
            }, speaker.empty() ? "rechat" : speaker, true);
            return;
        }
        
        const uint64_t generation = g_responseGeneration.load();
        const bool eventOnlyResponse =
            eventType == "captured_dialogue" ||
            eventType == "setconf" ||
            eventType == "goodnight" ||
            eventType == "waitstart" ||
            eventType == "waitstop";
        const bool queueResponse = !eventOnlyResponse;
        if (queueResponse) {
            ResponseQueueFNV::SetActiveGeneration(generation, eventType.c_str());
        }
        EnqueueHttpTask(queueResponse ? "HTTPStreamEvent" : eventType, [jsonBody, generation, eventType, queueResponse]() {
            const bool streamTask = queueResponse;
            if (queueResponse && generation != g_responseGeneration.load()) {
                Log("HTTPManager: Skipping stale queued %s task generation=%llu current=%llu",
                    eventType.c_str(),
                    static_cast<unsigned long long>(generation),
                    static_cast<unsigned long long>(g_responseGeneration.load()));
                return;
            }
            if (streamTask) {
            BeginStreamTask(queueResponse ? "HTTPStreamEvent" : eventType, generation);
            }
            bool queuedStreamedResponse = false;
            std::string response = SendJsonRequest(
                "main.php",
                jsonBody,
                queueResponse,
                [generation, &queuedStreamedResponse](const std::string& envelope) {
                    if (generation != g_responseGeneration.load()) {
                        Log("HTTPManager: Dropping stale streamed event envelope");
                        return;
                    }
                    if (IsJsonStreamCompletionEnvelope(envelope)) {
                        MarkLogicalStreamComplete("HTTPStreamEvent", generation);
                        return;
                    }
                    queuedStreamedResponse = ResponseRouter::ProcessJsonResponse(envelope, "HTTPManager:event-stream", generation) ||
                        queuedStreamedResponse;
                },
                [generation, queueResponse]() { return queueResponse && (!IsCurrentGeneration(generation) || IsCurrentHttpTaskCancelled()); });
            const bool stale = generation != g_responseGeneration.load();
            const bool generationIndependentResponse = eventType == "setconf";
            if ((!stale || generationIndependentResponse) && !response.empty() && !queuedStreamedResponse) {
                Log("HTTPManager: Received response: %s", 
                    response.length() > 100 ? response.substr(0, 100).c_str() : response.c_str());
                if (!queueResponse) {
                    if (eventType == "setconf" ||
                        eventType == "goodnight" ||
                        eventType == "waitstart" ||
                        eventType == "waitstop") {
                        const uint64_t commandGeneration = g_responseGeneration.load();
                        ResponseRouter::ProcessJsonActionsOnly(
                            response,
                            "HTTPManager:setconf-ack",
                            commandGeneration);
                    }
                    Log("HTTPManager: Not queueing %s response for dialogue playback", eventType.c_str());
                    return;
                }
                if (!ProcessJsonResponsePayload(response, "HTTPManager:event-final", generation)) {
                    ProcessFinalJsonResponseIfCurrent(response, generation, "HTTPManager:event-final-json");
                }
            } else if (stale) {
                Log("HTTPManager: Dropping stale event response");
            } else if (queuedStreamedResponse) {
                if (queueResponse && HasJsonActionLine(response)) {
                    ResponseRouter::ProcessJsonActionsOnly(response, "HTTPManager:event-final-actions", generation);
                }
                Log("HTTPManager: Queued streamed %s response", eventType.c_str());
            } else {
                Log("HTTPManager: Empty or failed response");
            }
            if (streamTask) {
                FinishStreamTask("HTTPStreamEvent", generation);
            }
        }, eventType, queueResponse);
    }

    void CancelPendingResponses() {
        const uint64_t previousGeneration = g_responseGeneration.fetch_add(1);
        const uint64_t nextGeneration = previousGeneration + 1;
        ResponseQueueFNV::SetActiveGeneration(nextGeneration, "http_cancel");
        const std::size_t turnCancelled = TaskManager::CancelByTurn(previousGeneration);
        if (turnCancelled > 0) {
            g_totalTasksCancelled.fetch_add(static_cast<uint64_t>(turnCancelled));
            Log("HTTPManager: cancelled %zu shared task(s) for response turn=%llu",
                turnCancelled,
                static_cast<unsigned long long>(previousGeneration));
        }
        CancelTasksByType("HTTPStream");
        CancelTasksByType("HTTPStreamRechat");
        CancelTasksByType("HTTPStreamEvent");
        MarkStreamCancellationRequested();
        Log("HTTPManager: Cancelled pending responses and advanced generation=%llu",
            static_cast<unsigned long long>(nextGeneration));
    }

    int CancelTasksByType(const std::string& taskType) {
        return CancelHttpTasksByType(taskType);
    }

    int CancelTasksByKey(const std::string& taskKey) {
        return CancelHttpTasksByKey(taskKey);
    }

    static std::string SendJsonRequest(
        const std::string& endpoint,
        const std::string& jsonBody,
        bool streamResponse,
        const std::function<void(const std::string&)>& streamCallback,
        const std::function<bool()>& cancelRequested) {
        if (!g_initialized) {
            Log("HTTPManager: Not initialized, cannot send JSON");
            return "";
        }

        const TaskManager::CancellationToken* currentTaskToken = t_currentHttpToken
            ? t_currentHttpToken
            : TaskManager::CurrentToken();
        if ((currentTaskToken && currentTaskToken->IsCancellationRequested()) ||
            (cancelRequested && cancelRequested())) {
            return "";
        }

        try {
            std::string serverPath = Config::serverPath;
            size_t queryPos = serverPath.find('?');
            if (queryPos != std::string::npos) {
                serverPath = serverPath.substr(0, queryPos);
            }

            size_t slashPos = serverPath.find_last_of("/\\");
            if (slashPos != std::string::npos) {
                serverPath = serverPath.substr(0, slashPos + 1) + endpoint;
            } else {
                serverPath = endpoint;
            }

            if (!serverPath.empty() && serverPath[0] != '/') {
                serverPath = "/" + serverPath;
            }

            std::wstring wideServer(Config::serverHost.begin(), Config::serverHost.end());
            std::wstring widePath(serverPath.begin(), serverPath.end());
            const std::string userAgent = std::string("Dialectic/") + DIALECTIC_VERSION;
            const std::wstring wideUserAgent(userAgent.begin(), userAgent.end());
            std::wstring wideHeaders = streamResponse
                ? L"Content-Type: application/json\r\nAccept: application/x-ndjson\r\nX-Dialectic-Stream: 1\r\nConnection: close"
                : L"Content-Type: application/json\r\nAccept: application/json\r\nConnection: close";

            HINTERNET hSession = WinHttpOpen(
                wideUserAgent.c_str(),
                WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                WINHTTP_NO_PROXY_NAME,
                WINHTTP_NO_PROXY_BYPASS,
                0);

            if (!hSession) {
                Log("HTTPManager: WinHttpOpen failed for JSON POST");
                return "";
            }

            HINTERNET hConnect = WinHttpConnect(
                hSession,
                wideServer.c_str(),
                Config::serverPort,
                0);

            if (!hConnect) {
                Log("HTTPManager: WinHttpConnect failed for JSON POST");
                WinHttpCloseHandle(hSession);
                return "";
            }

            HINTERNET hRequest = WinHttpOpenRequest(
                hConnect,
                L"POST",
                widePath.c_str(),
                NULL,
                WINHTTP_NO_REFERER,
                WINHTTP_DEFAULT_ACCEPT_TYPES,
                0);

            if (!hRequest) {
                Log("HTTPManager: WinHttpOpenRequest failed for JSON POST");
                WinHttpCloseHandle(hConnect);
                WinHttpCloseHandle(hSession);
                return "";
            }
            auto interruptibleRequest = std::make_shared<std::atomic<HINTERNET>>(hRequest);
            if (currentTaskToken) {
                currentTaskToken->SetInterrupt([interruptibleRequest]() {
                    HINTERNET request = interruptibleRequest->exchange(nullptr);
                    if (request) WinHttpCloseHandle(request);
                });
            }
            auto closeRequest = [&]() {
                if (currentTaskToken) currentTaskToken->ClearInterrupt();
                HINTERNET request = interruptibleRequest->exchange(nullptr);
                if (request) WinHttpCloseHandle(request);
            };

            DWORD timeout = Config::aiResponseTimeout * 1000;
            WinHttpSetTimeouts(hRequest, timeout, timeout, timeout, timeout);

            if (!WinHttpAddRequestHeaders(hRequest, wideHeaders.c_str(), -1, WINHTTP_ADDREQ_FLAG_ADD)) {
                Log("HTTPManager: WinHttpAddRequestHeaders failed for JSON POST");
                closeRequest();
                WinHttpCloseHandle(hConnect);
                WinHttpCloseHandle(hSession);
                return "";
            }

            const auto requestStart = std::chrono::steady_clock::now();

            if (!WinHttpSendRequest(
                hRequest,
                WINHTTP_NO_ADDITIONAL_HEADERS,
                0,
                const_cast<char*>(jsonBody.data()),
                static_cast<DWORD>(jsonBody.size()),
                static_cast<DWORD>(jsonBody.size()),
                0)) {
                Log("HTTPManager: WinHttpSendRequest failed for JSON POST");
                closeRequest();
                WinHttpCloseHandle(hConnect);
                WinHttpCloseHandle(hSession);
                return "";
            }

            if (!WinHttpReceiveResponse(hRequest, NULL)) {
                Log("HTTPManager: WinHttpReceiveResponse failed for JSON POST");
                closeRequest();
                WinHttpCloseHandle(hConnect);
                WinHttpCloseHandle(hSession);
                return "";
            }

            std::string responseBody;
            std::string lineBuffer;
            DWORD bytesAvailable = 0;
            DWORD bytesRead = 0;
            char buffer[4096];
            bool loggedFirstStreamLine = false;
            std::size_t streamedLineCount = 0;

            auto processStreamLine = [&](const std::string& rawLine) {
                std::string line = Trim(rawLine);
                if (line.empty()) {
                    return;
                }

                if (streamCallback) {
                    ++streamedLineCount;
                    if (!loggedFirstStreamLine) {
                        loggedFirstStreamLine = true;
                        const auto firstLineMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - requestStart).count();
                        Log("HTTPManager: first streamed JSON envelope from %s after %lldms (%zu bytes)",
                            endpoint.c_str(),
                            static_cast<long long>(firstLineMs),
                            line.size());
                    }
                    streamCallback(line);
                }
            };

            do {
                if (cancelRequested && cancelRequested()) {
                    Log("HTTPManager: JSON POST %s cancelled before read", endpoint.c_str());
                    break;
                }

                bytesAvailable = 0;
                if (!WinHttpQueryDataAvailable(hRequest, &bytesAvailable)) {
                    break;
                }

                if (bytesAvailable > 0) {
                    if (cancelRequested && cancelRequested()) {
                        Log("HTTPManager: JSON POST %s cancelled with data pending", endpoint.c_str());
                        break;
                    }

                    DWORD bytesToRead = (bytesAvailable < sizeof(buffer)) ? bytesAvailable : sizeof(buffer);
                    if (WinHttpReadData(hRequest, buffer, bytesToRead, &bytesRead)) {
                        responseBody.append(buffer, bytesRead);
                        if (streamResponse && bytesRead > 0) {
                            lineBuffer.append(buffer, bytesRead);

                            size_t newlinePos = std::string::npos;
                            while ((newlinePos = lineBuffer.find('\n')) != std::string::npos) {
                                std::string line = lineBuffer.substr(0, newlinePos);
                                lineBuffer.erase(0, newlinePos + 1);
                                if (cancelRequested && cancelRequested()) {
                                    Log("HTTPManager: JSON POST %s cancelled during stream callback", endpoint.c_str());
                                    break;
                                }
                                processStreamLine(line);
                            }
                        }
                    }
                }
            } while (bytesAvailable > 0);

            if (streamResponse && !Trim(lineBuffer).empty() && !(cancelRequested && cancelRequested())) {
                processStreamLine(lineBuffer);
            }

            closeRequest();
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);

            if (streamResponse) {
                Log("HTTPManager: JSON POST %s returned %zu bytes (streamed, lines=%zu)",
                    endpoint.c_str(),
                    responseBody.size(),
                    streamedLineCount);
            } else {
                Log("HTTPManager: JSON POST %s returned %zu bytes",
                    endpoint.c_str(),
                    responseBody.size());
            }
            return responseBody;

        } catch (...) {
            Log("HTTPManager: Exception in SendJson");
            return "";
        }
    }

    std::string SendJson(const std::string& endpoint, const std::string& jsonBody) {
        if (ShouldThrottleTelemetryEndpoint(endpoint) && !IsPromptCriticalGamedataPayload(jsonBody)) {
            const uint64_t skipped = g_telemetrySkippedDuringStream.fetch_add(1) + 1;
            if (skipped == 1 || skipped % 25 == 0) {
                Log("HTTPManager: Skipped %llu non-critical gamedata telemetry request(s) during active input/response",
                    static_cast<unsigned long long>(skipped));
            }
            return "OK";
        }
        return SendJsonRequest(endpoint, jsonBody);
    }

    std::string GetServerVersionRaw() {
        const std::string response = SendJsonRequest("ui/tools/server_version.php", "{}");
        if (response.empty()) {
            return "";
        }

        return Trim(ExtractJsonStringValue(response, "serverVersion"));
    }

    void SendDialogueDeliveryAck(const std::string& speaker,
                                 uint32_t actorFormId,
                                 const std::string& text,
                                 const std::string& ttsCacheKey,
                                 const std::string& utteranceId,
                                 const std::string& state,
                                 const std::string& requestId) {
        if (!g_initialized) {
            return;
        }

        const int64_t ts = GetTimestamp();
        const long long gameTs = WorldContextFNV::GetGameTimestamp();
        std::ostringstream json;
        json << "{";
        json << "\"schema\":\"dialectic.dialogue_delivery.v1\",";
        json << "\"type\":\"dialogue_delivery\",";
        json << "\"state\":\"" << EscapeJson(state) << "\",";
        json << "\"speaker\":\"" << EscapeJson(speaker) << "\",";
        json << "\"actor_formid\":" << actorFormId << ",";
        json << "\"text\":\"" << EscapeJson(text) << "\",";
        json << "\"tts_cache_key\":\"" << EscapeJson(ttsCacheKey) << "\",";
        json << "\"utterance_id\":\"" << EscapeJson(utteranceId) << "\",";
        if (!requestId.empty()) {
            json << "\"request_id\":\"" << EscapeJson(requestId) << "\",";
        }
        json << "\"ts\":" << ts << ",";
        json << "\"gamets\":" << (gameTs > 0 ? gameTs : 0);
        json << "}";

        const std::string jsonBody = json.str();
        EnqueueHttpTask("dialogue_delivery", [jsonBody, state, speaker]() {
            const std::string response = SendJsonRequest("gamedata.php", jsonBody);
            if (response.empty()) {
                Log("HTTPManager: dialogue_delivery %s ack for '%s' returned empty response",
                    state.c_str(),
                    speaker.c_str());
            }
        });
    }

    // Send dialogue event (NPC speech) in the current server-compatible format.
    void SendDialogueEvent(const std::string& speaker, const std::string& speech, const std::string& listener) {
        // Get current player location
        std::string location = Misc::GetPlayerLocation();
        if (location.empty()) {
            location = "Unknown";
        }
        
        // Build structured dialogue capture payload:
        // {"speaker": "NPC Name", "location": "Current Location", "speech": "...", "listener": "..."}
        std::ostringstream jsonPayload;
        jsonPayload << "{"
                    << "\"speaker\":\"" << EscapeJson(speaker) << "\","
                    << "\"location\":\"" << EscapeJson(location) << "\","
                    << "\"speech\":\"" << EscapeJson(speech) << "\","
                    << "\"listener\":\"" << EscapeJson(listener) << "\""
                    << "}";
        
        std::string jsonFormatted = FormatEventJson("_speech", jsonPayload.str());
        
        // Also send as a chat event for context logging.
        std::string chatMessage = "(Context location: " + location + " background chat) " + speaker + ": " + speech;
        std::string chatJsonFormatted = FormatEventJson("chat", chatMessage);
        
        Log("HTTPManager: Sending dialogue - Speaker: %s, Speech: %s", speaker.c_str(), speech.c_str());
        
        // Send both events asynchronously
        EnqueueHttpTask("dialogue_event", [jsonFormatted, chatJsonFormatted]() {
            // Send _speech event (structured)
            SendJsonRequest("main.php", jsonFormatted);
            
            // Send chat event (for context)
            SendJsonRequest("main.php", chatJsonFormatted);
        });
    }

    // Send player input text
    void SendPlayerInput(const std::string& npcName, const std::string& message) {
        std::string payload = CurrentPlayerName() + ": " + message;
        SendEvent(ActorPositionResolverFNV::IsPlayerSneaking() ? "inputtext_s" : "inputtext", payload);
    }

    static bool SendPlayerTtsPlayForGeneration(const std::string& message, uint64_t generation) {
        std::ostringstream payload;
        payload << "{"
                << "\"schema\":\"dialectic.player_tts.v1\","
                << "\"player\":\"" << EscapeJson(CurrentPlayerName()) << "\","
                << "\"text\":\"" << EscapeJson(message) << "\","
                << "\"game\":\"fnv\""
                << "}";
        std::string jsonBody = FormatEventJson("player_menu_tts_play", payload.str());
        ResponseQueueFNV::SetActiveGeneration(generation, "player_tts");
        ExtendHotInputWindow(8000, "player tts");

        Log("HTTPManager: Sending priority player_menu_tts_play: %s",
            jsonBody.length() > 100 ? jsonBody.substr(0, 100).c_str() : jsonBody.c_str());

        const auto start = std::chrono::steady_clock::now();
        std::string response = SendJsonRequest("processor/player_tts_play.php", jsonBody, false);
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        Log("HTTPManager: priority player_menu_tts_play returned in %lldms",
            static_cast<long long>(elapsedMs));

        if (generation != g_responseGeneration.load()) {
            Log("HTTPManager: Dropping stale player_menu_tts_play response");
            return true;
        }

        if (response.empty()) {
            Log("HTTPManager: player_menu_tts_play returned empty response");
            SpeakManager::ClearPlayerInputTtsGate("player_tts_empty_response");
            return false;
        }

        if (ProcessJsonResponsePayload(response, "HTTPManager:player-tts-priority", generation)) {
            return true;
        }

        if (ProcessFinalJsonResponseIfCurrent(response, generation, "HTTPManager:player-tts-final-json")) {
            return true;
        }

        Log("HTTPManager: player_menu_tts_play response had no queueable Player line");
        SpeakManager::ClearPlayerInputTtsGate("player_tts_no_queueable_line");
        return false;
    }

    bool SendPlayerTtsPlay(const std::string& message) {
        return SendPlayerTtsPlayForGeneration(message, g_responseGeneration.load());
    }

    bool QueuePlayerTtsPlay(const std::string& message) {
        const uint64_t generation = g_responseGeneration.load();
        ResponseQueueFNV::SetActiveGeneration(generation, "player_tts_queued");
        ExtendHotInputWindow(8000, "player tts queued");

        const uint64_t taskId = EnqueueHttpTask("PlayerTTS", [message, generation]() {
            if (generation != g_responseGeneration.load()) {
                Log("HTTPManager: Skipping stale queued player_menu_tts_play generation=%llu current=%llu",
                    static_cast<unsigned long long>(generation),
                    static_cast<unsigned long long>(g_responseGeneration.load()));
                return;
            }

            const bool queued = SendPlayerTtsPlayForGeneration(message, generation);
            Log("HTTPManager: async player_menu_tts_play queued dialogue=%d", queued ? 1 : 0);
        }, "player_tts", true);

        Log("HTTPManager: queued async player_menu_tts_play task id=%llu",
            static_cast<unsigned long long>(taskId));
        return taskId != 0;
    }

    // Functions required by GameLoop
    void SendMessage(const std::string& endpoint, const std::string& data) {
        std::string eventType = "request";

        if (LooksLikeJsonValue(data)) {
            eventType = ExtractJsonStringValue(data, "type");
            if (eventType.empty()) {
                eventType = ExtractJsonStringValue(data, "action");
            }
        }

        if (eventType.empty() || eventType == "request") {
            size_t actionPos = data.find("action=");
            if (actionPos != std::string::npos) {
            size_t endPos = data.find('&', actionPos);
            if (endPos == std::string::npos) {
                endPos = data.length();
            }
            eventType = data.substr(actionPos + 7, endPos - actionPos - 7);
            }
        }

        if (eventType.empty()) {
            eventType = "request";
        }

        SendEvent(eventType, data);
    }

    bool HasPendingResponse() {
        return ResponseQueueFNV::HasPending();
    }

    QueueStatus GetQueueStatus() {
        QueueStatus status;
        status.generation = g_responseGeneration.load();
        const ResponseQueueFNV::QueueStatus responseStatus = ResponseQueueFNV::GetStatus();
        status.responseQueueGeneration = responseStatus.activeGeneration;
        status.totalResponsesQueued = responseStatus.totalQueued;
        status.totalResponsesDispatched = responseStatus.totalDispatched;
        status.totalResponsesDroppedStale = responseStatus.totalDroppedStale;
        status.httpResponsesQueued = responseStatus.pendingItems;
        status.responseDialogueQueued = responseStatus.pendingDialogueLines;
        status.responseActionsQueued = responseStatus.pendingActionLines;
        {
            std::lock_guard<std::mutex> lock(g_streamStateMutex);
            status.streamInProgress = g_streamInProgress || responseStatus.unfinished;
            status.cancellationRequested = g_streamCancellationRequested;
            status.activeStreamTasks = g_activeStreamTasks;
            status.activeStreamName = !g_activeStreamName.empty() ? g_activeStreamName : responseStatus.unfinishedSource;
        }
        const TaskManager::Snapshot taskSnapshot = TaskManager::GetSnapshot();
        for (const auto& type : taskSnapshot.types) {
            if (type.type.rfind("http:", 0) != 0) continue;
            status.pendingHttpTasks += type.pending;
            status.activeHttpTasks += type.active;
        }
        for (const auto& worker : taskSnapshot.workers) {
            if (!worker.active || worker.type.rfind("http:", 0) != 0) continue;
            if (!status.activeHttpTaskSummary.empty()) status.activeHttpTaskSummary += ",";
            status.activeHttpTaskSummary += worker.type.substr(5);
            if (!worker.key.empty()) status.activeHttpTaskSummary += "(" + worker.key + ")";
        }
        status.totalHttpTasksQueued = g_totalTasksQueued.load();
        status.totalHttpTasksCompleted = g_totalTasksCompleted.load();
        status.totalHttpTasksCancelled = g_totalTasksCancelled.load();
        return status;
    }

    bool IsStreamInProgress() {
        return GetQueueStatus().streamInProgress;
    }

    std::string UploadVoiceSample(const std::string& audioData,
                                  const std::string& actorName,
                                  const std::string& originalName,
                                  const std::string& referenceText) {
        const TaskManager::CancellationToken* token = TaskManager::CurrentToken();
        if (token && token->IsCancellationRequested()) return "";
        if (!g_initialized) {
            Log("HTTPManager: Not initialized, cannot upload voice sample");
            return "";
        }

        if (audioData.empty()) {
            Log("HTTPManager: Empty voice sample data provided for %s", actorName.c_str());
            return "";
        }

        try {
            std::string serverPath = Config::serverPath;
            size_t pos = serverPath.find("comm.php");
            if (pos != std::string::npos) {
                serverPath.replace(pos, 8, "vsx.php");
            } else if ((pos = serverPath.find("main.php")) != std::string::npos) {
                serverPath.replace(pos, 8, "vsx.php");
            } else if ((pos = serverPath.find("gamedata.php")) != std::string::npos) {
                serverPath.replace(pos, 12, "vsx.php");
            }

            const size_t queryPos = serverPath.find('?');
            if (queryPos != std::string::npos) {
                serverPath = serverPath.substr(0, queryPos);
            }

            Log("HTTPManager: Uploading voice sample for %s from %s (%zu bytes)",
                actorName.c_str(),
                originalName.c_str(),
                audioData.size());

            std::wstring wideServer(Config::serverHost.begin(), Config::serverHost.end());
            std::wstring widePath(serverPath.begin(), serverPath.end());

            const char* boundary = "----974767299852498929531610575";
            const char* szHeaders =
                "Content-Type: multipart/form-data; boundary=----974767299852498929531610575\r\n"
                "Accept: application/json";
            std::wstring wideHeaders(szHeaders, szHeaders + strlen(szHeaders));

            std::string contentType = "application/octet-stream";
            std::string lowerOriginal = originalName;
            std::transform(lowerOriginal.begin(), lowerOriginal.end(), lowerOriginal.begin(),
                [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            if (lowerOriginal.ends_with(".ogg")) {
                contentType = "audio/ogg";
            } else if (lowerOriginal.ends_with(".wav")) {
                contentType = "audio/x-wav";
            }

            std::ostringstream metadata;
            metadata << "{\"schema\":\"dialectic.voice_sample.v1\","
                     << "\"game\":\"fnv\","
                     << "\"actor_name\":\"" << EscapeJson(actorName) << "\","
                     << "\"original_name\":\"" << EscapeJson(originalName) << "\"";
            if (!referenceText.empty()) {
                metadata << ",\"reference_text\":\"" << EscapeJson(referenceText) << "\"";
            }
            metadata << "}";
            const std::string metadataJson = metadata.str();

            std::string metadataPart =
                std::string("--") + boundary + "\r\n" +
                "Content-Disposition: form-data; name=\"metadata\"\r\n" +
                "Content-Type: application/json; charset=utf-8\r\n\r\n" +
                metadataJson + "\r\n";

            std::string filePart =
                std::string("--") + boundary + "\r\n" +
                "Content-Disposition: form-data; name=\"file\"; filename=\"voiceSample.dat\"\r\n" +
                "Content-Type: " + contentType + "\r\n\r\n";
            std::string endPart = std::string("\r\n--") + boundary + "--\r\n";

            HINTERNET hSession = WinHttpOpen(
                L"Dialectic Voice Uploader/0.1",
                WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                WINHTTP_NO_PROXY_NAME,
                WINHTTP_NO_PROXY_BYPASS,
                0);
            if (!hSession) {
                Log("HTTPManager: WinHttpOpen failed for voice sample upload");
                return "";
            }

            HINTERNET hConnect = WinHttpConnect(hSession, wideServer.c_str(), Config::serverPort, 0);
            if (!hConnect) {
                Log("HTTPManager: WinHttpConnect failed for voice sample upload");
                WinHttpCloseHandle(hSession);
                return "";
            }

            HINTERNET hRequest = WinHttpOpenRequest(
                hConnect,
                L"POST",
                widePath.c_str(),
                NULL,
                WINHTTP_NO_REFERER,
                WINHTTP_DEFAULT_ACCEPT_TYPES,
                0);
            if (!hRequest) {
                Log("HTTPManager: WinHttpOpenRequest failed for voice sample upload");
                WinHttpCloseHandle(hConnect);
                WinHttpCloseHandle(hSession);
                return "";
            }
            auto interruptibleRequest = std::make_shared<std::atomic<HINTERNET>>(hRequest);
            if (token) {
                token->SetInterrupt([interruptibleRequest]() {
                    HINTERNET request = interruptibleRequest->exchange(nullptr);
                    if (request) WinHttpCloseHandle(request);
                });
            }
            auto closeRequest = [&]() {
                if (token) token->ClearInterrupt();
                HINTERNET request = interruptibleRequest->exchange(nullptr);
                if (request) WinHttpCloseHandle(request);
            };

            const int timeoutMs = (Config::aiResponseTimeout > 0 ? Config::aiResponseTimeout : 30) * 1000;
            WinHttpSetTimeouts(hRequest, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

            if (!WinHttpAddRequestHeaders(hRequest, wideHeaders.c_str(), -1, WINHTTP_ADDREQ_FLAG_ADD)) {
                Log("HTTPManager: WinHttpAddRequestHeaders failed for voice sample upload");
                closeRequest();
                WinHttpCloseHandle(hConnect);
                WinHttpCloseHandle(hSession);
                return "";
            }

            std::vector<char> requestBody;
            requestBody.insert(requestBody.end(), metadataPart.begin(), metadataPart.end());
            requestBody.insert(requestBody.end(), filePart.begin(), filePart.end());
            requestBody.insert(requestBody.end(), audioData.begin(), audioData.end());
            requestBody.insert(requestBody.end(), endPart.begin(), endPart.end());

            if (!WinHttpSendRequest(
                hRequest,
                WINHTTP_NO_ADDITIONAL_HEADERS,
                0,
                requestBody.data(),
                static_cast<DWORD>(requestBody.size()),
                static_cast<DWORD>(requestBody.size()),
                0)) {
                Log("HTTPManager: WinHttpSendRequest failed for voice sample upload");
                closeRequest();
                WinHttpCloseHandle(hConnect);
                WinHttpCloseHandle(hSession);
                return "";
            }

            if (!WinHttpReceiveResponse(hRequest, NULL)) {
                Log("HTTPManager: WinHttpReceiveResponse failed for voice sample upload");
                closeRequest();
                WinHttpCloseHandle(hConnect);
                WinHttpCloseHandle(hSession);
                return "";
            }

            DWORD statusCode = 0;
            DWORD statusSize = sizeof(statusCode);
            if (WinHttpQueryHeaders(
                    hRequest,
                    WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                    WINHTTP_HEADER_NAME_BY_INDEX,
                    &statusCode,
                    &statusSize,
                WINHTTP_NO_HEADER_INDEX)) {
                if (statusCode < 200 || statusCode >= 300) {
                    Log("HTTPManager: Voice sample endpoint returned HTTP %lu", statusCode);
                    closeRequest();
                    WinHttpCloseHandle(hConnect);
                    WinHttpCloseHandle(hSession);
                    return "";
                }
            }

            std::string response;
            char buffer[4096];
            DWORD bytesAvailable = 0;
            DWORD bytesRead = 0;
            do {
                bytesAvailable = 0;
                if (!WinHttpQueryDataAvailable(hRequest, &bytesAvailable)) {
                    break;
                }

                if (bytesAvailable > 0) {
                    const DWORD bytesToRead = (bytesAvailable < sizeof(buffer)) ? bytesAvailable : sizeof(buffer);
                    if (WinHttpReadData(hRequest, buffer, bytesToRead, &bytesRead)) {
                        response.append(buffer, bytesRead);
                    }
                }
            } while (bytesAvailable > 0);

            closeRequest();
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);

            response = Trim(response);

            Log("HTTPManager: Voice sample upload for %s returned %zu bytes",
                actorName.c_str(),
                response.size());
            if (!response.empty() && response.starts_with("{")) {
                if (response.find("\"ok\":true") != std::string::npos ||
                    response.find("\"ok\": true") != std::string::npos) {
                    return "OK";
                }

                const std::string error = Trim(ExtractJsonStringValue(response, "error"));
                Log("HTTPManager: Voice sample JSON response failed for %s: %s",
                    actorName.c_str(),
                    error.empty() ? response.c_str() : error.c_str());
                return "";
            }

            Log("HTTPManager: Voice sample endpoint returned non-JSON response for %s", actorName.c_str());
            return "";
        } catch (...) {
            Log("HTTPManager: Exception in UploadVoiceSample");
            return "";
        }
    }

    std::string UploadCSVFile(const std::string& csvData,
                              const std::string& filename,
                              const std::string& fileType) {
        const TaskManager::CancellationToken* token = TaskManager::CurrentToken();
        if (token && token->IsCancellationRequested()) return "";
        if (!g_initialized) {
            Log("HTTPManager: Not initialized, cannot upload CSV import file");
            return "";
        }

        if (csvData.empty()) {
            Log("HTTPManager: CSV upload failed: empty data for %s", filename.c_str());
            return "";
        }

        if (filename.empty() || !filename.ends_with(".csv")) {
            Log("HTTPManager: CSV upload failed: invalid filename '%s'", filename.c_str());
            return "";
        }

        if (fileType != "biography_import") {
            Log("HTTPManager: CSV upload failed: invalid import type '%s'", fileType.c_str());
            return "";
        }

        const size_t maxFileSize = 10 * 1024 * 1024;
        if (csvData.size() > maxFileSize) {
            Log("HTTPManager: CSV upload failed: file '%s' too large (%zu bytes)", filename.c_str(), csvData.size());
            return "";
        }

        try {
            std::string serverPath = Config::serverPath;
            size_t pos = serverPath.find("comm.php");
            if (pos != std::string::npos) {
                serverPath.replace(pos, 8, "csv_import.php");
            } else if ((pos = serverPath.find("main.php")) != std::string::npos) {
                serverPath.replace(pos, 8, "csv_import.php");
            } else if ((pos = serverPath.find("gamedata.php")) != std::string::npos) {
                serverPath.replace(pos, 12, "csv_import.php");
            }

            const size_t queryPos = serverPath.find('?');
            if (queryPos != std::string::npos) {
                serverPath = serverPath.substr(0, queryPos);
            }

            serverPath += "?type=" + UrlEncode(fileType);
            serverPath += "&filename=" + UrlEncode(filename);
            serverPath += "&ts=" + std::to_string(Misc::GetCurrentTimeMillis() / 1000);
            serverPath += "&gamets=" + UrlEncode(Misc::GetGameTimeStamp());

            Log("HTTPManager: Uploading CSV import file %s type=%s bytes=%zu endpoint=%s:%d/%s",
                filename.c_str(),
                fileType.c_str(),
                csvData.size(),
                Config::serverHost.c_str(),
                Config::serverPort,
                serverPath.c_str());

            std::wstring wideServer(Config::serverHost.begin(), Config::serverHost.end());
            std::wstring widePath(serverPath.begin(), serverPath.end());

            const char* boundary = "----974767299852498929531610575";
            const char* szHeaders =
                "Content-Type: multipart/form-data; boundary=----974767299852498929531610575\r\n"
                "Accept: application/json";
            std::wstring wideHeaders(szHeaders, szHeaders + strlen(szHeaders));

            std::string metadataJson =
                "{\"schema\":\"dialectic.csv_import.v1\",\"game\":\"fnv\",\"type\":\"" +
                EscapeJson(fileType) + "\",\"filename\":\"" + EscapeJson(filename) + "\"}";
            std::string metadataPart =
                std::string("--") + boundary + "\r\n" +
                "Content-Disposition: form-data; name=\"metadata\"\r\n" +
                "Content-Type: application/json; charset=utf-8\r\n\r\n" +
                metadataJson + "\r\n";

            std::string filePart =
                std::string("--") + boundary + "\r\n" +
                "Content-Disposition: form-data; name=\"file\"; filename=\"" + filename + "\"\r\n" +
                "Content-Type: text/csv\r\n\r\n";
            std::string endPart = std::string("\r\n--") + boundary + "--\r\n";

            HINTERNET hSession = WinHttpOpen(
                L"Dialectic CSV Uploader/0.1",
                WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                WINHTTP_NO_PROXY_NAME,
                WINHTTP_NO_PROXY_BYPASS,
                0);
            if (!hSession) {
                Log("HTTPManager: WinHttpOpen failed for CSV upload");
                return "";
            }

            HINTERNET hConnect = WinHttpConnect(hSession, wideServer.c_str(), Config::serverPort, 0);
            if (!hConnect) {
                Log("HTTPManager: WinHttpConnect failed for CSV upload");
                WinHttpCloseHandle(hSession);
                return "";
            }

            HINTERNET hRequest = WinHttpOpenRequest(
                hConnect,
                L"POST",
                widePath.c_str(),
                NULL,
                WINHTTP_NO_REFERER,
                WINHTTP_DEFAULT_ACCEPT_TYPES,
                0);
            if (!hRequest) {
                Log("HTTPManager: WinHttpOpenRequest failed for CSV upload");
                WinHttpCloseHandle(hConnect);
                WinHttpCloseHandle(hSession);
                return "";
            }
            auto interruptibleRequest = std::make_shared<std::atomic<HINTERNET>>(hRequest);
            if (token) {
                token->SetInterrupt([interruptibleRequest]() {
                    HINTERNET request = interruptibleRequest->exchange(nullptr);
                    if (request) WinHttpCloseHandle(request);
                });
            }
            auto closeRequest = [&]() {
                if (token) token->ClearInterrupt();
                HINTERNET request = interruptibleRequest->exchange(nullptr);
                if (request) WinHttpCloseHandle(request);
            };

            const int timeoutMs = (Config::aiResponseTimeout > 0 ? Config::aiResponseTimeout : 30) * 1000;
            WinHttpSetTimeouts(hRequest, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

            if (!WinHttpAddRequestHeaders(hRequest, wideHeaders.c_str(), -1, WINHTTP_ADDREQ_FLAG_ADD)) {
                Log("HTTPManager: WinHttpAddRequestHeaders failed for CSV upload");
                closeRequest();
                WinHttpCloseHandle(hConnect);
                WinHttpCloseHandle(hSession);
                return "";
            }

            std::vector<char> requestBody;
            requestBody.insert(requestBody.end(), metadataPart.begin(), metadataPart.end());
            requestBody.insert(requestBody.end(), filePart.begin(), filePart.end());
            requestBody.insert(requestBody.end(), csvData.begin(), csvData.end());
            requestBody.insert(requestBody.end(), endPart.begin(), endPart.end());

            if (!WinHttpSendRequest(
                hRequest,
                WINHTTP_NO_ADDITIONAL_HEADERS,
                0,
                requestBody.data(),
                static_cast<DWORD>(requestBody.size()),
                static_cast<DWORD>(requestBody.size()),
                0)) {
                Log("HTTPManager: WinHttpSendRequest failed for CSV upload");
                closeRequest();
                WinHttpCloseHandle(hConnect);
                WinHttpCloseHandle(hSession);
                return "";
            }

            if (!WinHttpReceiveResponse(hRequest, NULL)) {
                Log("HTTPManager: WinHttpReceiveResponse failed for CSV upload");
                closeRequest();
                WinHttpCloseHandle(hConnect);
                WinHttpCloseHandle(hSession);
                return "";
            }

            DWORD statusCode = 0;
            DWORD statusSize = sizeof(statusCode);
            if (WinHttpQueryHeaders(
                    hRequest,
                    WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                    WINHTTP_HEADER_NAME_BY_INDEX,
                    &statusCode,
                    &statusSize,
                WINHTTP_NO_HEADER_INDEX)) {
                if (statusCode < 200 || statusCode >= 300) {
                    Log("HTTPManager: CSV import endpoint returned HTTP %lu for %s", statusCode, filename.c_str());
                    closeRequest();
                    WinHttpCloseHandle(hConnect);
                    WinHttpCloseHandle(hSession);
                    return "";
                }
            }

            std::string response;
            char buffer[4096];
            DWORD bytesAvailable = 0;
            DWORD bytesRead = 0;
            do {
                bytesAvailable = 0;
                if (!WinHttpQueryDataAvailable(hRequest, &bytesAvailable)) {
                    break;
                }
                if (bytesAvailable > 0) {
                    const DWORD bytesToRead = (bytesAvailable < sizeof(buffer)) ? bytesAvailable : sizeof(buffer);
                    if (WinHttpReadData(hRequest, buffer, bytesToRead, &bytesRead)) {
                        response.append(buffer, bytesRead);
                    }
                }
            } while (bytesAvailable > 0);

            closeRequest();
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);

            response = Trim(response);
            if (!response.empty() && response.starts_with("{")) {
                if (response.find("\"success\":true") != std::string::npos ||
                    response.find("\"success\": true") != std::string::npos) {
                    return "OK";
                }

                const std::string error = Trim(ExtractJsonStringValue(response, "error"));
                Log("HTTPManager: CSV import JSON response failed for %s: %s",
                    filename.c_str(),
                    error.empty() ? response.c_str() : error.c_str());
                return "";
            }

            Log("HTTPManager: CSV import endpoint returned non-JSON response for %s", filename.c_str());
            return "";
        } catch (...) {
            Log("HTTPManager: Exception in UploadCSVFile");
            return "";
        }
    }

    // Upload audio WAV data to server for STT transcription
    std::string UploadAudioForSTT(const std::string& wavData,
                                  const TaskManager::CancellationToken* token) {
        if (!g_initialized) {
            Log("HTTPManager: Not initialized, cannot upload audio");
            return "";
        }

        if (wavData.empty()) {
            Log("HTTPManager: Empty WAV data provided");
            return "";
        }

        Log("HTTPManager: Uploading %zu bytes of audio for STT", wavData.size());
        ExtendHotInputWindow(10000, "stt upload");

        try {
            // Build server path by replacing the configured game request endpoint with stt.php.
            std::string serverPath = Config::serverPath;
            size_t pos = serverPath.find("comm.php");
            if (pos != std::string::npos) {
                serverPath.replace(pos, 8, "stt.php");
            } else if (serverPath.find("main.php") != std::string::npos) {
                pos = serverPath.find("main.php");
                serverPath.replace(pos, 8, "stt.php");
            }
            
            const size_t queryPos = serverPath.find('?');
            if (queryPos != std::string::npos) {
                serverPath = serverPath.substr(0, queryPos);
            }

            Log("HTTPManager: Using STT endpoint: %s:%d/%s", 
                Config::serverHost.c_str(), Config::serverPort, serverPath.c_str());

            std::wstring wideServer(Config::serverHost.begin(), Config::serverHost.end());
            std::wstring widePath(serverPath.begin(), serverPath.end());

            // Multipart form data headers
            const char* szHeaders =
                "Content-Type: multipart/form-data; boundary=----974767299852498929531610575\r\n"
                "Accept: application/json";
            std::wstring wideHeaders(szHeaders, szHeaders + strlen(szHeaders));

            const std::string metadataJson = "{\"schema\":\"dialectic.stt.v1\",\"game\":\"fnv\"}";
            std::string metadataPart =
                "------974767299852498929531610575\r\n"
                "Content-Disposition: form-data; name=\"metadata\"\r\n"
                "Content-Type: application/json; charset=utf-8\r\n\r\n" +
                metadataJson + "\r\n";

            // Multipart form content
            const char* szContent =
                "------974767299852498929531610575\r\n"
                "Content-Disposition: form-data; name=\"file\"; filename=\"wavData.wav\"\r\n"
                "Content-Type: audio/x-wav\r\n\r\n";
            const char* szEndData = "\r\n------974767299852498929531610575--\r\n";

            // Open WinHTTP session
            HINTERNET hSession = WinHttpOpen(L"Dialectic/0.1", 
                                            WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                            WINHTTP_NO_PROXY_NAME, 
                                            WINHTTP_NO_PROXY_BYPASS, 
                                            0);
            if (!hSession) {
                Log("HTTPManager: WinHttpOpen failed");
                return "";
            }

            // Connect to server
            HINTERNET hConnect = WinHttpConnect(hSession, 
                                               wideServer.c_str(),
                                               Config::serverPort, 
                                               0);
            if (!hConnect) {
                Log("HTTPManager: WinHttpConnect failed");
                WinHttpCloseHandle(hSession);
                return "";
            }

            // Open request
            HINTERNET hRequest = WinHttpOpenRequest(hConnect, 
                                                   L"POST",
                                                   widePath.c_str(), 
                                                   NULL,
                                                   WINHTTP_NO_REFERER,
                                                   WINHTTP_DEFAULT_ACCEPT_TYPES, 
                                                   0);
            if (!hRequest) {
                Log("HTTPManager: WinHttpOpenRequest failed");
                WinHttpCloseHandle(hConnect);
                WinHttpCloseHandle(hSession);
                return "";
            }
            auto interruptibleRequest = std::make_shared<std::atomic<HINTERNET>>(hRequest);
            if (token) {
                token->SetInterrupt([interruptibleRequest]() {
                    HINTERNET request = interruptibleRequest->exchange(nullptr);
                    if (request) WinHttpCloseHandle(request);
                });
            }
            auto closeRequest = [&]() {
                if (token) token->ClearInterrupt();
                HINTERNET request = interruptibleRequest->exchange(nullptr);
                if (request) WinHttpCloseHandle(request);
            };

            const int timeoutMs = (Config::aiResponseTimeout > 0 ? Config::aiResponseTimeout : 30) * 1000;
            WinHttpSetTimeouts(hRequest, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

            // Add headers
            if (!WinHttpAddRequestHeaders(hRequest, wideHeaders.c_str(), -1, WINHTTP_ADDREQ_FLAG_ADD)) {
                Log("HTTPManager: WinHttpAddRequestHeaders failed");
                closeRequest();
                WinHttpCloseHandle(hConnect);
                WinHttpCloseHandle(hSession);
                return "";
            }

            // Build request body
            std::vector<char> requestBody;
            requestBody.insert(requestBody.end(), metadataPart.begin(), metadataPart.end());
            requestBody.insert(requestBody.end(), szContent, szContent + strlen(szContent));
            requestBody.insert(requestBody.end(), wavData.begin(), wavData.end());
            requestBody.insert(requestBody.end(), szEndData, szEndData + strlen(szEndData));

            Log("HTTPManager: Sending request with %zu bytes", requestBody.size());

            // Send request
            if (!WinHttpSendRequest(hRequest, 
                                   WINHTTP_NO_ADDITIONAL_HEADERS, 
                                   0,
                                   requestBody.data(), 
                                   static_cast<DWORD>(requestBody.size()),
                                   static_cast<DWORD>(requestBody.size()), 
                                   0)) {
                Log("HTTPManager: WinHttpSendRequest failed");
                closeRequest();
                WinHttpCloseHandle(hConnect);
                WinHttpCloseHandle(hSession);
                return "";
            }

            // Wait for response
            if (!WinHttpReceiveResponse(hRequest, NULL)) {
                Log("HTTPManager: WinHttpReceiveResponse failed");
                closeRequest();
                WinHttpCloseHandle(hConnect);
                WinHttpCloseHandle(hSession);
                return "";
            }

            DWORD statusCode = 0;
            DWORD statusSize = sizeof(statusCode);
            bool sttHttpSuccess = true;
            if (WinHttpQueryHeaders(hRequest,
                                    WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX,
                                    &statusCode,
                                    &statusSize,
                                    WINHTTP_NO_HEADER_INDEX)) {
                if (statusCode < 200 || statusCode >= 300) {
                    Log("HTTPManager: STT endpoint returned HTTP %lu", statusCode);
                    sttHttpSuccess = false;
                }
            } else {
                Log("HTTPManager: Could not query STT HTTP status code");
            }

            // Read response
            const int bufferSize = 4096;
            char buffer[bufferSize];
            DWORD bytesRead = 0;
            std::string response;

            do {
                if (token && token->IsCancellationRequested()) {
                    response.clear();
                    break;
                }
                if (!WinHttpQueryDataAvailable(hRequest, &bytesRead)) {
                    Log("HTTPManager: WinHttpQueryDataAvailable failed");
                    break;
                }
                
                if (bytesRead > 0) {
                    DWORD bytesToRead = (bytesRead < bufferSize) ? bytesRead : bufferSize;
                    if (WinHttpReadData(hRequest, buffer, bytesToRead, &bytesRead)) {
                        response.append(buffer, bytesRead);
                    }
                }
            } while (bytesRead > 0);

            Log("HTTPManager: Received STT response (%zu bytes): %s", 
                response.size(), 
                response.length() > 100 ? response.substr(0, 100).c_str() : response.c_str());

            response = Trim(response);

            // Check if response is HTML error (starts with <)
            if (response.starts_with("<")) {
                Log("HTTPManager: STT service returned HTML error page");
                response = "";
            }

            // Clean up
            closeRequest();
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);

            if (response.starts_with("{")) {
                const std::string sttText = Trim(ExtractJsonStringValue(response, "text"));
                const std::string error = Trim(ExtractJsonStringValue(response, "error"));
                if (sttText.empty() && response.find("\"ok\":false") != std::string::npos) {
                    Log("HTTPManager: STT JSON response failed: %s", error.c_str());
                }
                if (!sttHttpSuccess) {
                    Log("HTTPManager: STT request failed HTTP %lu error=%s",
                        statusCode, error.empty() ? "unspecified" : error.c_str());
                    return "";
                }
                return sttText;
            }

            if (!response.empty()) {
                Log("HTTPManager: STT service returned non-JSON response");
            }
            if (!sttHttpSuccess) {
                Log("HTTPManager: STT request failed HTTP %lu without a JSON error body", statusCode);
            }
            return "";

        } catch (...) {
            Log("HTTPManager: Exception in UploadAudioForSTT");
            return "";
        }
    }
}
