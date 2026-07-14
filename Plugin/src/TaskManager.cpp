#include "TaskManager.h"

#include "GameThreadDispatcher.h"
#include "Logger.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <deque>
#include <exception>
#include <limits>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

namespace TaskManager {

struct CancellationState {
    std::atomic_bool cancelled{false};
    mutable std::mutex mutex;
    std::condition_variable condition;
    std::function<void()> interrupt;
};

namespace {

constexpr std::size_t kLaneCount = 5;

struct Task {
    std::uint64_t id{0};
    Options options;
    std::shared_ptr<CancellationState> cancellation;
    Work work;
    Completion completion;
    std::chrono::steady_clock::time_point enqueuedAt{};
    std::chrono::steady_clock::time_point startedAt{};
    std::chrono::steady_clock::time_point deadline{};
    bool timeoutRecorded{false};
};

struct InternalTypeMetrics {
    Lane lane{Lane::Gameplay};
    std::uint64_t queued{0};
    std::uint64_t completed{0};
    std::uint64_t cancelled{0};
    std::uint64_t timedOut{0};
    std::uint64_t rejected{0};
    std::uint64_t errors{0};
    std::uint64_t totalDurationMs{0};
    std::uint64_t maximumDurationMs{0};
    std::string lastError;
    std::string lastErrorKey;
};

struct InternalWorkerMetrics {
    bool active{false};
    std::uint64_t taskId{0};
    std::string type;
    std::string key;
    Lane lane{Lane::Gameplay};
    std::chrono::steady_clock::time_point startedAt{};
    std::uint64_t completed{0};
    std::uint64_t errors{0};
    std::uint64_t longestTaskMs{0};
    std::string longestTaskType;
    std::string lastError;
};

std::mutex g_mutex;
std::condition_variable g_condition;
std::deque<Task> g_pending;
std::unordered_map<std::uint64_t, Task> g_active;
std::vector<std::thread> g_workers;
std::thread g_watchdog;
std::vector<InternalWorkerMetrics> g_workerMetrics;
std::unordered_map<std::string, InternalTypeMetrics> g_typeMetrics;
std::array<std::size_t, kLaneCount> g_activeByLane{};
std::unordered_map<std::string, std::size_t> g_activeByType;
std::size_t g_maxPending = 512;
bool g_stopping = false;
std::atomic<std::uint64_t> g_nextId{0};
std::atomic<std::uint64_t> g_queued{0};
std::atomic<std::uint64_t> g_completed{0};
std::atomic<std::uint64_t> g_cancelled{0};
std::atomic<std::uint64_t> g_timedOut{0};
std::atomic<std::uint64_t> g_rejected{0};
std::atomic<std::uint64_t> g_errors{0};
thread_local const CancellationToken* g_currentToken = nullptr;
std::chrono::steady_clock::time_point g_startedAt{};
std::chrono::steady_clock::time_point g_lastQueueFullWarning{};

std::size_t LaneIndex(Lane lane) {
    return static_cast<std::size_t>(lane);
}

std::size_t LaneLimit(Lane lane, std::size_t workers) {
    switch (lane) {
        case Lane::Interactive: return std::max<std::size_t>(2, workers / 2);
        case Lane::Audio: return std::min<std::size_t>(2, workers);
        case Lane::Gameplay: return std::min<std::size_t>(2, workers);
        case Lane::Compute: return 1;
        case Lane::Background: return 1;
    }
    return workers;
}

int LaneScore(Lane lane) {
    switch (lane) {
        case Lane::Interactive: return 500;
        case Lane::Audio: return 400;
        case Lane::Gameplay: return 300;
        case Lane::Compute: return 200;
        case Lane::Background: return 100;
    }
    return 0;
}

Lane InferLane(const std::string& type, const std::string& key) {
    if (type.rfind("http:", 0) == 0) return Lane::Interactive;
    if (type == "audio_prepare" || type == "stt_upload") return Lane::Audio;
    if (type == "spatial_navgraph") return Lane::Compute;
    if (type == "voice_sample" || type == "voice_sample_batch" || type == "import_detection" ||
        type == "runtime_status" || key == "world_data") return Lane::Background;
    return Lane::Gameplay;
}

bool RequestCancellation(const std::shared_ptr<CancellationState>& state) {
    if (!state || state->cancelled.exchange(true, std::memory_order_acq_rel)) {
        return false;
    }
    std::function<void()> interrupt;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        interrupt = state->interrupt;
    }
    state->condition.notify_all();
    if (interrupt) {
        try {
            interrupt();
        } catch (...) {
            Logger::LogWarning("TaskManager: cancellation interrupt threw");
        }
    }
    return true;
}

bool IsTypeAtLimit(const Task& task) {
    const std::size_t limit = task.options.concurrencyLimit;
    if (limit == 0) return false;
    const auto it = g_activeByType.find(task.options.type);
    return it != g_activeByType.end() && it->second >= limit;
}

bool CanRun(const Task& task) {
    const std::size_t laneIndex = LaneIndex(task.options.lane);
    if (laneIndex >= g_activeByLane.size()) return false;
    if (g_activeByLane[laneIndex] >= LaneLimit(task.options.lane, g_workers.size())) return false;
    return !IsTypeAtLimit(task);
}

std::deque<Task>::iterator SelectNextTask(std::chrono::steady_clock::time_point now) {
    auto selected = g_pending.end();
    long long selectedScore = std::numeric_limits<long long>::min();
    for (auto it = g_pending.begin(); it != g_pending.end(); ++it) {
        if (!CanRun(*it)) continue;
        const auto ageMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->enqueuedAt).count();
        const long long ageBoost = std::min<long long>(700, std::max<long long>(0, ageMs / 100));
        const long long score = (it->options.priority ? 1000 : 0) + LaneScore(it->options.lane) + ageBoost;
        if (selected == g_pending.end() || score > selectedScore) {
            selected = it;
            selectedScore = score;
        }
    }
    return selected;
}

