#include "GameThreadDispatcher.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <string>

namespace Logger {
void LogTrace(const char*, ...) {}
void LogDebug(const char*, ...) {}
void LogInfo(const char*, ...) {}
void LogWarning(const char*, ...) {}
void LogError(const char*, ...) {}
}

namespace {

int g_failures = 0;

void Check(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "FAILED: %s\n", message);
    ++g_failures;
}

void TestCriticalCompletionReserve() {
    Check(GameThreadDispatcher::Initialize(32), "dispatcher must initialize");
    std::atomic_int executed{0};
    for (int index = 0; index < 32; ++index) {
        Check(GameThreadDispatcher::Enqueue("ordinary", std::to_string(index), 1,
            [&] { ++executed; }), "ordinary queue must accept its configured capacity");
    }

    std::atomic_bool rejectionObserved{false};
    Check(!GameThreadDispatcher::Enqueue("ordinary", "overflow", 1, [] {},
        [&](const char* reason) {
            const auto status = GameThreadDispatcher::GetStatus();
            rejectionObserved = reason && status.pending == 32;
        }), "ordinary queue must reject beyond configured capacity");
    Check(rejectionObserved.load(), "queue-full callback must run outside the dispatcher mutex");

    for (int index = 0; index < 128; ++index) {
        Check(GameThreadDispatcher::EnqueueCritical("task_completion", std::to_string(index), 1,
            [&] { ++executed; }), "critical completion must use reserved admission capacity");
    }
    Check(!GameThreadDispatcher::EnqueueCritical("task_completion", "overflow", 1, [] {}),
        "critical reserve must remain bounded");
    Check(GameThreadDispatcher::Pump(1, 200) == 160, "game thread must drain ordinary and critical work");
    Check(executed.load() == 160, "all admitted dispatcher work must execute exactly once");
    GameThreadDispatcher::Shutdown();
}

void TestGenerationDrop() {
    Check(GameThreadDispatcher::Initialize(32), "dispatcher must reinitialize");
    std::atomic_bool ran{false};
    std::atomic_bool dropped{false};
    Check(GameThreadDispatcher::EnqueueCritical("task_completion", "stale", 3,
        [&] { ran = true; },
        [&](const char* reason) { dropped = reason && std::string(reason) == "stale_generation"; }),
        "stale completion must enqueue");
    GameThreadDispatcher::Pump(4, 8);
    Check(!ran.load() && dropped.load(), "stale generation must drop instead of executing");
    GameThreadDispatcher::Shutdown();
}

} // namespace

int main() {
    TestCriticalCompletionReserve();
    TestGenerationDrop();
    if (g_failures == 0) std::printf("GameThreadDispatcher tests passed\n");
    return g_failures == 0 ? 0 : 1;
}
