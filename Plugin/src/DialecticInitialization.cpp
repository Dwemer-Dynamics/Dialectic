#include "DialecticInitialization.h"

#include <chrono>
#include <deque>
#include <mutex>
#include <sstream>
#include <utility>

namespace DialecticInitialization {
namespace {

struct Notice {
    std::string message;
    IngameNotifier::Level level = IngameNotifier::Level::Info;
};

std::mutex g_mutex;
std::deque<Notice> g_notices;
bool g_active = false;
bool g_voiceFinished = false;
bool g_voiceSucceeded = false;
std::size_t g_voiceCompleted = 0;
std::size_t g_voiceTotal = 0;
bool g_worldFinished = false;
bool g_worldSucceeded = false;
std::string g_worldStage;
std::size_t g_worldCompleted = 0;
std::size_t g_worldTotal = 0;
std::chrono::steady_clock::time_point g_lastProgressNotice{};

std::string BuildProgressMessage() {
    std::ostringstream message;
    message << "DIALECTIC initialization: ";
    if (g_voiceFinished) {
        message << "voices complete";
    } else if (g_voiceTotal > 0) {
        message << "voices " << g_voiceCompleted << "/" << g_voiceTotal;
    } else {
        message << "finding voices";
    }

    message << "; ";
    if (g_worldFinished) {
        message << "world data complete";
    } else if (!g_worldStage.empty()) {
        message << g_worldStage;
        if (g_worldTotal > 0) {
            message << " " << g_worldCompleted << "/" << g_worldTotal;
        }
    } else {
        message << "finding world data";
    }
    message << ".";
    return message.str();
}

void QueueFinalNoticeIfReady() {
    if (!g_active || !g_voiceFinished || !g_worldFinished) {
        return;
    }

    const bool success = g_voiceSucceeded && g_worldSucceeded;
    g_notices.push_back({
        success ? "DIALECTIC initialized." : "DIALECTIC initialization failed.",
        success ? IngameNotifier::Level::Success : IngameNotifier::Level::Error
    });
    g_active = false;
}

} // namespace

bool TryBegin() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_active) {
        return false;
    }

    g_active = true;
    g_voiceFinished = false;
    g_voiceSucceeded = false;
    g_voiceCompleted = 0;
    g_voiceTotal = 0;
    g_worldFinished = false;
    g_worldSucceeded = false;
    g_worldStage = "finding world data";
    g_worldCompleted = 0;
    g_worldTotal = 0;
    g_lastProgressNotice = std::chrono::steady_clock::now();
    g_notices.push_back({
        "DIALECTIC initialization has started. Please wait.",
        IngameNotifier::Level::Success
    });
    return true;
}

void QueueNotice(std::string message, IngameNotifier::Level level) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_notices.push_back({std::move(message), level});
}

void ReportVoiceProgress(std::size_t completed, std::size_t total) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_voiceCompleted = completed;
    g_voiceTotal = total;
}

void ReportVoiceFinished(bool success) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_notices.push_back({
        success ? "Voices synced." : "Voice sync failed.",
        success ? IngameNotifier::Level::Success : IngameNotifier::Level::Error
    });
    g_voiceFinished = true;
    g_voiceSucceeded = success;
    QueueFinalNoticeIfReady();
}

void ReportWorldProgress(std::string stage, std::size_t completed, std::size_t total) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_worldStage = std::move(stage);
    g_worldCompleted = completed;
    g_worldTotal = total;
}

void ReportWorldFinished(bool success) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_worldFinished = true;
    g_worldSucceeded = success;
    QueueFinalNoticeIfReady();
}

void Update() {
    std::deque<Notice> notices;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto now = std::chrono::steady_clock::now();
        if (g_active && now - g_lastProgressNotice >= std::chrono::seconds(5)) {
            g_notices.push_back({BuildProgressMessage(), IngameNotifier::Level::Success});
            g_lastProgressNotice = now;
        }
        notices.swap(g_notices);
    }

    for (const Notice& notice : notices) {
        IngameNotifier::Notify(notice.message, notice.level);
    }
}

} // namespace DialecticInitialization
