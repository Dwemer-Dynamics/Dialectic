#include "MultiplayerSharing.h"
#include "ServerPluginSync.h"

#include "Config.h"
#include "Logger.h"
#include "TaskManager.h"

#include <Windows.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace
{
    constexpr std::uintmax_t kMaximumPackageBytes = 512ULL * 1024ULL * 1024ULL;
    constexpr std::size_t kUploadChunkBytes = 1024ULL * 1024ULL;
    std::atomic_bool g_syncScheduled{false};

    struct ServerPluginPackage
    {
        std::string name;
        std::string version;
        std::filesystem::path archive;
        std::uintmax_t size{0};
    };

    struct HttpResponse
    {
        DWORD status{0};
        std::string body;

        bool ok() const { return status >= 200 && status < 300; }
    };

    std::wstring ToWide(const std::string& value)
    {
        if (value.empty()) return {};
        const int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
        if (size <= 1) return std::wstring(value.begin(), value.end());
        std::wstring result(static_cast<std::size_t>(size), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), size);
        result.resize(static_cast<std::size_t>(size - 1));
        return result;
    }

    std::string EscapeJson(const std::string& value)
    {
        std::ostringstream escaped;
        for (const unsigned char character : value) {
            switch (character) {
                case '\"': escaped << "\\\""; break;
                case '\\': escaped << "\\\\"; break;
                case '\b': escaped << "\\b"; break;
                case '\f': escaped << "\\f"; break;
                case '\n': escaped << "\\n"; break;
                case '\r': escaped << "\\r"; break;
                case '\t': escaped << "\\t"; break;
                default:
                    if (character < 0x20) {
                        escaped << "\\u00" << std::hex << std::uppercase
                                << static_cast<int>(character >> 4)
                                << static_cast<int>(character & 0x0F)
                                << std::dec;
                    } else {
                        escaped << static_cast<char>(character);
                    }
            }
        }
        return escaped.str();
    }

    std::string JsonStringValue(const std::string& body, const std::string& key)
    {
        const std::string needle = "\"" + key + "\"";
        const std::size_t keyPosition = body.find(needle);
        if (keyPosition == std::string::npos) return {};
        const std::size_t colon = body.find(':', keyPosition + needle.size());
        if (colon == std::string::npos) return {};
        const std::size_t openingQuote = body.find('\"', colon + 1);
        if (openingQuote == std::string::npos) return {};

        std::string value;
        bool escaped = false;
        for (std::size_t index = openingQuote + 1; index < body.size(); ++index) {
            const char character = body[index];
            if (escaped) {
                switch (character) {
                    case 'n': value.push_back('\n'); break;
                    case 'r': value.push_back('\r'); break;
                    case 't': value.push_back('\t'); break;
                    default: value.push_back(character); break;
                }
                escaped = false;
            } else if (character == '\\') {
                escaped = true;
            } else if (character == '\"') {
                return value;
            } else {
                value.push_back(character);
            }
        }
        return {};
    }

    bool JsonBooleanValue(const std::string& body, const std::string& key, bool fallback)
    {
        const std::string needle = "\"" + key + "\"";
        const std::size_t keyPosition = body.find(needle);
        if (keyPosition == std::string::npos) return fallback;
        const std::size_t colon = body.find(':', keyPosition + needle.size());
        if (colon == std::string::npos) return fallback;
        const std::size_t value = body.find_first_not_of(" \t\r\n", colon + 1);
        if (value == std::string::npos) return fallback;
        if (body.compare(value, 4, "true") == 0) return true;
        if (body.compare(value, 5, "false") == 0) return false;
        return fallback;
    }

    bool IsSafePluginName(const std::string& value)
    {
        static const std::regex pattern(R"(^[A-Za-z0-9][A-Za-z0-9 ._-]{0,63}$)");
        return std::regex_match(value, pattern) && value.back() != '.' && value.back() != ' ';
    }

    bool IsSafeVersion(const std::string& value)
    {
        static const std::regex pattern(R"(^[0-9A-Za-z][0-9A-Za-z._+-]{0,63}$)");
        return std::regex_match(value, pattern);
    }

    bool IsSupportedPackageArchive(const std::filesystem::path& path)
    {
        std::string extension = path.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        return extension == ".dwpkg" || extension == ".zip";
    }

    std::string PackageApiPath(const std::string& action)
    {
        std::string path = Config::serverPath;
        const auto query = path.find('?');
        if (query != std::string::npos) path.resize(query);
        const auto position = path.find_last_of("/\\");
        path = position == std::string::npos
            ? "ui/api/plugin_packages.php"
            : path.substr(0, position + 1) + "ui/api/plugin_packages.php";
        path.append("?action=").append(action);
        if (path.empty() || path.front() != '/') path.insert(path.begin(), '/');
        return path;
    }

    HttpResponse Request(const std::string& method, const std::string& path, const std::string& contentType,
                         const char* data, std::size_t size, const TaskManager::CancellationToken& token)
    {
        HttpResponse response;
        if (MultiplayerSharing::IsListener()) return response;
        if (token.IsCancellationRequested() || path.empty() ||
            size > static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())) return response;
        if (Config::serverHost.empty() || Config::serverPort < 1 || Config::serverPort > 65535) return response;

        HINTERNET session = WinHttpOpen(L"Dialectic Server Plugin Sync/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) return response;
        WinHttpSetTimeouts(session, 5000, 5000, 30000, 30000);

        HINTERNET connection = WinHttpConnect(session, ToWide(Config::serverHost).c_str(),
                                               static_cast<INTERNET_PORT>(Config::serverPort), 0);
        if (!connection) {
            WinHttpCloseHandle(session);
            return response;
        }
        HINTERNET request = WinHttpOpenRequest(connection, ToWide(method).c_str(), ToWide(path).c_str(), nullptr,
                                               WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
        if (!request) {
            WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);
            return response;
        }

        auto interruptibleRequest = std::make_shared<std::atomic<HINTERNET>>(request);
        token.SetInterrupt([interruptibleRequest]() {
            HINTERNET active = interruptibleRequest->exchange(nullptr);
            if (active) WinHttpCloseHandle(active);
        });

        const std::wstring headers = ToWide("Content-Type: " + contentType + "\r\nAccept: application/json\r\n");
        const DWORD bodySize = static_cast<DWORD>(size);
        const BOOL sent = WinHttpSendRequest(
            request, headers.c_str(), static_cast<DWORD>(-1L), size > 0 ? const_cast<char*>(data) : WINHTTP_NO_REQUEST_DATA,
            bodySize, bodySize, 0);
        if (sent && WinHttpReceiveResponse(request, nullptr)) {
            DWORD statusSize = sizeof(response.status);
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &response.status, &statusSize, WINHTTP_NO_HEADER_INDEX);
            DWORD available = 0;
            while (WinHttpQueryDataAvailable(request, &available) && available > 0) {
                std::string buffer(available, '\0');
                DWORD read = 0;
                if (!WinHttpReadData(request, buffer.data(), available, &read) || read == 0) break;
                response.body.append(buffer.data(), read);
            }
        }
        token.ClearInterrupt();
        request = interruptibleRequest->exchange(nullptr);
        if (request) WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return response;
    }

    HttpResponse PostJson(const std::string& action, const std::string& body,
                          const TaskManager::CancellationToken& token)
    {
        return Request("POST", PackageApiPath(action), "application/json", body.data(), body.size(), token);
    }

    bool ResponseSucceeded(const HttpResponse& response, const std::string& operation)
    {
        if (!response.ok()) {
            Logger::LogWarning("[SERVER_PLUGIN_SYNC] %s failed with HTTP %lu",
                               operation.c_str(), static_cast<unsigned long>(response.status));
            return false;
        }
        if (!JsonBooleanValue(response.body, "ok", false)) {
            const std::string error = JsonStringValue(response.body, "error");
            Logger::LogWarning("[SERVER_PLUGIN_SYNC] %s failed: %s", operation.c_str(),
                               error.empty() ? "invalid server response" : error.c_str());
            return false;
        }
        return true;
    }

    std::vector<ServerPluginPackage> FindPackages()
    {
        const std::filesystem::path root("Data/Dialectic/server-plugins");
        std::vector<ServerPluginPackage> packages;
        std::error_code error;
        if (!std::filesystem::is_directory(root, error)) return packages;

        for (const auto& directory : std::filesystem::directory_iterator(root, error)) {
            if (error || !directory.is_directory()) continue;
            const std::string name = directory.path().filename().string();
            if (!IsSafePluginName(name)) {
                Logger::LogWarning("[SERVER_PLUGIN_SYNC] Ignoring unsafe plugin folder name: %s", name.c_str());
                continue;
            }

            std::vector<std::filesystem::directory_entry> archives;
            for (const auto& file : std::filesystem::directory_iterator(directory.path(), error)) {
                if (!error && file.is_regular_file() && IsSupportedPackageArchive(file.path())) archives.push_back(file);
            }
            if (archives.empty()) continue;
            std::sort(archives.begin(), archives.end(), [](const auto& left, const auto& right) {
                return left.last_write_time() > right.last_write_time();
            });
            if (archives.size() > 1) {
                Logger::LogWarning("[SERVER_PLUGIN_SYNC] %s has multiple packages; using newest file %s", name.c_str(),
                                   archives.front().path().filename().string().c_str());
            }
            const auto& archive = archives.front();
            const std::string version = archive.path().stem().string();
            if (!IsSafeVersion(version)) {
                Logger::LogWarning("[SERVER_PLUGIN_SYNC] Ignoring %s because package filename is not a valid version: %s",
                                   name.c_str(), archive.path().filename().string().c_str());
                continue;
            }
            const auto size = archive.file_size(error);
            if (error || size == 0 || size > kMaximumPackageBytes) {
                Logger::LogWarning("[SERVER_PLUGIN_SYNC] Ignoring %s %s because its size is invalid", name.c_str(), version.c_str());
                continue;
            }
            packages.push_back({name, version, archive.path(), size});
        }
        std::sort(packages.begin(), packages.end(), [](const auto& left, const auto& right) {
            return left.name < right.name;
        });
        return packages;
    }

    bool UploadPackage(const ServerPluginPackage& package, const TaskManager::CancellationToken& token)
    {
        const std::string probeBody = "{\"name\":\"" + EscapeJson(package.name) +
            "\",\"version\":\"" + EscapeJson(package.version) + "\"}";
        const HttpResponse probe = PostJson("probe", probeBody, token);
        if (!ResponseSucceeded(probe, "probe " + package.name)) return false;
        if (!JsonBooleanValue(probe.body, "upload_required", true)) {
            Logger::LogInfo("[SERVER_PLUGIN_SYNC] %s %s is already current", package.name.c_str(), package.version.c_str());
            return true;
        }

        const std::size_t totalChunks = static_cast<std::size_t>((package.size + kUploadChunkBytes - 1) / kUploadChunkBytes);
        std::ostringstream startBody;
        startBody << "{\"name\":\"" << EscapeJson(package.name)
                  << "\",\"version\":\"" << EscapeJson(package.version)
                  << "\",\"archive_name\":\"" << EscapeJson(package.archive.filename().string())
                  << "\",\"size\":" << package.size
                  << ",\"total_chunks\":" << totalChunks << "}";
        const HttpResponse started = PostJson("start-upload", startBody.str(), token);
        if (!ResponseSucceeded(started, "start upload " + package.name)) return false;
        const std::string uploadId = JsonStringValue(started.body, "upload_id");
        if (uploadId.empty()) return false;

        std::ifstream stream(package.archive, std::ios::binary);
        if (!stream.is_open()) return false;
        std::vector<char> buffer(kUploadChunkBytes);
        for (std::size_t index = 0; index < totalChunks; ++index) {
            if (token.IsCancellationRequested()) return false;
            stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto count = stream.gcount();
            if (count <= 0) return false;
            const std::string action = "upload-chunk&upload_id=" + uploadId + "&index=" + std::to_string(index);
            const auto response = Request("POST", PackageApiPath(action), "application/octet-stream", buffer.data(),
                                          static_cast<std::size_t>(count), token);
            if (!ResponseSucceeded(response, "upload chunk " + std::to_string(index + 1) + " for " + package.name)) return false;
            if (index + 1 == totalChunks) {
                if (!JsonBooleanValue(response.body, "complete", false) ||
                    JsonStringValue(response.body, "status") != "completed") {
                    const std::string error = JsonStringValue(response.body, "error");
                    Logger::LogWarning("[SERVER_PLUGIN_SYNC] %s %s was uploaded but activation failed: %s",
                                       package.name.c_str(), package.version.c_str(),
                                       error.empty() ? "unknown error" : error.c_str());
                    return false;
                }
            }
        }
        Logger::LogInfo("[SERVER_PLUGIN_SYNC] Installed %s %s", package.name.c_str(), package.version.c_str());
        return true;
    }

    bool SyncPackages(const TaskManager::CancellationToken& token)
    {
        const auto packages = FindPackages();
        if (packages.empty()) {
            Logger::LogInfo("[SERVER_PLUGIN_SYNC] No bundled server plugins found");
            return true;
        }
        Logger::LogInfo("[SERVER_PLUGIN_SYNC] Found %zu bundled server plugin(s)", packages.size());
        bool complete = true;
        for (const auto& package : packages) {
            if (token.IsCancellationRequested()) return false;
            if (!UploadPackage(package, token)) complete = false;
        }
        return complete;
    }
}

