#include "VersionCheck.h"

#include "HTTPManager.h"
#include "IngameNotifier.h"
#include "Logger.h"
#include "RuntimeGeneration.h"
#include "TaskManager.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <string>
#include <utility>

#ifndef DIALECTIC_VERSION
#define DIALECTIC_VERSION "0.5.1"
#endif

namespace {

std::string Trim(std::string value) {
    const auto isWhitespace = [](unsigned char c) { return std::isspace(c) != 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(),
        [&](char c) { return !isWhitespace(static_cast<unsigned char>(c)); }));
    value.erase(std::find_if(value.rbegin(), value.rend(),
        [&](char c) { return !isWhitespace(static_cast<unsigned char>(c)); }).base(), value.end());
    return value;
}

std::string NormalizeServerVersionForCompare(std::string version) {
    version = Trim(std::move(version));
    if (version.size() >= 3) {
        std::string suffix = version.substr(version.size() - 3);
        std::transform(suffix.begin(), suffix.end(), suffix.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (suffix == "dev") {
            version = Trim(version.substr(0, version.size() - 3));
        }
    }
    return version;
}

} // namespace

namespace VersionCheck {

void Schedule() {
    TaskManager::Options options;
    options.type = "version_check";
    options.key = "server_plugin";
    options.generation = RuntimeGeneration::Current();
    options.lane = TaskManager::Lane::Background;
    options.timeout = std::chrono::seconds(25);
    options.deadlineFromEnqueue = true;
    options.coalescing = TaskManager::CoalescingPolicy::ReplacePending;

    const TaskManager::TaskHandle handle = TaskManager::Submit(std::move(options),
        [](const TaskManager::CancellationToken& token) {
            if (!token.WaitFor(std::chrono::seconds(10))) {
                return;
            }

            const std::string rawServerVersion = HTTPManager::GetServerVersionRaw();
            if (rawServerVersion.empty()) {
                Logger::LogInfo("[VersionCheck] Server version unavailable, skipping mismatch warning");
                return;
            }

            const std::string serverVersion = NormalizeServerVersionForCompare(rawServerVersion);
            const std::string pluginVersion = DIALECTIC_VERSION;
            if (serverVersion.empty()) {
                Logger::LogError("[VersionCheck] Server version was empty after normalization");
                return;
            }

            if (pluginVersion == serverVersion) {
                Logger::LogInfo("[VersionCheck] Plugin and server versions match (plugin=%s, serverRaw=%s)",
                    pluginVersion.c_str(), rawServerVersion.c_str());
                return;
            }

            const std::string warning = "Version mismatch: Plugin " + pluginVersion +
                " / Server " + rawServerVersion;
            Logger::LogWarning("[VersionCheck] [DIALECTIC] %s", warning.c_str());
            IngameNotifier::Notify(warning, IngameNotifier::Level::Warning);
        });

    if (!handle) {
        Logger::LogWarning("[VersionCheck] Could not queue server/plugin version check");
    }
}

} // namespace VersionCheck