void DispatchCompletion(Task& task, bool succeeded, const char* reason) {
    if (!task.completion) return;
    auto completion = std::move(task.completion);
    const std::string key = task.options.key;
    const std::uint64_t generation = task.options.generation;
    GameThreadDispatcher::EnqueueCritical("task_completion", key, generation,
        [completion, succeeded, reasonText = std::string(reason ? reason : "completed")]() {
            completion(succeeded, reasonText.c_str());
        },
        [completion](const char* dropReason) {
            completion(false, dropReason ? dropReason : "completion_dropped");
        });
}

void WorkerLoop(std::size_t workerIndex) {
    for (;;) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(g_mutex);
            g_condition.wait(lock, [] {
                if (g_stopping && g_pending.empty()) return true;
                const auto now = std::chrono::steady_clock::now();
                return SelectNextTask(now) != g_pending.end();
            });
            if (g_stopping && g_pending.empty()) return;
            const auto now = std::chrono::steady_clock::now();
            auto it = SelectNextTask(now);
            if (it == g_pending.end()) continue;
            task = std::move(*it);
            g_pending.erase(it);
            task.startedAt = now;
            if (task.options.timeout.count() > 0 && !task.options.deadlineFromEnqueue) {
                task.deadline = now + task.options.timeout;
            }
            g_active.emplace(task.id, task);
            ++g_activeByLane[LaneIndex(task.options.lane)];
            ++g_activeByType[task.options.type];
            InternalWorkerMetrics& worker = g_workerMetrics[workerIndex];
            worker.active = true;
            worker.taskId = task.id;
            worker.type = task.options.type;
            worker.key = task.options.key;
            worker.lane = task.options.lane;
            worker.startedAt = now;
        }

        bool succeeded = false;
        bool threw = false;
        std::string errorText;
        const char* completionReason = "cancelled";
        const CancellationToken token(task.cancellation, task.deadline);
        g_currentToken = &token;
        if (!token.IsCancellationRequested()) {
            try {
                task.work(token);
                succeeded = !token.IsCancellationRequested();
                completionReason = succeeded ? "completed" : (token.IsTimedOut() ? "timeout" : "cancelled");
            } catch (const std::exception& exception) {
                threw = true;
                errorText = exception.what();
                completionReason = "exception";
                ++g_errors;
                Logger::LogError("TaskManager: worker=%zu task=%llu type=%s key=%s threw: %s",
                    workerIndex,
                    static_cast<unsigned long long>(task.id),
                    task.options.type.c_str(),
                    task.options.key.c_str(),
                    errorText.c_str());
            } catch (...) {
                threw = true;
                errorText = "unknown exception";
                completionReason = "exception";
                ++g_errors;
                Logger::LogError("TaskManager: worker=%zu task=%llu type=%s key=%s threw",
                    workerIndex,
                    static_cast<unsigned long long>(task.id),
                    task.options.type.c_str(),
                    task.options.key.c_str());
            }
        } else if (token.IsTimedOut()) {
            completionReason = "timeout";
        }

        const auto finishedAt = std::chrono::steady_clock::now();
        const auto elapsedMs = static_cast<std::uint64_t>(std::max<long long>(0,
            std::chrono::duration_cast<std::chrono::milliseconds>(finishedAt - task.startedAt).count()));
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto activeIt = g_active.find(task.id);
            const bool timeoutRecorded = activeIt != g_active.end() && activeIt->second.timeoutRecorded;
            if (token.IsTimedOut() && !timeoutRecorded) {
                ++g_timedOut;
                ++g_typeMetrics[task.options.type].timedOut;
            }
            g_active.erase(task.id);
            if (g_activeByLane[LaneIndex(task.options.lane)] > 0) --g_activeByLane[LaneIndex(task.options.lane)];
            auto activeTypeIt = g_activeByType.find(task.options.type);
            if (activeTypeIt != g_activeByType.end() && activeTypeIt->second > 0) --activeTypeIt->second;
            InternalTypeMetrics& type = g_typeMetrics[task.options.type];
            ++type.completed;
            if (threw) {
                ++type.errors;
                type.lastError = errorText;
                type.lastErrorKey = task.options.key;
            }
            type.totalDurationMs += elapsedMs;
            type.maximumDurationMs = std::max(type.maximumDurationMs, elapsedMs);
            InternalWorkerMetrics& worker = g_workerMetrics[workerIndex];
            worker.active = false;
            worker.taskId = 0;
            worker.type.clear();
            worker.key.clear();
            ++worker.completed;
            if (threw) {
                ++worker.errors;
                worker.lastError = errorText;
            }
            if (elapsedMs > worker.longestTaskMs) {
                worker.longestTaskMs = elapsedMs;
                worker.longestTaskType = task.options.type;
            }
        }
        token.ClearInterrupt();
        g_currentToken = nullptr;
        DispatchCompletion(task, succeeded, completionReason);
        ++g_completed;
        g_condition.notify_all();
    }
}