void ScheduleServerPluginSync()
{
    if (MultiplayerSharing::IsListener()) return;
    bool expected = false;
    if (!g_syncScheduled.compare_exchange_strong(expected, true)) return;
    TaskManager::Options options;
    options.type = "server_plugin_sync";
    options.key = "startup";
    options.lane = TaskManager::Lane::Background;
    options.timeout = std::chrono::minutes(15);
    options.deadlineFromEnqueue = true;
    options.coalescing = TaskManager::CoalescingPolicy::RejectIfPendingOrActive;
    options.concurrencyLimit = 1;
    const TaskManager::TaskHandle handle = TaskManager::Submit(std::move(options),
        [](const TaskManager::CancellationToken& token) {
            if (SyncPackages(token) || token.IsCancellationRequested()) return;
            Logger::LogWarning("[SERVER_PLUGIN_SYNC] Startup sync was incomplete; retrying once");
            if (!SyncPackages(token) && !token.IsCancellationRequested()) {
                Logger::LogWarning("[SERVER_PLUGIN_SYNC] Automatic server plugin sync remains incomplete");
            }
        });
    if (!handle) {
        g_syncScheduled = false;
        Logger::LogWarning("[SERVER_PLUGIN_SYNC] Could not schedule automatic sync");
    }
}
