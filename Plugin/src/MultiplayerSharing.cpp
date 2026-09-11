#include "MultiplayerSharing.h"
#include "AudioManager.h"
#include "ActionManager.h"
#include "Config.h"
#include "GameLoop.h"
#include "GameThreadDispatcher.h"
#include "HTTPManager.h"
#include "IngameNotifier.h"
#include "RuntimeGeneration.h"
#include "SpeakManager.h"
#include "TaskManager.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <random>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace MultiplayerSharing {
namespace {
using Clock = std::chrono::steady_clock;
struct Settings {
    int mode = 0;
    std::string url, session, key;
    bool operator==(const Settings&) const = default;
};
struct Line {
    std::string speaker, text, utterance;
    std::vector<uint8_t> audio;
    Clock::time_point received = Clock::now();
};
struct Exchange {
    std::atomic_bool done{false};
    bool ok = false, reset = false, alive = false;
    long long cursor = -1;
    std::string epoch, control;
    Line line;
};
struct Command { std::string op, fields; };
Settings g_settings;
std::shared_ptr<Exchange> g_exchange;
TaskManager::TaskHandle g_task;
std::deque<Command> g_commands;
std::deque<Line> g_lines;
std::string g_epoch, g_owner;
long long g_cursor = -1;
uint64_t g_serial = 0, g_runtime = 0;
bool g_connected = false, g_running = false, g_remotePaused = false, g_waitingForHost = false;
int g_failures = 0;
Clock::time_point g_next{}, g_lastContact{};

std::string Quote(const std::string& value) { return "\"" + HTTPManager::EscapeJson(value) + "\""; }

// Reject credential-bearing URLs and redirects; HTTPS works through a narrowly exposed relay.
std::vector<uint8_t> Request(const Settings& settings, const std::string& body,
                            size_t limit, const TaskManager::CancellationToken& token) {
    std::vector<uint8_t> bytes;
    if (token.IsCancellationRequested() || settings.url.size() > 2048 || settings.key.size() < 32
        || settings.key.size() > 256 || settings.key.find_first_of("\r\n") != std::string::npos) return bytes;
    const std::wstring url(settings.url.begin(), settings.url.end());
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength =
        parts.dwUserNameLength = parts.dwPasswordLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || parts.dwUserNameLength || parts.dwPasswordLength
        || parts.dwExtraInfoLength || (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS)) return bytes;
    std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.nScheme == INTERNET_SCHEME_HTTP) {
        unsigned a = 0, b = 0, c = 0, d = 0;
        wchar_t extra = 0;
        const bool ipv4 = swscanf_s(host.c_str(), L"%u.%u.%u.%u%c", &a, &b, &c, &d, &extra, 1) == 4
            && a <= 255 && b <= 255 && c <= 255 && d <= 255;
        const bool local = host == L"localhost" || host == L"[::1]" || (ipv4 &&
            (a == 127 || a == 10 || (a == 192 && b == 168) || (a == 172 && b >= 16 && b <= 31)));
        if (!local) return bytes; // Internet endpoints must protect the sharing key with HTTPS.
    }
    HINTERNET session = WinHttpOpen(L"DialecticSharing/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return bytes;
    WinHttpSetTimeouts(session, 2000, 2000, 3000, 3000);
    HINTERNET connection = WinHttpConnect(session, host.c_str(), parts.nPort, 0);
    HINTERNET request = connection ? WinHttpOpenRequest(connection, L"POST", path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : nullptr;
    if (request) {
        auto handle = std::make_shared<std::atomic<HINTERNET>>(request);
        token.SetInterrupt([handle]() { if (auto h = handle->exchange(nullptr)) WinHttpCloseHandle(h); });
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
        const std::wstring key(settings.key.begin(), settings.key.end());
        const std::wstring headers = L"Content-Type: application/json\r\nAuthorization: Bearer " + key + L"\r\n";
        if (!token.IsCancellationRequested() && WinHttpSendRequest(request, headers.c_str(), -1,
            const_cast<char*>(body.data()), static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0)
            && WinHttpReceiveResponse(request, nullptr)) {
            DWORD status = 0, size = sizeof(status);
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
            if (status == 200) {
                uint8_t buffer[8192];
                DWORD count = 0;
                while (!token.IsCancellationRequested()) {
                    if (!WinHttpReadData(request, buffer, sizeof(buffer), &count)) { bytes.clear(); break; }
                    if (!count) break;
                    if (bytes.size() + count > limit) { bytes.clear(); break; }
                    bytes.insert(bytes.end(), buffer, buffer + count);
                }
            }
        }
        token.ClearInterrupt();
        if (auto h = handle->exchange(nullptr)) WinHttpCloseHandle(h);
    }
    if (connection) WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    if (token.IsCancellationRequested()) bytes.clear();
    return bytes;
}

// The relay sends tab-separated ASCII metadata and percent-encoded UTF-8 display fields.
std::vector<std::string> Fields(const std::string& row) {
    std::vector<std::string> fields;
    size_t begin = 0;
    for (;;) {
        const size_t end = row.find('\t', begin);
        fields.push_back(row.substr(begin, end == std::string::npos ? end : end - begin));
        if (end == std::string::npos) return fields;
        begin = end + 1;
    }
}
std::string Decode(const std::string& value) {
    std::string decoded;
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%') {
            if (i + 2 >= value.size()) throw std::runtime_error("Invalid sharing field");
            const auto hex = value.substr(i + 1, 2);
            if (hex.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) throw std::runtime_error("Invalid sharing field");
            decoded += static_cast<char>(std::stoi(hex, nullptr, 16));
            i += 2;
        } else decoded += value[i];
    }
    if (decoded.find('\0') != std::string::npos) throw std::runtime_error("Invalid sharing field");
    return decoded;
}