void WatchdogLoop() {
    for (;;) {
        std::vector<Task> pendingTimedOut;
        std::vector<std::shared_ptr<CancellationState>> activeTimedOut;
        {
            std::unique_lock<std::mutex> lock(g_mutex);
            g_condition.wait_for(lock, std::chrono::milliseconds(50), [] { return g_stopping; });
            if (g_stopping) return;
            const auto now = std::chrono::steady_clock::now();
            for (auto it = g_pending.begin(); it != g_pending.end();) {
                if (it->deadline.time_since_epoch().count() == 0 || now < it->deadline) {
                    ++it;
                    continue;
                }
                ++g_timedOut;
                ++g_typeMetrics[it->options.type].timedOut;
                Logger::LogWarning("TaskManager: pending task=%llu type=%s key=%s reached queue deadline",
                    static_cast<unsigned long long>(it->id), it->options.type.c_str(), it->options.key.c_str());
                pendingTimedOut.push_back(std::move(*it));
                it = g_pending.erase(it);
            }
            for (auto& entry : g_active) {
                Task& task = entry.second;
                if (task.cancellation->cancelled.load(std::memory_order_acquire) ||
                    task.deadline.time_since_epoch().count() == 0 || task.timeoutRecorded || now < task.deadline) continue;
                task.timeoutRecorded = true;
                ++g_timedOut;
                ++g_typeMetrics[task.options.type].timedOut;
                activeTimedOut.push_back(task.cancellation);
                Logger::LogWarning("TaskManager: task=%llu type=%s key=%s reached deadline",
                    static_cast<unsigned long long>(task.id), task.options.type.c_str(), task.options.key.c_str());
            }
        }
        for (Task& task : pendingTimedOut) {
            RequestCancellation(task.cancellation);
            DispatchCompletion(task, false, "timeout");
        }
        for (const auto& state : activeTimedOut) RequestCancellation(state);
        if (!pendingTimedOut.empty()) g_condition.notify_all();
    }
}

