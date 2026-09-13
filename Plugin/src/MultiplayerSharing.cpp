#include "Interaction.h"
#include "MultiplayerSharing.h"
#include "AudioManager.h"
#include "ActionManager.h"
#include "Config.h"
#include "GameLoop.h"
#include "GameThreadDispatcher.h"
#include "HTTPManager.h"
#include "IngameNotifier.h"
#include "Logger.h"
#include "RuntimeGeneration.h"
#include "SpeakManager.h"
#include "TaskManager.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <random>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <utility>

namespace MultiplayerSharing {
namespace {
using Clock = std::chrono::steady_clock;
struct Settings {
    int mode = 0;
    std::string url, session, key;
    bool hosted = false;
    bool operator==(const Settings&) const = default;
};
struct Line {
    uint64_t interactionEpoch = Interaction::Epoch();
    std::string speaker, text, utterance;
    std::vector<uint8_t> audio;
    Clock::time_point received = Clock::now();
};
struct RequestDiagnostics {
    unsigned long status = 0, error = 0;
    const char* stage = "not_started";
};
struct Exchange {
    uint64_t interactionEpoch = Interaction::Epoch();
    std::atomic_bool done{false};
    bool ok = false, reset = false, alive = false;
    long long cursor = -1;
    std::string epoch, control, operation, utterance;
    RequestDiagnostics diagnostic;
    long long elapsedMs = 0;
    Line line;
};
struct Command { std::string op, fields; std::vector<uint8_t> audio; std::string utterance; };
struct SetupResult {
    std::atomic_bool done{false};
    Settings settings;
    std::string code;
    unsigned long status = 0;
    RequestDiagnostics diagnostic;
};
std::atomic<int> g_publicMode{0};
Settings g_publicSettings;
std::string g_joinCode, g_pendingCode;
int g_action = 0;
std::shared_ptr<SetupResult> g_setup;
TaskManager::TaskHandle g_setupTask;
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

// Stable truncated SHA-256 lets paired logs correlate events without recording raw identifiers.
std::string DiagnosticId(const std::string& value) {
    if (value.empty()) return "-";
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    unsigned char digest[32]{};
    bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0;
    if (ok) ok = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0;
    if (ok) ok = BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(value.data())), static_cast<ULONG>(value.size()), 0) == 0;
    if (ok) ok = BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0;
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok) return "unavailable";
    std::string id;
    for (int i = 0; i < 8; ++i) { id += "0123456789abcdef"[digest[i] >> 4]; id += "0123456789abcdef"[digest[i] & 15]; }
    return id;
}

std::string Quote(const std::string& value) { return "\"" + HTTPManager::EscapeJson(value) + "\""; }

