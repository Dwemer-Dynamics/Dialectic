#include "IngameNotifier.h"

#include "GameThreadDispatcher.h"
#include "Logger.h"
#include "Misc.h"
#include "RuntimeGeneration.h"
#include "XNVSEAdapter.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <fstream>
#include <mutex>
#include <sstream>

namespace IngameNotifier {
namespace {

constexpr const char* kNotificationPath = "Data\\NVSE\\Plugins\\dialectic_notification.tmp";
constexpr size_t kMaxHudMessageLength = 220;

std::mutex g_notificationMutex;
uint64_t g_notificationCounter = 0;
std::string g_lastMessage;
std::chrono::steady_clock::time_point g_lastMessageAt{};

bool StartsWithInsensitive(const std::string& value, const char* prefix) {
    const size_t prefixLen = std::char_traits<char>::length(prefix);
    if (value.size() < prefixLen) {
        return false;
    }

    for (size_t i = 0; i < prefixLen; ++i) {
        if (std::tolower(static_cast<unsigned char>(value[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

std::string StripKnownPrefix(std::string value) {
    value = Misc::Trim(value);

    const char* prefixes[] = {
        "[DIALECTIC]",
        "[Dialectic]",
        "Dialectic:"
    };

    for (const char* prefix : prefixes) {
        if (StartsWithInsensitive(value, prefix)) {
            value = Misc::Trim(value.substr(std::char_traits<char>::length(prefix)));
            break;
        }
    }

    return value;
}

std::string NormalizeMessage(const std::string& message) {
    std::string body = StripKnownPrefix(message);
    if (body.empty()) {
        body = "Notification received.";
    }

    std::replace(body.begin(), body.end(), '\r', ' ');
    std::replace(body.begin(), body.end(), '\n', ' ');
    std::replace(body.begin(), body.end(), '\t', ' ');

    while (body.find("  ") != std::string::npos) {
        body.erase(body.find("  "), 1);
    }

    std::string normalized = "[DIALECTIC] " + Misc::Trim(body);
    if (normalized.size() > kMaxHudMessageLength) {
        normalized.resize(kMaxHudMessageLength - 3);
        normalized += "...";
    }

    return normalized;
}

const char* IconForLevel(Level level) {
    switch (level) {
    case Level::Success:
        return "#4";
    case Level::Warning:
        return "#5";
    case Level::Error:
        return "#6";
    case Level::Info:
    default:
        return "#3";
    }
}

Level InferLevel(const std::string& normalizedMessage) {
    const std::string lower = Misc::ToLower(normalizedMessage);
    if (lower.find("failed") != std::string::npos ||
        lower.find("failure") != std::string::npos ||
        lower.find("error") != std::string::npos ||
        lower.find("could not") != std::string::npos ||
        lower.find("cannot") != std::string::npos) {
        return Level::Error;
    }
    if (lower.find("warning") != std::string::npos ||
        lower.find("missing") != std::string::npos ||
        lower.find("ignored") != std::string::npos) {
        return Level::Warning;
    }
    if (lower.find("registered") != std::string::npos ||
        lower.find("connected") != std::string::npos ||
        lower.find("completed") != std::string::npos ||
        lower.find("synced") != std::string::npos) {
        return Level::Success;
    }
    return Level::Info;
}

void WriteNotificationFile(const std::string& message, Level level) {
    std::ofstream out(kNotificationPath, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        Logger::LogWarning("IngameNotifier: failed to open notification bridge file [%s]", kNotificationPath);
        return;
    }

    ++g_notificationCounter;
    out << g_notificationCounter << "\n";
    out << IconForLevel(level) << "\n";
    out << message << "\n";
}

} // namespace

void Notify(const std::string& message, Level level) {
    const std::string normalized = NormalizeMessage(message);

    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(g_notificationMutex);
        if (normalized == g_lastMessage &&
            now - g_lastMessageAt < std::chrono::milliseconds(1500)) {
            return;
        }

        g_lastMessage = normalized;
        g_lastMessageAt = now;
    }

    Logger::LogInfo("HUD notify: %s", normalized.c_str());
    const std::uint32_t emotion = level == Level::Success ? 0U :
        (level == Level::Error ? 3U : (level == Level::Warning ? 1U : 2U));
    const auto deliver = [normalized, level, emotion]() {
        if (!XNVSEAdapter::QueueNativeNotification(normalized, emotion)) {
            std::lock_guard<std::mutex> lock(g_notificationMutex);
            WriteNotificationFile(normalized, level);
        }
    };
    if (GameThreadDispatcher::IsGameThread()) {
        deliver();
    } else if (!GameThreadDispatcher::Enqueue("notification", normalized,
            RuntimeGeneration::Current(), deliver,
            [normalized, level](const char*) {
                std::lock_guard<std::mutex> lock(g_notificationMutex);
                WriteNotificationFile(normalized, level);
            })) {
        std::lock_guard<std::mutex> lock(g_notificationMutex);
        WriteNotificationFile(normalized, level);
    }
}

void NotifyRawDialecticLine(const std::string& message) {
    Notify(message, InferLevel(message));
}

} // namespace IngameNotifier