template <class Predicate>
std::size_t CancelMatching(Predicate predicate, const char* reason) {
    std::size_t count = 0;
    std::vector<Task> removedPending;
    std::vector<std::pair<std::string, std::shared_ptr<CancellationState>>> activeStates;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto it = g_pending.begin(); it != g_pending.end();) {
            if (predicate(*it)) {
                removedPending.push_back(std::move(*it));
                it = g_pending.erase(it);
                ++count;
            } else {
                ++it;
            }
        }
        for (auto& entry : g_active) {
            Task& task = entry.second;
            if (predicate(task) && !task.cancellation->cancelled.load(std::memory_order_acquire)) {
                activeStates.emplace_back(task.options.type, task.cancellation);
                ++count;
            }
        }
    }
    for (Task& task : removedPending) {
        RequestCancellation(task.cancellation);
        ++g_cancelled;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            ++g_typeMetrics[task.options.type].cancelled;
        }
        DispatchCompletion(task, false, reason);
    }
    for (const auto& entry : activeStates) {
        if (!RequestCancellation(entry.second)) {
            --count;
            continue;
        }
        ++g_cancelled;
        std::lock_guard<std::mutex> lock(g_mutex);
        ++g_typeMetrics[entry.first].cancelled;
    }
    g_condition.notify_all();
    return count;
}

} // namespace

CancellationToken::CancellationToken(std::shared_ptr<CancellationState> state,
                                     std::chrono::steady_clock::time_point deadline)
    : state_(std::move(state)), deadline_(deadline) {}

bool CancellationToken::IsCancellationRequested() const {
    if (!state_ || state_->cancelled.load(std::memory_order_acquire)) return true;
    if (IsTimedOut()) {
        RequestCancellation(state_);
        return true;
    }
    return false;
}

bool CancellationToken::IsTimedOut() const {
    return deadline_.time_since_epoch().count() != 0 && std::chrono::steady_clock::now() >= deadline_;
}

bool CancellationToken::WaitFor(std::chrono::milliseconds duration) const {
    if (!state_ || IsCancellationRequested()) return false;
    if (deadline_.time_since_epoch().count() != 0) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline_ - std::chrono::steady_clock::now());
        duration = std::min(duration, std::max(std::chrono::milliseconds(0), remaining));
    }
    std::unique_lock<std::mutex> lock(state_->mutex);
    state_->condition.wait_for(lock, duration, [this] {
        return !state_ || state_->cancelled.load(std::memory_order_acquire);
    });
    lock.unlock();
    return !IsCancellationRequested();
}

void CancellationToken::SetInterrupt(std::function<void()> interrupt) const {
    if (!state_) return;
    bool invokeNow = false;
    std::function<void()> invoke;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->interrupt = std::move(interrupt);
        invokeNow = state_->cancelled.load(std::memory_order_acquire);
        if (invokeNow) invoke = state_->interrupt;
    }
    if (invokeNow && invoke) invoke();
}

void CancellationToken::ClearInterrupt() const {
    if (!state_) return;
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->interrupt = {};
}

bool TaskHandle::Cancel() const {
    return id != 0 && CancelById(id);
}

const char* LaneName(Lane lane) {
    switch (lane) {
        case Lane::Interactive: return "interactive";
        case Lane::Audio: return "audio";
        case Lane::Gameplay: return "gameplay";
        case Lane::Compute: return "compute";
        case Lane::Background: return "background";
    }
    return "unknown";
}

