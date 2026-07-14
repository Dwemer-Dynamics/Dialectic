#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace TaskManager {

enum class Lane : std::uint8_t {
    Interactive = 0,
    Audio = 1,
    Gameplay = 2,
    Compute = 3,
    Background = 4,
};

enum class CoalescingPolicy : std::uint8_t {
    None = 0,
    ReplacePending = 1,
    RejectIfPendingOrActive = 2,
};

struct CancellationState;

class CancellationToken {
public:
    CancellationToken(std::shared_ptr<CancellationState> state,
                      std::chrono::steady_clock::time_point deadline = {});
    bool IsCancellationRequested() const;
    bool IsTimedOut() const;
    bool WaitFor(std::chrono::milliseconds duration) const;
    void SetInterrupt(std::function<void()> interrupt) const;
    void ClearInterrupt() const;

private:
    std::shared_ptr<CancellationState> state_;
    std::chrono::steady_clock::time_point deadline_{};
};

struct Scope {
    std::uint32_t actorFormId{0};
    std::uint64_t turnId{0};
    std::string utteranceId;
};

struct Options {
    std::string type;
    std::string key;
    std::uint64_t generation{0};
    Scope scope;
    Lane lane{Lane::Gameplay};
    bool priority{false};
    bool deadlineFromEnqueue{false};
    std::chrono::milliseconds timeout{0};
    CoalescingPolicy coalescing{CoalescingPolicy::None};
    std::size_t concurrencyLimit{0};
};

struct TaskHandle {
    std::uint64_t id{0};
    explicit operator bool() const { return id != 0; }
    bool Cancel() const;
};

struct Status {
    std::size_t workers{0};
    std::size_t pending{0};
    std::size_t active{0};
    std::uint64_t queued{0};
    std::uint64_t completed{0};
    std::uint64_t cancelled{0};
    std::uint64_t timedOut{0};
    std::uint64_t rejected{0};
    std::uint64_t errors{0};
    std::uint64_t oldestPendingMs{0};
    std::uint64_t uptimeMs{0};
};

struct TypeStatus {
    std::string type;
    Lane lane{Lane::Gameplay};
    std::size_t pending{0};
    std::size_t active{0};
    std::uint64_t queued{0};
    std::uint64_t completed{0};
    std::uint64_t cancelled{0};
    std::uint64_t timedOut{0};
    std::uint64_t rejected{0};
    std::uint64_t errors{0};
    std::uint64_t averageDurationMs{0};
    std::uint64_t maximumDurationMs{0};
    std::uint64_t oldestPendingMs{0};
    std::string lastError;
    std::string lastErrorKey;
};

struct WorkerStatus {
    std::size_t index{0};
    bool active{false};
    std::uint64_t taskId{0};
    std::string type;
    std::string key;
    Lane lane{Lane::Gameplay};
    std::uint64_t runningMs{0};
    std::uint64_t completed{0};
    std::uint64_t errors{0};
    std::uint64_t longestTaskMs{0};
    std::string longestTaskType;
    std::string lastError;
};

struct Snapshot {
    Status totals;
    std::vector<TypeStatus> types;
    std::vector<WorkerStatus> workers;
};

using Work = std::function<void(const CancellationToken&)>;
using Completion = std::function<void(bool, const char*)>;

bool Initialize(std::size_t workerCount = 8, std::size_t maxPending = 512);
void Shutdown();
TaskHandle Submit(Options options, Work work, Completion completion = {});

// Compatibility wrappers for existing call sites. New work should use Submit.
std::uint64_t Enqueue(std::string type,
    std::string key,
    std::uint64_t generation,
    bool priority,
    std::chrono::milliseconds timeout,
    Work work,
    Completion completion = {});
std::uint64_t EnqueueScoped(std::string type,
    std::string key,
    std::uint64_t generation,
    bool priority,
    std::chrono::milliseconds timeout,
    Scope scope,
    Work work,
    Completion completion = {});

bool CancelById(std::uint64_t taskId);
std::size_t CancelByType(const std::string& type);
std::size_t CancelByTypePrefix(const std::string& prefix);
std::size_t CancelByKey(const std::string& key);
std::size_t CancelByActor(std::uint32_t actorFormId);
std::size_t CancelByTurn(std::uint64_t turnId);
std::size_t CancelByUtterance(const std::string& utteranceId);
std::size_t CancelOlderThanGeneration(std::uint64_t generation);
void CancelAll();
Status GetStatus();
Snapshot GetSnapshot();
const CancellationToken* CurrentToken();
const char* LaneName(Lane lane);

} // namespace TaskManager
