#include "TaskManager.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace Logger {
void LogTrace(const char*, ...) {}
void LogDebug(const char*, ...) {}
void LogInfo(const char*, ...) {}
void LogWarning(const char*, ...) {}
void LogError(const char*, ...) {}
}

namespace GameThreadDispatcher {
bool Enqueue(std::string, std::string, std::uint64_t, std::function<void()> execute,
             std::function<void(const char*)>) {
    if (execute) execute();
    return true;
}
bool EnqueueCritical(std::string, std::string, std::uint64_t, std::function<void()> execute,
                     std::function<void(const char*)>) {
    if (execute) execute();
    return true;
}
}

namespace {

using namespace std::chrono_literals;

int g_failures = 0;

void Check(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "FAILED: %s\n", message);
    ++g_failures;
}

bool WaitUntil(const std::function<bool()>& condition, std::chrono::milliseconds timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return condition();
}

TaskManager::Options Options(const char* type, TaskManager::Lane lane) {
    TaskManager::Options options;
    options.type = type;
    options.key = type;
    options.lane = lane;
    options.timeout = 2s;
    return options;
}

void TestInteractiveLaneIsNotStarved() {
    std::atomic_bool releaseBackground{false};
    std::atomic_int backgroundStarted{0};
    for (int index = 0; index < 4; ++index) {
        auto options = Options(("background_" + std::to_string(index)).c_str(), TaskManager::Lane::Background);
        TaskManager::Submit(std::move(options), [&](const TaskManager::CancellationToken& token) {
            ++backgroundStarted;
            while (!releaseBackground.load() && token.WaitFor(10ms)) {}
        });
    }
    Check(WaitUntil([&] { return backgroundStarted.load() == 1; }), "background lane must enforce its concurrency cap");

    std::atomic_bool interactiveRan{false};
    auto interactive = Options("interactive_test", TaskManager::Lane::Interactive);
    interactive.priority = true;
    TaskManager::Submit(std::move(interactive), [&](const TaskManager::CancellationToken&) {
        interactiveRan = true;
    });
    Check(WaitUntil([&] { return interactiveRan.load(); }, 300ms), "interactive work must run while background work is blocked");
    releaseBackground = true;
    TaskManager::CancelByTypePrefix("background_");
}

void TestAudioLaneIsReserved() {
    std::atomic_bool releaseInteractive{false};
    std::atomic_int interactiveStarted{0};
    for (int index = 0; index < 8; ++index) {
        auto options = Options(("interactive_blocker_" + std::to_string(index)).c_str(), TaskManager::Lane::Interactive);
        TaskManager::Submit(std::move(options), [&](const TaskManager::CancellationToken& token) {
            ++interactiveStarted;
            while (!releaseInteractive.load() && token.WaitFor(10ms)) {}
        });
    }
    Check(WaitUntil([&] { return interactiveStarted.load() == 4; }), "interactive lane must stop at its reserved cap");

    std::atomic_bool audioRan{false};
    auto audio = Options("reserved_audio", TaskManager::Lane::Audio);
    TaskManager::Submit(std::move(audio), [&](const TaskManager::CancellationToken&) { audioRan = true; });
    Check(WaitUntil([&] { return audioRan.load(); }, 300ms), "audio must run while interactive work is saturated");
    releaseInteractive = true;
    TaskManager::CancelByTypePrefix("interactive_blocker_");
}

void TestPendingCoalescing() {
    std::atomic_bool release{false};
    auto blocker = Options("coalesce_blocker", TaskManager::Lane::Background);
    TaskManager::Submit(std::move(blocker), [&](const TaskManager::CancellationToken& token) {
        while (!release.load() && token.WaitFor(10ms)) {}
    });
    Check(WaitUntil([] { return TaskManager::GetStatus().active > 0; }), "coalescing blocker must start");

    std::atomic_int ran{0};
    std::atomic_int coalesced{0};
    for (int index = 0; index < 2; ++index) {
        auto options = Options("coalesced_task", TaskManager::Lane::Background);
        options.key = "same";
        options.coalescing = TaskManager::CoalescingPolicy::ReplacePending;
        TaskManager::Submit(std::move(options), [&](const TaskManager::CancellationToken&) { ++ran; },
            [&](bool, const char* reason) {
                if (reason && std::string(reason) == "coalesced") ++coalesced;
            });
    }
    Check(WaitUntil([&] { return coalesced.load() == 1; }), "replacement must complete the superseded pending task as coalesced");
    release = true;
    Check(WaitUntil([&] { return ran.load() == 1; }), "only the newest coalesced task may run");
}