bool Initialize(std::size_t workerCount, std::size_t maxPending) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_workers.empty()) return true;
    g_stopping = false;
    g_maxPending = std::max<std::size_t>(32, maxPending);
    workerCount = std::clamp<std::size_t>(workerCount, 4, 16);
    g_pending.clear();
    g_active.clear();
    g_typeMetrics.clear();
    g_activeByType.clear();
    g_queued.store(0);
    g_completed.store(0);
    g_cancelled.store(0);
    g_timedOut.store(0);
    g_rejected.store(0);
    g_errors.store(0);
    g_activeByLane.fill(0);
    g_startedAt = std::chrono::steady_clock::now();
    g_lastQueueFullWarning = {};
    g_workerMetrics.assign(workerCount, {});
    for (std::size_t index = 0; index < workerCount; ++index) g_workers.emplace_back(WorkerLoop, index);
    g_watchdog = std::thread(WatchdogLoop);
    Logger::LogInfo("TaskManager: initialized workers=%zu max_pending=%zu lanes=interactive,audio,gameplay,compute,background",
        workerCount, g_maxPending);
    return true;
}

void Shutdown() {
    std::vector<Task> pending;
    std::vector<std::pair<std::string, std::shared_ptr<CancellationState>>> activeStates;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_workers.empty()) return;
        g_stopping = true;
        while (!g_pending.empty()) {
            Task task = std::move(g_pending.front());
            g_pending.pop_front();
            ++g_cancelled;
            ++g_typeMetrics[task.options.type].cancelled;
            pending.push_back(std::move(task));
        }
        for (auto& entry : g_active) {
            Task& task = entry.second;
            if (task.cancellation->cancelled.load(std::memory_order_acquire)) continue;
            activeStates.emplace_back(task.options.type, task.cancellation);
        }
    }
    for (Task& task : pending) {
        RequestCancellation(task.cancellation);
        DispatchCompletion(task, false, "shutdown");
    }
    for (const auto& entry : activeStates) {
        if (!RequestCancellation(entry.second)) continue;
        ++g_cancelled;
        std::lock_guard<std::mutex> lock(g_mutex);
        ++g_typeMetrics[entry.first].cancelled;
    }
    g_condition.notify_all();
    if (g_watchdog.joinable()) g_watchdog.join();
    for (std::thread& worker : g_workers) if (worker.joinable()) worker.join();
    std::lock_guard<std::mutex> lock(g_mutex);
    g_workers.clear();
    g_pending.clear();
    g_active.clear();
    g_workerMetrics.clear();
    g_activeByLane.fill(0);
    g_activeByType.clear();
    g_stopping = false;
}

TaskHandle Submit(Options options, Work work, Completion completion) {
    if (!work || options.type.empty()) {
        ++g_rejected;
        return {};
    }
    Task task;
    task.id = g_nextId.fetch_add(1) + 1;
    const std::uint64_t taskId = task.id;
    task.options = std::move(options);
    task.cancellation = std::make_shared<CancellationState>();
    task.work = std::move(work);
    task.completion = std::move(completion);
    task.enqueuedAt = std::chrono::steady_clock::now();
    if (task.options.timeout.count() > 0 && task.options.deadlineFromEnqueue) {
        task.deadline = task.enqueuedAt + task.options.timeout;
    }
    std::vector<Task> replaced;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        InternalTypeMetrics& metrics = g_typeMetrics[task.options.type];
        metrics.lane = task.options.lane;
        if (g_stopping || g_workers.empty() || g_pending.size() >= g_maxPending) {
            ++g_rejected;
            ++metrics.rejected;
            const auto now = std::chrono::steady_clock::now();
            if (g_pending.size() >= g_maxPending &&
                (g_lastQueueFullWarning.time_since_epoch().count() == 0 ||
                 now - g_lastQueueFullWarning >= std::chrono::seconds(30))) {
                g_lastQueueFullWarning = now;
                Logger::LogWarning(
                    "TaskManager: queue full pending=%zu max=%zu rejected_type=%s key=%s",
                    g_pending.size(), g_maxPending, task.options.type.c_str(), task.options.key.c_str());
            }
            return {};
        }
        if (task.options.coalescing == CoalescingPolicy::RejectIfPendingOrActive) {
            const bool duplicatePending = std::any_of(g_pending.begin(), g_pending.end(), [&](const Task& candidate) {
                return candidate.options.type == task.options.type && candidate.options.key == task.options.key;
            });
            const bool duplicateActive = std::any_of(g_active.begin(), g_active.end(), [&](const auto& entry) {
                return entry.second.options.type == task.options.type && entry.second.options.key == task.options.key;
            });
            if (duplicatePending || duplicateActive) {
                ++g_rejected;
                ++metrics.rejected;
                return {};
            }
        } else if (task.options.coalescing == CoalescingPolicy::ReplacePending) {
            for (auto it = g_pending.begin(); it != g_pending.end();) {
                if (it->options.type == task.options.type && it->options.key == task.options.key) {
                    replaced.push_back(std::move(*it));
                    it = g_pending.erase(it);
                } else {
                    ++it;
                }
            }
        }
        ++metrics.queued;
        g_pending.push_back(std::move(task));
    }
    for (Task& replacedTask : replaced) {
        RequestCancellation(replacedTask.cancellation);
        ++g_cancelled;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            ++g_typeMetrics[replacedTask.options.type].cancelled;
        }
        DispatchCompletion(replacedTask, false, "coalesced");
    }
    ++g_queued;
    g_condition.notify_all();
    return {taskId};
}

