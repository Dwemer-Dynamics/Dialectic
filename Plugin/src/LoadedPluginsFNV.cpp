#include "LoadedPluginsFNV.h"

#include "HTTPManager.h"
#include "Logger.h"
#include "RuntimeGeneration.h"
#include "TaskManager.h"
#include "XNVSEAdapter.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace LoadedPluginsFNV {
namespace {

constexpr auto kRetryInterval = std::chrono::seconds(5);

struct PluginRow {
    std::string pluginName;
    uint32_t compileIndex = 0;
    std::string formIdPrefix;
};

std::atomic<bool> g_syncRequested(false);
std::atomic<bool> g_synced(false);
std::atomic<bool> g_syncInProgress(false);
std::chrono::steady_clock::time_point g_lastAttempt;

std::string FormatFormIdPrefix(uint32_t compileIndex) {
    std::ostringstream prefix;
    prefix << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << (compileIndex & 0xFF);
    return prefix.str();
}

std::vector<PluginRow> CollectLoadedPlugins() {
    std::vector<PluginRow> plugins;
    std::vector<XNVSEAdapter::NativeLoadedPlugin> nativePlugins;
    if (!XNVSEAdapter::CaptureNativeLoadedPlugins(nativePlugins)) {
        Logger::LogWarning("[LOADED_PLUGINS] Native DataHandler snapshot unavailable");
        return plugins;
    }
    plugins.reserve(nativePlugins.size());
    for (const auto& native : nativePlugins) {
        plugins.push_back({native.name, native.compileIndex,
            FormatFormIdPrefix(native.compileIndex)});
    }

    return plugins;
}

std::string BuildPayload(const std::vector<PluginRow>& plugins) {
    std::ostringstream json;
    json << "{\"type\":\"loaded_plugins\",\"plugins\":[";
    for (size_t i = 0; i < plugins.size(); ++i) {
        if (i > 0) {
            json << ",";
        }

        const PluginRow& plugin = plugins[i];
        json << "{";
        json << "\"plugin_name\":\"" << HTTPManager::EscapeJson(plugin.pluginName) << "\",";
        json << "\"is_light\":false,";
        json << "\"compile_index\":" << plugin.compileIndex << ",";
        json << "\"small_file_compile_index\":0,";
        json << "\"partial_index\":0,";
        json << "\"formid_prefix\":\"" << HTTPManager::EscapeJson(plugin.formIdPrefix) << "\"";
        json << "}";
    }
    json << "]}";
    return json.str();
}

void SendSyncPayloadAsync(std::string payload, size_t pluginCount) {
    TaskManager::Enqueue("gamedata", "loaded_plugins", RuntimeGeneration::Current(), false,
        std::chrono::seconds(30), [payload = std::move(payload), pluginCount](const TaskManager::CancellationToken& token) {
        if (token.IsCancellationRequested()) {
            g_syncInProgress = false;
            return;
        }
        Logger::LogInfo("[LOADED_PLUGINS] Syncing %zu loaded Fallout plugins", pluginCount);
        const std::string response = HTTPManager::SendJson("gamedata.php", payload);
        std::string normalizedResponse = response;
        normalizedResponse.erase(
            std::remove_if(normalizedResponse.begin(), normalizedResponse.end(), [](unsigned char ch) {
                return std::isspace(ch) != 0;
            }),
            normalizedResponse.end());
        std::transform(normalizedResponse.begin(), normalizedResponse.end(), normalizedResponse.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });

        const bool success =
            normalizedResponse == "ok" ||
            normalizedResponse.find("\"ok\":true") != std::string::npos ||
            normalizedResponse.find("\"status\":\"success\"") != std::string::npos ||
            normalizedResponse.find("\"status\":\"ok\"") != std::string::npos ||
            normalizedResponse.find("\"success\":true") != std::string::npos;
        if (success) {
            g_synced = true;
            g_syncRequested = false;
            Logger::LogInfo("[LOADED_PLUGINS] Synced %zu loaded Fallout plugins", pluginCount);
        } else {
            Logger::LogWarning("[LOADED_PLUGINS] Sync returned an empty/non-OK response; will retry");
        }

        g_syncInProgress = false;
    });
}

} // namespace

void RequestSync() {
    if (!g_synced.load()) {
        g_syncRequested = true;
    }
}

bool IsSynced() {
    return g_synced.load();
}

void Update() {
    if (g_synced.load() || !g_syncRequested.load()) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (g_lastAttempt.time_since_epoch().count() != 0 && now - g_lastAttempt < kRetryInterval) {
        return;
    }

    g_lastAttempt = now;
    if (g_syncInProgress.exchange(true)) {
        return;
    }

    const auto plugins = CollectLoadedPlugins();
    if (plugins.empty()) {
        Logger::LogWarning("[LOADED_PLUGINS] No active plugins discovered; will retry");
        g_syncInProgress = false;
        return;
    }

    SendSyncPayloadAsync(BuildPayload(plugins), plugins.size());
}

} // namespace LoadedPluginsFNV