void TestCancellationById() {
    std::atomic_bool started{false};
    std::atomic_bool completedCancelled{false};
    auto options = Options("cancel_by_id", TaskManager::Lane::Interactive);
    const auto handle = TaskManager::Submit(std::move(options), [&](const TaskManager::CancellationToken& token) {
        started = true;
        while (token.WaitFor(10ms)) {}
    }, [&](bool succeeded, const char*) { completedCancelled = !succeeded; });
    Check(handle.id != 0, "cancel-by-id task must enqueue");
    Check(WaitUntil([&] { return started.load(); }), "cancel-by-id task must start");
    Check(handle.Cancel(), "task handle must cancel active task");
    Check(!handle.Cancel(), "repeated cancellation must not report the task twice");
    Check(WaitUntil([&] { return completedCancelled.load(); }), "cancelled task completion must execute exactly once");
}

void TestDeadlineInterruptsBlockingWork() {
    std::atomic_bool interrupted{false};
    std::atomic_bool completionTimedOut{false};
    auto options = Options("deadline_interrupt", TaskManager::Lane::Interactive);
    options.timeout = 100ms;
    options.deadlineFromEnqueue = true;
    TaskManager::Submit(std::move(options), [&](const TaskManager::CancellationToken& token) {
        token.SetInterrupt([&] { interrupted = true; });
        while (token.WaitFor(10ms)) {}
    }, [&](bool succeeded, const char* reason) {
        completionTimedOut = !succeeded && reason && std::string(reason) == "timeout";
    });
    Check(WaitUntil([&] { return interrupted.load(); }), "deadline must invoke the blocking-resource interrupt");
    Check(WaitUntil([&] { return completionTimedOut.load(); }), "deadline completion reason must be timeout");
}

void TestPendingDeadlineExpiresInQueue() {
    std::atomic_bool release{false};
    std::atomic_int blockersStarted{0};
    for (int index = 0; index < 2; ++index) {
        auto blocker = Options(("audio_blocker_" + std::to_string(index)).c_str(), TaskManager::Lane::Audio);
        TaskManager::Submit(std::move(blocker), [&](const TaskManager::CancellationToken& token) {
            ++blockersStarted;
            while (!release.load() && token.WaitFor(10ms)) {}
        });
    }
    Check(WaitUntil([&] { return blockersStarted.load() == 2; }), "audio lane blockers must fill the lane");

    std::atomic_bool ran{false};
    std::atomic_int completions{0};
    std::atomic_bool timedOut{false};
    auto queued = Options("queued_deadline", TaskManager::Lane::Audio);
    queued.timeout = 100ms;
    queued.deadlineFromEnqueue = true;
    TaskManager::Submit(std::move(queued), [&](const TaskManager::CancellationToken&) { ran = true; },
        [&](bool succeeded, const char* reason) {
            ++completions;
            timedOut = !succeeded && reason && std::string(reason) == "timeout";
        });
    Check(WaitUntil([&] { return timedOut.load(); }, 500ms), "queued deadline must expire without waiting for a worker");
    Check(!ran.load(), "expired queued work must never run");
    Check(completions.load() == 1, "expired queued work must complete exactly once");
    release = true;
    TaskManager::CancelByTypePrefix("audio_blocker_");
}

void TestExceptionDiagnostics() {
    std::atomic_bool completed{false};
    auto options = Options("exception_diagnostic", TaskManager::Lane::Gameplay);
    TaskManager::Submit(std::move(options), [](const TaskManager::CancellationToken&) {
        throw std::runtime_error("diagnostic failure");
    }, [&](bool succeeded, const char* reason) {
        completed = !succeeded && reason && std::string(reason) == "exception";
    });
    Check(WaitUntil([&] { return completed.load(); }), "throwing work must report exception completion");
    const auto snapshot = TaskManager::GetSnapshot();
    const auto type = std::find_if(snapshot.types.begin(), snapshot.types.end(), [](const auto& status) {
        return status.type == "exception_diagnostic";
    });
    Check(type != snapshot.types.end() && type->lastError == "diagnostic failure",
        "task health must preserve the latest exception detail");
}

void TestMixedWorkloadCompletesExactlyOnce() {
    constexpr int kTaskCount = 200;
    std::atomic_int completions{0};
    std::vector<TaskManager::TaskHandle> handles;
    handles.reserve(kTaskCount);
    for (int index = 0; index < kTaskCount; ++index) {
        const auto lane = static_cast<TaskManager::Lane>(index % 5);
        auto options = Options(("stress_" + std::to_string(index % 7)).c_str(), lane);
        options.key = std::to_string(index);
        options.timeout = 2s;
        handles.push_back(TaskManager::Submit(std::move(options),
            [](const TaskManager::CancellationToken& token) { token.WaitFor(1ms); },
            [&](bool, const char*) { ++completions; }));
    }
    const int accepted = static_cast<int>(std::count_if(handles.begin(), handles.end(), [](const auto& handle) {
        return static_cast<bool>(handle);
    }));
    for (int index = 0; index < kTaskCount; index += 3) handles[index].Cancel();
    Check(accepted == kTaskCount, "bounded stress workload must be admitted");
    Check(WaitUntil([&] { return completions.load() == accepted; }, 3s),
        "every accepted mixed-lane task must complete exactly once");
    Check(completions.load() == accepted, "mixed-lane completion count must not duplicate");
    Check(WaitUntil([] {
        const auto status = TaskManager::GetStatus();
        return status.pending == 0 && status.active == 0;
    }), "mixed workload must fully drain");
}