std::uint64_t Enqueue(std::string type, std::string key, std::uint64_t generation, bool priority,
                      std::chrono::milliseconds timeout, Work work, Completion completion) {
    Options options;
    options.type = std::move(type);
    options.key = std::move(key);
    options.generation = generation;
    options.priority = priority;
    options.timeout = timeout;
    options.lane = InferLane(options.type, options.key);
    options.deadlineFromEnqueue = options.lane == Lane::Interactive || options.lane == Lane::Audio;
    if (options.type == "gamedata" || options.type == "runtime_status") {
        options.coalescing = CoalescingPolicy::ReplacePending;
    }
    return Submit(std::move(options), std::move(work), std::move(completion)).id;
}

std::uint64_t EnqueueScoped(std::string type, std::string key, std::uint64_t generation, bool priority,
                            std::chrono::milliseconds timeout, Scope scope, Work work, Completion completion) {
    Options options;
    options.type = std::move(type);
    options.key = std::move(key);
    options.generation = generation;
    options.scope = std::move(scope);
    options.priority = priority;
    options.timeout = timeout;
    options.lane = InferLane(options.type, options.key);
    options.deadlineFromEnqueue = options.lane == Lane::Interactive || options.lane == Lane::Audio;
    return Submit(std::move(options), std::move(work), std::move(completion)).id;
}

bool CancelById(std::uint64_t taskId) {
    if (taskId == 0) return false;
    return CancelMatching([&](const Task& task) { return task.id == taskId; }, "cancelled_id") > 0;
}

std::size_t CancelByType(const std::string& type) {
    return CancelMatching([&](const Task& task) { return task.options.type == type; }, "cancelled_type");
}

std::size_t CancelByTypePrefix(const std::string& prefix) {
    return CancelMatching([&](const Task& task) {
        return task.options.type.size() >= prefix.size() && task.options.type.compare(0, prefix.size(), prefix) == 0;
    }, "cancelled_type_prefix");
}

std::size_t CancelByKey(const std::string& key) {
    return CancelMatching([&](const Task& task) { return task.options.key == key; }, "cancelled_key");
}

std::size_t CancelByActor(std::uint32_t actorFormId) {
    if (actorFormId == 0) return 0;
    return CancelMatching([&](const Task& task) { return task.options.scope.actorFormId == actorFormId; }, "cancelled_actor");
}

std::size_t CancelByTurn(std::uint64_t turnId) {
    if (turnId == 0) return 0;
    return CancelMatching([&](const Task& task) { return task.options.scope.turnId == turnId; }, "cancelled_turn");
}

std::size_t CancelByUtterance(const std::string& utteranceId) {
    if (utteranceId.empty()) return 0;
    return CancelMatching([&](const Task& task) { return task.options.scope.utteranceId == utteranceId; }, "cancelled_utterance");
}