long long Sequence(const std::string& value) {
    if (value.empty() || value.size() > 19 || value.find_first_not_of("0123456789") != std::string::npos)
        throw std::runtime_error("Invalid sharing sequence");
    return std::stoll(value);
}

void SetConnected(bool connected) {
    if (connected == g_connected) return;
    g_connected = connected;
    IngameNotifier::Notify(connected ? "Dialogue sharing connected" : "Dialogue sharing disconnected",
        connected ? IngameNotifier::Level::Success : IngameNotifier::Level::Warning);
}
}

bool IsListener() { return Config::multiplayerMode.load() == 2; }

void Reset() {
    g_task.Cancel();
    g_exchange.reset();
    g_commands.clear();
    g_lines.clear();
    g_cursor = -1;
    g_epoch.clear();
    g_remotePaused = false;
    g_connected = false;
    if (IsListener()) SpeakManager::StopSharedDialogue();
    if (g_settings.mode == 1 && g_running) g_commands.push_back({"reset", ""});
    g_next = {};
}

void Shutdown() {
    // Process shutdown cannot wait for network I/O. The server lease expires after 15 seconds.
    g_running = false;
    Reset();
}

void Control(const char* operation) {
    if (g_settings.mode != 1 || !g_running) return;
    if (std::string(operation) == "cancel") {
        g_commands.clear();
        g_commands.push_back({g_connected ? "cancel" : "reset", ""});
    } else if (g_commands.size() < 32) g_commands.push_back({operation, ""});
    g_next = {};
}

void Publish(const std::string& speaker, const std::string& text,
             const std::string& cacheKey, const std::string& utteranceId) {
    if (g_settings.mode != 1 || !g_running || !g_connected || g_commands.size() >= 32) return;
    if (speaker.empty() || speaker.size() > 160 || text.empty() || text.size() > 4096
        || cacheKey.size() != 32 || cacheKey.find_first_not_of("0123456789abcdef") != std::string::npos) return;
    // Legacy lines without an ID get a host-local ID; serial ordering still prevents retry duplicates.
    const std::string id = utteranceId.empty() ? g_owner + "_" + std::to_string(g_serial + g_commands.size() + 1) : utteranceId;
    g_commands.push_back({"publish", ",\"speaker\":" + Quote(speaker) + ",\"text\":" + Quote(text)
        + ",\"cache\":" + Quote(cacheKey) + ",\"utterance\":" + Quote(id)});
    g_next = {};
}

