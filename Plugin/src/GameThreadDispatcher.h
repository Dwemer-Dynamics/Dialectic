#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace GameThreadDispatcher {

struct Status {
    std::size_t pending{0};
    std::uint64_t queued{0};
    std::uint64_t executed{0};
    std::uint64_t cancelled{0};
    std::uint64_t stale{0};
    std::uint64_t rejected{0};
};

bool Initialize(std::size_t maxPending = 512);
void Shutdown();
bool Enqueue(std::string type,
    std::string key,
    std::uint64_t generation,
    std::function<void()> execute,
    std::function<void(const char*)> dropped = {});
// Reserved admission for task completions and other cleanup that must reach the
// game thread even when ordinary commands have filled the primary queue.
bool EnqueueCritical(std::string type,
    std::string key,
    std::uint64_t generation,
    std::function<void()> execute,
    std::function<void(const char*)> dropped = {});
std::size_t Pump(std::uint64_t activeGeneration, std::size_t maxCommands = 64);
std::size_t CancelByType(const std::string& type, const char* reason);
std::size_t CancelByKey(const std::string& key, const char* reason);
void CancelAll(const char* reason);
Status GetStatus();
bool IsGameThread();

} // namespace GameThreadDispatcher
