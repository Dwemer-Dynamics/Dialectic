#include "ActivityStatusFNV.h"
#include "GameThreadDispatcher.h"
#include "TaskManager.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>

namespace Logger {
void LogTrace(const char*, ...) {}
void LogDebug(const char*, ...) {}
void LogInfo(const char*, ...) {}
void LogWarning(const char*, ...) {}
void LogError(const char*, ...) {}
}

namespace {

using namespace std::chrono_literals;
int g_failures = 0;

void Check(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "FAILED: %s\n", message);
    ++g_failures;
}

bool PumpUntil(const std::function<bool()>& condition, std::uint64_t generation,
               std::chrono::milliseconds timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        GameThreadDispatcher::Pump(generation, 64);
        if (condition()) return true;
        std::this_thread::sleep_for(2ms);
    }
    GameThreadDispatcher::Pump(generation, 1024);
    return condition();
}

void TestCompletionUsesPumpingThreadWithOrdinaryQueueFull() {
    const std::thread::id pumpThread = std::this_thread::get_id();
    Check(GameThreadDispatcher::Initialize(32), "dispatcher must initialize");
    Check(TaskManager::Initialize(8, 64), "task manager must initialize");

    for (int index = 0; index < 32; ++index) {
        Check(GameThreadDispatcher::Enqueue("ordinary", std::to_string(index), 1, [] {}),
            "ordinary dispatcher queue must fill");
    }

    std::atomic_bool completed{false};
    std::thread::id completionThread;
    TaskManager::Options options;
    options.type = "integration_completion";
    options.key = "critical";
    options.generation = 1;
    options.lane = TaskManager::Lane::Interactive;
    options.timeout = 1s;
    Check(static_cast<bool>(TaskManager::Submit(std::move(options),
        [](const TaskManager::CancellationToken&) {},
        [&](bool succeeded, const char*) {
            completionThread = std::this_thread::get_id();
            completed = succeeded;
        })), "integration task must submit");

    Check(PumpUntil([&] { return completed.load(); }, 1),
        "critical task completion must survive a full ordinary dispatcher queue");
    Check(completionThread == pumpThread, "task completion must execute on the pumping game thread");
}

void TestStaleCompletionDropsOnPumpingThread() {
    const std::thread::id pumpThread = std::this_thread::get_id();
    std::atomic_bool dropped{false};
    std::thread::id completionThread;
    TaskManager::Options options;
    options.type = "integration_stale";
    options.key = "stale";
    options.generation = 7;
    options.lane = TaskManager::Lane::Gameplay;
    options.timeout = 1s;
    TaskManager::Submit(std::move(options), [](const TaskManager::CancellationToken&) {},
        [&](bool succeeded, const char* reason) {
            completionThread = std::this_thread::get_id();
            dropped = !succeeded && reason && std::string(reason) == "stale_generation";
        });
    Check(PumpUntil([&] { return dropped.load(); }, 8),
        "stale task completion must report generation drop");
    Check(completionThread == pumpThread, "stale completion cleanup must execute on the pumping thread");
}

void TestAutomaticDialogueActivityEligibility() {
    ActivityStatusFNV::AutomaticDialogueState missing;
    Check(ActivityStatusFNV::AutomaticDialogueBlockReason(missing) == nullptr,
        "missing activity state must fail open");

    ActivityStatusFNV::AutomaticDialogueState sleeping;
    sleeping.available = true;
    sleeping.sleeping = true;
    Check(std::string(ActivityStatusFNV::AutomaticDialogueBlockReason(sleeping)) == "actor is sleeping",
        "sleeping actors must be blocked from automatic dialogue");

    ActivityStatusFNV::AutomaticDialogueState unconscious;
    unconscious.available = true;
    unconscious.unconscious = true;
    Check(std::string(ActivityStatusFNV::AutomaticDialogueBlockReason(unconscious)) == "actor is unconscious",
        "unconscious actors must be blocked from automatic dialogue");
}

} // namespace

int main() {
    TestCompletionUsesPumpingThreadWithOrdinaryQueueFull();
    TestStaleCompletionDropsOnPumpingThread();
    TestAutomaticDialogueActivityEligibility();
    TaskManager::Shutdown();
    GameThreadDispatcher::Shutdown();
    if (g_failures == 0) std::printf("Task pipeline integration tests passed\n");
    return g_failures == 0 ? 0 : 1;
}