// Reject credential-bearing URLs and redirects; HTTPS works through a narrowly exposed relay.
std::vector<uint8_t> Request(const Settings& settings, const std::string& body,
                            size_t limit, const TaskManager::CancellationToken& token,
                            const std::vector<uint8_t>& audio = {}, unsigned long* responseStatus = nullptr, RequestDiagnostics* diagnostics = nullptr) {
    RequestDiagnostics local;
    auto& diagnostic = diagnostics ? *diagnostics : local;
    diagnostic = {};
    diagnostic.stage = "invalid_settings";
    std::vector<uint8_t> bytes;
    if (token.IsCancellationRequested() || settings.url.size() > 2048 || settings.key.size() < 32
        || settings.key.size() > 256 || settings.key.find_first_of("\r\n") != std::string::npos) return bytes;
    const std::wstring url(settings.url.begin(), settings.url.end());
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength =
        parts.dwUserNameLength = parts.dwPasswordLength = static_cast<DWORD>(-1);
    diagnostic.stage = "invalid_url";
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
        if (!local) { diagnostic.stage = "https_required"; return bytes; } // Internet endpoints must protect the sharing key with HTTPS.
    }
    HINTERNET session = WinHttpOpen(L"DialecticSharing/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { diagnostic.stage = "open_session"; diagnostic.error = GetLastError(); return bytes; }
    WinHttpSetTimeouts(session, 2000, 2000, 3000, 3000);
    HINTERNET connection = WinHttpConnect(session, host.c_str(), parts.nPort, 0);
    HINTERNET request = connection ? WinHttpOpenRequest(connection, L"POST", path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : nullptr;
    if (!request) { diagnostic.stage = "open_request"; diagnostic.error = GetLastError(); }
    if (request) {
        auto handle = std::make_shared<std::atomic<HINTERNET>>(request);
        token.SetInterrupt([handle]() { if (auto h = handle->exchange(nullptr)) WinHttpCloseHandle(h); });
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
        const std::wstring key(settings.key.begin(), settings.key.end());
        const std::wstring headers = (audio.empty() ? L"Content-Type: application/json\r\nAuthorization: Bearer "
            : L"Content-Type: application/octet-stream\r\nAuthorization: Bearer ") + key + L"\r\n";
        std::string payload = body;
        if (!audio.empty()) {
            const uint32_t length = static_cast<uint32_t>(body.size());
            payload.assign(reinterpret_cast<const char*>(&length), sizeof(length));
            payload += body;
            payload.append(reinterpret_cast<const char*>(audio.data()), audio.size());
        }
        if (!token.IsCancellationRequested() && WinHttpSendRequest(request, headers.c_str(), -1,
            payload.data(), static_cast<DWORD>(payload.size()), static_cast<DWORD>(payload.size()), 0)
            && WinHttpReceiveResponse(request, nullptr)) {
            DWORD status = 0, size = sizeof(status);
            if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX)) diagnostic.error = GetLastError();
            diagnostic.status = status;
            diagnostic.stage = status == 200 ? "ok" : "http_status";
            if (responseStatus) *responseStatus = status;
            if (status == 200) {
                uint8_t buffer[8192];
                DWORD count = 0;
                while (!token.IsCancellationRequested()) {
                    if (!WinHttpReadData(request, buffer, sizeof(buffer), &count)) { diagnostic.stage = "read"; diagnostic.error = GetLastError(); bytes.clear(); break; }
                    if (!count) break;
                    if (bytes.size() + count > limit) { diagnostic.stage = "response_limit"; bytes.clear(); break; }
                    bytes.insert(bytes.end(), buffer, buffer + count);
                }
            }
        }
        else { diagnostic.stage = "send_receive"; diagnostic.error = GetLastError(); }
        token.ClearInterrupt();
        if (auto h = handle->exchange(nullptr)) WinHttpCloseHandle(h);
    }
    if (connection) WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    if (token.IsCancellationRequested()) { diagnostic.stage = "cancelled"; bytes.clear(); }
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

bool IsListener() { return g_publicMode.load() ? g_publicMode.load() == 2 : Config::multiplayerMode.load() == 2; }
bool IsHost() { return g_publicMode.load() ? g_publicMode.load() == 1 : Config::multiplayerMode.load() == 1; }

void LogPlayback(const std::string& utterance, const char* stage, size_t bytes) {
    Logger::LogInfo("Sharing: role=%s session=%s line=%s stage=%s bytes=%zu",
        IsListener() ? "listener" : "host", DiagnosticId(g_settings.session).c_str(),
        DiagnosticId(utterance).c_str(), stage, bytes);
}