void TestScopedCancellation() {
    std::atomic_bool release{false};
    std::atomic_bool blockerStarted{false};
    auto blocker = Options("scope_blocker", TaskManager::Lane::Background);
    TaskManager::Submit(std::move(blocker), [&](const TaskManager::CancellationToken& token) {
        blockerStarted = true;
        while (!release.load() && token.WaitFor(10ms)) {}
    });
    Check(WaitUntil([&] { return blockerStarted.load(); }), "scope blocker must start");

    std::atomic_int cancelledCompletions{0};
    auto submitScoped = [&](const char* type, TaskManager::Scope scope) {
        auto options = Options(type, TaskManager::Lane::Background);
        options.scope = std::move(scope);
        return TaskManager::Submit(std::move(options), [](const TaskManager::CancellationToken&) {},
            [&](bool succeeded, const char*) { if (!succeeded) ++cancelledCompletions; });
    };
    submitScoped("scope_actor", {0x1234, 0, {}});
    submitScoped("scope_turn", {0, 77, {}});
    submitScoped("scope_utterance", {0, 0, "utterance-1"});
    Check(TaskManager::CancelByActor(0x1234) == 1, "actor scope must cancel its task");
    Check(TaskManager::CancelByTurn(77) == 1, "turn scope must cancel its task");
    Check(TaskManager::CancelByUtterance("utterance-1") == 1, "utterance scope must cancel its task");
    Check(WaitUntil([&] { return cancelledCompletions.load() == 3; }),
        "all scoped cancellations must complete exactly once");
    release = true;
    TaskManager::CancelByType("scope_blocker");
}

void TestShutdownCompletesPendingExactlyOnce() {
    std::atomic_bool blockerStarted{false};
    auto blocker = Options("shutdown_blocker", TaskManager::Lane::Background);
    TaskManager::Submit(std::move(blocker), [&](const TaskManager::CancellationToken& token) {
        blockerStarted = true;
        while (token.WaitFor(10ms)) {}
    });
    Check(WaitUntil([&] { return blockerStarted.load(); }), "shutdown blocker must start");

    std::atomic_int pendingRan{0};
    std::atomic_int pendingCompletions{0};
    auto pending = Options("shutdown_pending", TaskManager::Lane::Background);
    TaskManager::Submit(std::move(pending), [&](const TaskManager::CancellationToken&) { ++pendingRan; },
        [&](bool succeeded, const char* reason) {
            if (!succeeded && reason && std::string(reason) == "shutdown") ++pendingCompletions;
        });
    TaskManager::Shutdown();
    Check(pendingRan.load() == 0, "pending work must not run during shutdown");
    Check(pendingCompletions.load() == 1, "shutdown must complete pending work exactly once");
}

void TestRestartAndBoundedAdmission() {
    Check(TaskManager::Initialize(4, 32), "TaskManager must restart after joined shutdown");
    std::atomic_bool blockerStarted{false};
    auto blocker = Options("admission_blocker", TaskManager::Lane::Background);
    TaskManager::Submit(std::move(blocker), [&](const TaskManager::CancellationToken& token) {
        blockerStarted = true;
        while (token.WaitFor(10ms)) {}
    });
    Check(WaitUntil([&] { return blockerStarted.load(); }), "admission blocker must start");
    for (int index = 0; index < 32; ++index) {
        auto pending = Options(("admission_" + std::to_string(index)).c_str(), TaskManager::Lane::Background);
        Check(static_cast<bool>(TaskManager::Submit(std::move(pending), [](const TaskManager::CancellationToken&) {})),
            "queue must accept work through its configured bound");
    }
    auto overflow = Options("admission_overflow", TaskManager::Lane::Background);
    Check(!TaskManager::Submit(std::move(overflow), [](const TaskManager::CancellationToken&) {}),
        "queue must reject work beyond its configured bound");
    TaskManager::CancelAll();
    TaskManager::Shutdown();
}

} // namespace

int main() {
    Check(TaskManager::Initialize(8, 512), "TaskManager must initialize");
    TestInteractiveLaneIsNotStarved();
    TestAudioLaneIsReserved();
    TestPendingCoalescing();
    TestCancellationById();
    TestDeadlineInterruptsBlockingWork();
    TestPendingDeadlineExpiresInQueue();
    TestExceptionDiagnostics();
    TestMixedWorkloadCompletesExactlyOnce();
    TestScopedCancellation();
    TaskManager::CancelAll();
    TestShutdownCompletesPendingExactlyOnce();
    TestRestartAndBoundedAdmission();
    if (g_failures == 0) std::printf("TaskManager tests passed\n");
    return g_failures == 0 ? 0 : 1;
}
