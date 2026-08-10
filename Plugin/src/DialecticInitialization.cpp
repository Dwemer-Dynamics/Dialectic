#include "DialecticInitialization.h"

#include <deque>
#include <mutex>
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
bool g_worldFinished = false;
bool g_worldSucceeded = false;

void QueueFinalNoticeIfReady() {
    if (!g_active || !g_voiceFinished || !g_worldFinished) {
        return;
    }

    const bool success = g_voiceSucceeded && g_worldSucceeded;
    g_notices.push_back({
        success ? "Initialization complete." : "Initialization finished with errors.",
        success ? IngameNotifier::Level::Success : IngameNotifier::Level::Error
    });
    g_active = false;
}

} // namespace

void Begin() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_active = true;
    g_voiceFinished = false;
    g_voiceSucceeded = false;
    g_worldFinished = false;
    g_worldSucceeded = false;
}

void QueueNotice(std::string message, IngameNotifier::Level level) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_notices.push_back({std::move(message), level});
}

void ReportVoiceFinished(bool success) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_notices.push_back({
        success ? "Voice samples synced." : "Voice sample sync failed.",
        success ? IngameNotifier::Level::Success : IngameNotifier::Level::Error
    });
    g_voiceFinished = true;
    g_voiceSucceeded = success;
    QueueFinalNoticeIfReady();
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
        notices.swap(g_notices);
    }

    for (const Notice& notice : notices) {
        IngameNotifier::Notify(notice.message, notice.level);
    }
}

} // namespace DialecticInitialization