void SetupAction(int action) {
    if (action < 1 || action > 6) return;
    if (action == 2) {
        g_pendingCode.clear();
        if (OpenClipboard(nullptr)) {
            if (HANDLE handle = GetClipboardData(CF_UNICODETEXT)) {
                const SIZE_T bytes = GlobalSize(handle);
                if (bytes >= 2 && bytes <= 128) {
                    if (auto text = static_cast<const wchar_t*>(GlobalLock(handle))) {
                        for (size_t i = 0; i < bytes / sizeof(wchar_t) && text[i]; ++i) {
                            wchar_t c = text[i];
                            if (c == L'-' || c == L' ' || c == L'\r' || c == L'\n') continue;
                            if (c >= L'a' && c <= L'f') c -= L'a' - L'A';
                            g_pendingCode += c <= 127 ? static_cast<char>(c) : '?';
                        }
                        GlobalUnlock(handle);
                    }
                }
            }
            CloseClipboard();
        }
    }
    g_action = action;
}

// Copies only the short listener invitation; never place a host credential on the clipboard.
static bool CopyJoinCode() {
    if (g_joinCode.empty() || !OpenClipboard(nullptr)) return false;
    bool copied = false;
    const std::wstring text(g_joinCode.begin(), g_joinCode.end());
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
    if (memory) {
        if (void* data = GlobalLock(memory)) {
            std::memcpy(data, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(memory);
            if (EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory)) copied = true;
        }
        if (!copied) GlobalFree(memory);
    }
    CloseClipboard();
    return copied;
}

// Resolve redirected/OneDrive Desktops through Windows and never replace an existing file.
static std::string SaveInviteToDesktop() {
    if (g_joinCode.size() != 12 || g_joinCode.find_first_not_of("0123456789ABCDEF") != std::string::npos) return {};
    PWSTR desktop = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop))) return {};
    const std::wstring directory(desktop);
    CoTaskMemFree(desktop);
    const std::string content = "DIALECTIC multiplayer dialogue invite\r\n\r\nJoin code: " + g_joinCode
        + "\r\n\r\n1. Copy only the 12-character join code above.\r\n"
          "2. In Fallout, open MCM > Tools > (Beta) Multiplayer Dialogue Sharing.\r\n"
          "3. Select Join session (copied code), then close MCM.\r\n\r\n"
          "Both players need the public-relay Dialectic build using the same relay.\r\n"
          "This code grants listening access. Share it only with your intended players.\r\n"
          "The invite expires when the host ends the session or the relay expires it.\r\n";
    for (int suffix = 0; suffix < 100; ++suffix) {
        const std::string name = "Dialectic Invite" + (suffix ? " (" + std::to_string(suffix + 1) + ")" : "") + ".txt";
        const std::wstring path = directory + L"\\" + std::wstring(name.begin(), name.end());
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_FILE_EXISTS || GetLastError() == ERROR_ALREADY_EXISTS) continue;
            return {};
        }
        DWORD written = 0;
        const bool saved = WriteFile(file, content.data(), static_cast<DWORD>(content.size()), &written, nullptr)
            && written == content.size() && FlushFileBuffers(file);
        CloseHandle(file);
        if (saved) return name;
        DeleteFileW(path.c_str()); // Only remove the incomplete file this action just created.
        return {};
    }
    return {};
}