void Update() {
    if (Config::multiplayerMode.load() == 0 && g_settings.mode == 0) return;
    const Settings settings{Config::multiplayerMode.load(), Config::multiplayerUrl,
        Config::multiplayerSession, Config::multiplayerKey};
    const auto& game = GameLoop::GetGameState();
    const bool running = settings.mode != 0 && game.isInGame && !game.isLoading;
    const auto now = Clock::now();
    if (!(settings == g_settings) || running != g_running || g_runtime != RuntimeGeneration::Current()) {
        const bool changedMode = settings.mode != g_settings.mode;
        if (g_settings.mode == 1 && g_running && (!running || !(settings == g_settings))) {
            const Settings previous = g_settings;
            const std::string body = "{\"op\":\"close\",\"session\":" + Quote(previous.session)
                + ",\"owner\":" + Quote(g_owner) + ",\"serial\":" + std::to_string(++g_serial) + "}";
            TaskManager::Options close;
            close.type = "multiplayer_close";
            close.lane = TaskManager::Lane::Background;
            close.timeout = std::chrono::seconds(5);
            TaskManager::Submit(close, [previous, body](const TaskManager::CancellationToken& token) {
                Request(previous, body, 128, token);
            });
        }
        if (changedMode) {
            const auto generation = RuntimeGeneration::Advance("sharing_mode_changed");
            TaskManager::CancelOlderThanGeneration(generation);
            GameThreadDispatcher::CancelAll("sharing_mode_changed");
            ActionManager::CancelNativeRuntimeActions("sharing_mode_changed");
        }
        g_settings = settings;
        g_running = running;
        g_runtime = RuntimeGeneration::Current();
        Reset();
        if (changedMode) {
            HTTPManager::CancelPendingResponses();
            GameLoop::StopConversation();
            SpeakManager::ClearAllSpeech("sharing_mode_changed");
            SpeakManager::StopSharedDialogue();
        }
        g_connected = false;
        g_failures = 0;
        g_waitingForHost = false;
        if (g_owner.empty()) {
            std::random_device random;
            constexpr char hex[] = "0123456789abcdef";
            for (int i = 0; i < 32; ++i) g_owner += hex[random() & 15];
        }
    }
    if (!running) return;
    if (settings.url.empty() || settings.session.empty() || settings.key.size() < 32) {
        if (!g_failures++) IngameNotifier::Notify("Set dialogue sharing URL, session and key in dialectic_custom.ini",
            IngameNotifier::Level::Warning);
        return;
    }
    if (g_exchange && g_exchange->done.load()) {
        auto result = std::move(g_exchange);
        if (result->ok) {
            g_lastContact = now;
            g_failures = 0;
            if (settings.mode == 2 && !result->alive && !g_connected && !g_waitingForHost)
                IngameNotifier::Notify("Waiting for the dialogue host");
            g_waitingForHost = settings.mode == 2 && !result->alive;
            SetConnected(settings.mode == 1 || result->alive);
            if (settings.mode == 2) {
                if (result->reset || !result->alive || result->control == "cancel") {
                    g_lines.clear();
                    SpeakManager::StopSharedDialogue();
                    g_remotePaused = false;
                }
                if (result->control == "pause") g_remotePaused = true;
                if (result->control == "resume") g_remotePaused = false;
                g_cursor = result->cursor;
                g_epoch = result->epoch;
                if (!result->line.audio.empty() && g_lines.size() < 2) g_lines.push_back(std::move(result->line));
            }
            int delayMs = 500;
            if (settings.mode == 1) delayMs = g_commands.empty() ? 3000 : 0;
            g_next = now + std::chrono::milliseconds(delayMs);
        } else {
            if (!g_failures) IngameNotifier::Notify("Dialogue sharing unavailable; check the server and sharing settings",
                IngameNotifier::Level::Warning);
            SetConnected(false);
            g_lines.clear();
            g_cursor = -1;
            if (IsListener()) SpeakManager::StopSharedDialogue();
            g_commands.clear();
            if (settings.mode == 1) g_commands.push_back({"reset", ""});
            g_next = now + std::chrono::seconds(std::min(15, 1 << std::min(++g_failures, 3)));
        }
    }
    if (settings.mode == 2) {
        SpeakManager::UpdateSharedDialogue(g_remotePaused);
        while (!g_lines.empty() && now - g_lines.front().received > std::chrono::seconds(30)) g_lines.pop_front();
        if (!game.isPaused && !game.isInMenu && !g_remotePaused && !AudioManager::IsPlaying() && !g_lines.empty()) {
            auto line = std::move(g_lines.front());
            g_lines.pop_front();
            SpeakManager::PlaySharedDialogue(line.speaker, line.text, line.utterance, line.audio);
        }
        if (g_connected && now - g_lastContact > std::chrono::seconds(15)) {
            SetConnected(false);
            Reset();
        }
    }
    if (g_exchange || now < g_next) return;
    Command command{settings.mode == 1 ? "heartbeat" : "poll", ""};
    if (settings.mode == 1 && !g_commands.empty()) { command = std::move(g_commands.front()); g_commands.pop_front(); }
    // Never run concurrent relay requests: preserves host publication/cancellation order.
    const std::string prefix = "{\"session\":" + Quote(settings.session);
    std::string body = prefix + ",\"op\":" + Quote(command.op);
    if (settings.mode == 1) body += ",\"owner\":" + Quote(g_owner) + ",\"serial\":" + std::to_string(++g_serial) + command.fields;
    else body += ",\"cursor\":" + std::to_string(g_cursor) + ",\"epoch\":" + Quote(g_epoch);
    body += "}";
    auto result = std::make_shared<Exchange>();
    g_exchange = result;
    TaskManager::Options options;
    options.type = "multiplayer";
    options.lane = TaskManager::Lane::Background;
    options.generation = RuntimeGeneration::Current();
    options.timeout = std::chrono::seconds(10);
    g_task = TaskManager::Submit(options, [settings, body, prefix, result](const TaskManager::CancellationToken& token) {
        const auto bytes = Request(settings, body, 65536, token);
        const std::string reply(bytes.begin(), bytes.end());
        if (settings.mode == 1) { result->ok = reply == "ok"; return; }
        std::istringstream stream(reply);
        std::string row;
        std::getline(stream, row);
        const auto header = Fields(row);
        if (header.size() != 5 || header[0] != "dialectic.share.v1"
            || (!header[1].empty() && (header[1].size() != 32 || header[1].find_first_not_of("0123456789abcdef") != std::string::npos))
            || (header[3] != "0" && header[3] != "1") || (header[4] != "0" && header[4] != "1")) return;
        result->epoch = header[1];
        result->cursor = Sequence(header[2]);
        result->alive = header[3] == "1";
        result->reset = header[4] == "1";
        if (std::getline(stream, row) && !row.empty()) {
            const auto fields = Fields(row);
            if (fields.size() != 6 || result->reset || !result->alive || Sequence(fields[1]) != result->cursor) return;
            if (fields[0] == "cancel" || fields[0] == "pause" || fields[0] == "resume") result->control = fields[0];
            else if (fields[0] != "say") return;
            else {
                const long long sequence = Sequence(fields[1]);
                result->line.utterance = Decode(fields[2]);
                result->line.speaker = Decode(fields[3]);
                result->line.text = Decode(fields[4]);
                if (result->line.speaker.size() > 160 || result->line.text.size() > 4096) return;
                const auto audioBody = prefix + ",\"op\":\"audio\",\"epoch\":" + Quote(result->epoch)
                    + ",\"sequence\":" + std::to_string(sequence) + "}";
                result->line.audio = Request(settings, audioBody, 16777216, token);
                result->line.received = Clock::now();
            }
        }
        result->ok = !token.IsCancellationRequested();
    }, [result](bool success, const char*) {
        if (!success) result->ok = false;
        result->done.store(true);
    });
    if (!g_task) { result->ok = false; result->done.store(true); }
}
}