std::size_t CancelOlderThanGeneration(std::uint64_t generation) {
    return CancelMatching([&](const Task& task) {
        return task.options.generation != 0 && task.options.generation < generation;
    }, "stale_generation");
}

void CancelAll() {
    CancelMatching([](const Task&) { return true; }, "cancelled_all");
}

Snapshot GetSnapshot() {
    Snapshot snapshot;
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(g_mutex);
    snapshot.totals.workers = g_workers.size();
    snapshot.totals.pending = g_pending.size();
    snapshot.totals.active = g_active.size();
    snapshot.totals.queued = g_queued.load();
    snapshot.totals.completed = g_completed.load();
    snapshot.totals.cancelled = g_cancelled.load();
    snapshot.totals.timedOut = g_timedOut.load();
    snapshot.totals.rejected = g_rejected.load();
    snapshot.totals.errors = g_errors.load();
    snapshot.totals.uptimeMs = g_startedAt.time_since_epoch().count() == 0 ? 0 :
        static_cast<std::uint64_t>(std::max<long long>(0,
            std::chrono::duration_cast<std::chrono::milliseconds>(now - g_startedAt).count()));
    for (const Task& task : g_pending) {
        const auto age = static_cast<std::uint64_t>(std::max<long long>(0,
            std::chrono::duration_cast<std::chrono::milliseconds>(now - task.enqueuedAt).count()));
        snapshot.totals.oldestPendingMs = std::max(snapshot.totals.oldestPendingMs, age);
    }
    for (const auto& entry : g_typeMetrics) {
        TypeStatus type;
        type.type = entry.first;
        type.lane = entry.second.lane;
        type.queued = entry.second.queued;
        type.completed = entry.second.completed;
        type.cancelled = entry.second.cancelled;
        type.timedOut = entry.second.timedOut;
        type.rejected = entry.second.rejected;
        type.errors = entry.second.errors;
        type.averageDurationMs = entry.second.completed > 0 ? entry.second.totalDurationMs / entry.second.completed : 0;
        type.maximumDurationMs = entry.second.maximumDurationMs;
        type.lastError = entry.second.lastError;
        type.lastErrorKey = entry.second.lastErrorKey;
        for (const Task& task : g_pending) {
            if (task.options.type != type.type) continue;
            ++type.pending;
            const auto age = static_cast<std::uint64_t>(std::max<long long>(0,
                std::chrono::duration_cast<std::chrono::milliseconds>(now - task.enqueuedAt).count()));
            type.oldestPendingMs = std::max(type.oldestPendingMs, age);
        }
        const auto activeIt = g_activeByType.find(type.type);
        if (activeIt != g_activeByType.end()) type.active = activeIt->second;
        snapshot.types.push_back(std::move(type));
    }
    std::sort(snapshot.types.begin(), snapshot.types.end(), [](const TypeStatus& left, const TypeStatus& right) {
        return left.type < right.type;
    });
    snapshot.workers.reserve(g_workerMetrics.size());
    for (std::size_t index = 0; index < g_workerMetrics.size(); ++index) {
        const InternalWorkerMetrics& source = g_workerMetrics[index];
        WorkerStatus worker;
        worker.index = index;
        worker.active = source.active;
        worker.taskId = source.taskId;
        worker.type = source.type;
        worker.key = source.key;
        worker.lane = source.lane;
        worker.runningMs = source.active ? static_cast<std::uint64_t>(std::max<long long>(0,
            std::chrono::duration_cast<std::chrono::milliseconds>(now - source.startedAt).count())) : 0;
        worker.completed = source.completed;
        worker.errors = source.errors;
        worker.longestTaskMs = source.longestTaskMs;
        worker.longestTaskType = source.longestTaskType;
        worker.lastError = source.lastError;
        snapshot.workers.push_back(std::move(worker));
    }
    return snapshot;
}

Status GetStatus() {
    return GetSnapshot().totals;
}

const CancellationToken* CurrentToken() {
    return g_currentToken;
}

} // namespace TaskManager