// Setup runs only after an explicit Tools action. Off without pending setup has no work.
static void UpdateSetup() {
    if (g_setup && g_setup->done.load()) {
        auto result = std::move(g_setup);
        if (result->code.empty()) {
            Logger::LogWarning("Sharing: setup_failed role=%d stage=%s http=%lu winhttp=%lu",
                result->settings.mode, result->diagnostic.stage, result->diagnostic.status, result->diagnostic.error);
            std::string message = "Could not reach the public relay. Check your connection and try again.";
            if (result->status == 403) message = "Join code is invalid or expired. Ask the host for a new code.";
            if (result->status == 429) message = "Too many connection attempts. Wait a minute and try again.";
            if (result->status == 503) message = "Public relay is busy or unavailable. Try again later.";
            IngameNotifier::Notify(message, IngameNotifier::Level::Warning);
        } else {
            g_publicSettings = result->settings;
            g_publicMode.store(result->settings.mode);
            g_joinCode = result->code;
            Logger::LogInfo("Sharing: setup_ready role=%d session=%s", result->settings.mode, DiagnosticId(result->settings.session).c_str());
            if (result->settings.mode == 1) {
                IngameNotifier::Notify("Session created. Use Copy join code to invite friends.");
            } else IngameNotifier::Notify("Joined session. Waiting for shared speech.");
        }
    }
    if (!g_action || GameLoop::GetGameState().isInMenu) return;
    const int action = std::exchange(g_action, 0);
    if (action == 6) {
        if (g_publicMode.load() != 1 || !g_connected) {
            IngameNotifier::Notify("Host a connected session before saving an invite");
            return;
        }
        const std::string name = SaveInviteToDesktop();
        IngameNotifier::Notify(name.empty() ? "Could not save the invite to Desktop. Check folder access and try again."
            : "Saved to Desktop: " + name, name.empty() ? IngameNotifier::Level::Warning : IngameNotifier::Level::Success);
        return;
    }
    if (action == 3) {
        IngameNotifier::Notify(CopyJoinCode() ? "Join code copied" : "No join code to copy, or clipboard unavailable");
        return;
    }
    if (action == 4) {
        IngameNotifier::Notify(g_setup ? "Sharing: connecting" : (g_connected
            ? (IsHost() ? "Sharing: hosting" : "Sharing: listening")
            : (g_publicMode.load() || Config::multiplayerMode.load() ? "Sharing: disconnected or waiting" : "Sharing: Off")));
        return;
    }
    if (action == 5) {
        g_setupTask.Cancel(); g_setup.reset();
        if (g_publicMode.load() == 1) {
            const Settings previous = g_publicSettings;
            TaskManager::Options options;
            options.type = "multiplayer_end"; options.lane = TaskManager::Lane::Background;
            options.timeout = std::chrono::seconds(5);
            TaskManager::Submit(options, [previous](const TaskManager::CancellationToken& token) {
                Request(previous, "{\"op\":\"end\",\"session\":" + Quote(previous.session) + "}", 128, token);
            });
        }
        g_publicMode.store(0); g_publicSettings = {}; g_joinCode.clear();
        if (Config::multiplayerMode.exchange(0) != 0) Config::WriteCustomINIValue("Multiplayer", "Mode", "0");
        IngameNotifier::Notify("Sharing: Off");
        return;
    }
    if (g_setup || g_publicMode.load() || Config::multiplayerMode.load()) {
        IngameNotifier::Notify("Disconnect the current session before hosting or joining"); return;
    }
    if (Config::multiplayerPublicRelayUrl.empty()) {
        IngameNotifier::Notify("Public relay is not available in this build yet", IngameNotifier::Level::Warning); return;
    }
    if (action == 2 && (g_pendingCode.size() != 12 || g_pendingCode.find_first_not_of("0123456789ABCDEF") != std::string::npos)) {
        IngameNotifier::Notify("Copy your friend's 12-character join code, then choose Join session", IngameNotifier::Level::Warning); return;
    }
    unsigned char random[32];
    if (BCryptGenRandom(nullptr, random, sizeof(random), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return;
    std::string key;
    for (unsigned char byte : random) { key += "0123456789abcdef"[byte >> 4]; key += "0123456789abcdef"[byte & 15]; }
    auto result = std::make_shared<SetupResult>();
    result->settings = {action == 1 ? 1 : 2, Config::multiplayerPublicRelayUrl, "", key, true};
    g_setup = result;
    const std::string body = action == 1 ? "{\"op\":\"create\"}" : "{\"op\":\"join\",\"code\":" + Quote(g_pendingCode) + "}";
    TaskManager::Options options;
    options.type = "multiplayer_setup"; options.lane = TaskManager::Lane::Background;
    options.timeout = std::chrono::seconds(10);
    IngameNotifier::Notify("Connecting to dialogue sharing...");
    g_setupTask = TaskManager::Submit(options, [result, body](const TaskManager::CancellationToken& token) {
        const auto bytes = Request(result->settings, body, 1024, token, {}, &result->status, &result->diagnostic);
        if (bytes.empty()) return;
        result->diagnostic.stage = "invalid_setup_reply";
        const auto fields = Fields(std::string(bytes.begin(), bytes.end()));
        if (fields.size() != 4 || fields[0] != "dialectic.session.v1" || fields[1].size() != 32
            || fields[2].size() != 64 || fields[3].size() != 12
            || fields[1].find_first_not_of("0123456789abcdef") != std::string::npos
            || fields[2].find_first_not_of("0123456789abcdef") != std::string::npos
            || fields[3].find_first_not_of("0123456789ABCDEF") != std::string::npos) return;
        result->settings.session = fields[1]; result->settings.key = fields[2]; result->code = fields[3];
    }, [result](bool success, const char*) { if (!success) result->code.clear(); result->done.store(true); });
    if (!g_setupTask) result->done.store(true);
}

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
    if (g_setup) { g_setupTask.Cancel(); g_setup.reset(); }
    g_action = 0;
    if (g_settings.mode == 0) return;
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
             const std::string& cacheKey, const std::string& utteranceId, const std::vector<uint8_t>& audio) {
    if (g_settings.mode != 1) return;
    if (!g_running || !g_connected || g_commands.size() >= 32) {
        LogPlayback(utteranceId, g_commands.size() >= 32 ? "publish_skipped_queue_full" : "publish_skipped_disconnected", audio.size());
        return;
    }
    if (speaker.empty() || speaker.size() > 160 || text.empty() || text.size() > 4096
        || cacheKey.size() != 32 || cacheKey.find_first_not_of("0123456789abcdef") != std::string::npos) {
        LogPlayback(utteranceId, "publish_skipped_metadata", audio.size()); return;
    }
    if (g_settings.hosted) {
        size_t queued = audio.size();
        for (const auto& command : g_commands) queued += command.audio.size();
        if (audio.empty() || audio.size() > 4194304 || queued > 8388608) {
            LogPlayback(utteranceId, "upload_skipped_limit", audio.size());
            IngameNotifier::Notify("Shared speech skipped: audio limit reached", IngameNotifier::Level::Warning); return;
        }
    }
    // Legacy lines without an ID get a host-local ID; serial ordering still prevents retry duplicates.
    const std::string id = utteranceId.empty() ? g_owner + "_" + std::to_string(g_serial + g_commands.size() + 1) : utteranceId;
    g_commands.push_back({"publish", ",\"speaker\":" + Quote(speaker) + ",\"text\":" + Quote(text)
        + ",\"cache\":" + Quote(cacheKey) + ",\"utterance\":" + Quote(id),
        g_settings.hosted ? audio : std::vector<uint8_t>{}, id});
    LogPlayback(id, "publish_queued", audio.size());
    g_next = {};
}

void Update() {
    if (g_action || g_setup) UpdateSetup();
    if (g_publicMode.load() == 0 && Config::multiplayerMode.load() == 0 && g_settings.mode == 0) return;
    const Settings settings = g_publicMode.load() ? g_publicSettings : Settings{Config::multiplayerMode.load(), Config::multiplayerUrl,
        Config::multiplayerSession, Config::multiplayerKey};
    const auto& game = GameLoop::GetGameState();
    const bool running = settings.mode != 0 && game.isInGame && !game.isLoading;
    const auto now = Clock::now();
    if (!(settings == g_settings) || running != g_running || g_runtime != RuntimeGeneration::Current()) {
        const bool changedMode = settings.mode != g_settings.mode;
        Logger::LogInfo("Sharing: transition mode=%d->%d hosted=%d running=%d generation=%llu session=%s",
            g_settings.mode, settings.mode, settings.hosted, running, RuntimeGeneration::Current(), DiagnosticId(settings.session).c_str());
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
            if (g_failures) Logger::LogInfo("Sharing: recovered session=%s failures=%d", DiagnosticId(settings.session).c_str(), g_failures);
            if (settings.mode == 1 && result->operation != "heartbeat")
                Logger::LogInfo("Sharing: role=host session=%s line=%s op=%s accepted=1 elapsed_ms=%lld",
                    DiagnosticId(settings.session).c_str(), DiagnosticId(result->utterance).c_str(), result->operation.c_str(), result->elapsedMs);
            if (settings.mode == 2 && (result->reset || !result->control.empty()) && (g_connected || result->alive))
                Logger::LogInfo("Sharing: role=listener session=%s epoch=%s cursor=%lld reset=%d control=%s",
                    DiagnosticId(settings.session).c_str(), DiagnosticId(result->epoch).c_str(), result->cursor, result->reset, result->control.c_str());
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
                if (Interaction::IsCurrent(result->interactionEpoch) && !result->line.audio.empty()) {
                    LogPlayback(result->line.utterance, g_lines.size() < 2 ? "download_queued" : "download_dropped_queue_full", result->line.audio.size());
                    if (g_lines.size() < 2) g_lines.push_back(std::move(result->line));
                }
            }
            int delayMs = 500;
            if (settings.mode == 1) delayMs = g_commands.empty() ? 3000 : 0;
            g_next = now + std::chrono::milliseconds(delayMs);
        } else {
            if (g_failures % 8 == 0) Logger::LogWarning("Sharing: request_failed role=%d session=%s op=%s line=%s stage=%s http=%lu winhttp=%lu failures=%d elapsed_ms=%lld",
                settings.mode, DiagnosticId(settings.session).c_str(), result->operation.c_str(), DiagnosticId(result->utterance).c_str(),
                result->diagnostic.stage, result->diagnostic.status, result->diagnostic.error, g_failures + 1, result->elapsedMs);
            if (!g_failures) IngameNotifier::Notify("Sharing connection lost. Disconnect and join again if it does not recover.",
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
        if (!Interaction::Allowed()) g_lines.clear();
        SpeakManager::UpdateSharedDialogue(g_remotePaused);
        while (!g_lines.empty() && (!Interaction::IsCurrent(g_lines.front().interactionEpoch)
            || now - g_lines.front().received > std::chrono::seconds(30))) {
            LogPlayback(g_lines.front().utterance, "download_dropped_stale", g_lines.front().audio.size());
            g_lines.pop_front();
        }
        if (Interaction::Allowed() && !game.isPaused && !game.isInMenu && !g_remotePaused && !AudioManager::IsPlaying() && !g_lines.empty()) {
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
    result->operation = command.op; result->utterance = command.utterance;
    g_exchange = result;
    TaskManager::Options options;
    options.type = "multiplayer";
    options.lane = TaskManager::Lane::Background;
    options.generation = RuntimeGeneration::Current();
    options.timeout = std::chrono::seconds(10);
    g_task = TaskManager::Submit(options, [settings, body, prefix, result, audio = std::move(command.audio)](const TaskManager::CancellationToken& token) {
        const auto started = Clock::now();
        const auto bytes = Request(settings, body, 65536, token, audio, nullptr, &result->diagnostic);
        result->elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
        if (bytes.empty()) return;
        result->diagnostic.stage = "invalid_reply";
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
                RequestDiagnostics audioDiagnostic;
                result->line.audio = Request(settings, audioBody, 16777216, token, {}, nullptr, &audioDiagnostic);
                Logger::LogInfo("Sharing: role=listener session=%s line=%s cursor=%lld stage=download_result bytes=%zu http=%lu winhttp=%lu outcome=%s",
                    DiagnosticId(settings.session).c_str(), DiagnosticId(result->line.utterance).c_str(), sequence,
                    result->line.audio.size(), audioDiagnostic.status, audioDiagnostic.error, audioDiagnostic.stage);
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
