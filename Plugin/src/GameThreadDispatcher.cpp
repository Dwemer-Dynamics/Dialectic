#include "GameThreadDispatcher.h"

#include "Logger.h"

#include <algorithm>
#include <atomic>
#include <deque>
#include <iterator>
#include <mutex>
#include <thread>
#include <vector>

namespace GameThreadDispatcher {
namespace {

struct Command {
    std::string type;
    std::string key;
    std::uint64_t generation{0};
    std::function<void()> execute;
    std::function<void(const char*)> dropped;
};

std::mutex g_mutex;
std::deque<Command> g_commands;
std::size_t g_maxPending = 512;
constexpr std::size_t kMinimumCriticalReserve = 128;
std::thread::id g_gameThreadId;
std::atomic<bool> g_initialized{false};
std::atomic<std::uint64_t> g_queued{0};
std::atomic<std::uint64_t> g_executed{0};
std::atomic<std::uint64_t> g_cancelled{0};
std::atomic<std::uint64_t> g_stale{0};
std::atomic<std::uint64_t> g_rejected{0};

void DropCommands(std::vector<Command>& commands, const char* reason) {
    for (Command& command : commands) {
        if (command.dropped) {
            command.dropped(reason);
        }
    }
}

} // namespace

bool Initialize(std::size_t maxPending) {
    g_maxPending = std::max<std::size_t>(32, maxPending);
    g_initialized.store(true, std::memory_order_release);
    Logger::LogInfo("GameThreadDispatcher: initialized max_pending=%zu", g_maxPending);
    return true;
}

void Shutdown() {
    CancelAll("shutdown");
    g_initialized.store(false, std::memory_order_release);
    g_gameThreadId = {};
}

bool EnqueueImpl(bool critical,
    std::string type,
    std::string key,
    std::uint64_t generation,
    std::function<void()> execute,
    std::function<void(const char*)> dropped) {
    if (!g_initialized.load(std::memory_order_acquire) || !execute) {
        ++g_rejected;
        if (dropped) {
            dropped("dispatcher_unavailable");
        }
        return false;
    }

    bool queueFull = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const std::size_t criticalReserve = std::max(kMinimumCriticalReserve, g_maxPending + 64);
        const std::size_t admissionLimit = critical ? g_maxPending + criticalReserve : g_maxPending;
        if (g_commands.size() >= admissionLimit) {
            ++g_rejected;
            queueFull = true;
        } else {
            Command command{std::move(type), std::move(key), generation, std::move(execute), std::move(dropped)};
            g_commands.push_back(std::move(command));
            ++g_queued;
        }
    }
    if (queueFull) {
        if (dropped) dropped("queue_full");
        return false;
    }
    return true;
}

bool Enqueue(std::string type,
    std::string key,
    std::uint64_t generation,
    std::function<void()> execute,
    std::function<void(const char*)> dropped) {
    return EnqueueImpl(false, std::move(type), std::move(key), generation,
        std::move(execute), std::move(dropped));
}

bool EnqueueCritical(std::string type,
    std::string key,
    std::uint64_t generation,
    std::function<void()> execute,
    std::function<void(const char*)> dropped) {
    return EnqueueImpl(true, std::move(type), std::move(key), generation,
        std::move(execute), std::move(dropped));
}

std::size_t Pump(std::uint64_t activeGeneration, std::size_t maxCommands) {
    g_gameThreadId = std::this_thread::get_id();
    std::vector<Command> ready;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const std::size_t count = std::min(maxCommands, g_commands.size());
        ready.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            ready.push_back(std::move(g_commands.front()));
            g_commands.pop_front();
        }
    }

    std::size_t executed = 0;
    for (Command& command : ready) {
        if (command.generation != 0 && command.generation != activeGeneration) {
            ++g_stale;
            if (command.dropped) {
                command.dropped("stale_generation");
            }
            continue;
        }
        try {
            command.execute();
            ++executed;
            ++g_executed;
        } catch (...) {
            Logger::LogError("GameThreadDispatcher: command threw type=%s key=%s",
                command.type.c_str(), command.key.c_str());
            if (command.dropped) {
                command.dropped("exception");
            }
        }
    }
    return executed;
}

std::size_t CancelByType(const std::string& type, const char* reason) {
    std::vector<Command> removed;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto it = g_commands.begin(); it != g_commands.end();) {
            if (it->type == type) {
                removed.push_back(std::move(*it));
                it = g_commands.erase(it);
            } else {
                ++it;
            }
        }
    }
    g_cancelled.fetch_add(removed.size());
    DropCommands(removed, reason ? reason : "cancelled_type");
    return removed.size();
}

std::size_t CancelByKey(const std::string& key, const char* reason) {
    std::vector<Command> removed;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto it = g_commands.begin(); it != g_commands.end();) {
            if (it->key == key) {
                removed.push_back(std::move(*it));
                it = g_commands.erase(it);
            } else {
                ++it;
            }
        }
    }
    g_cancelled.fetch_add(removed.size());
    DropCommands(removed, reason ? reason : "cancelled_key");
    return removed.size();
}

void CancelAll(const char* reason) {
    std::vector<Command> removed;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        removed.assign(std::make_move_iterator(g_commands.begin()), std::make_move_iterator(g_commands.end()));
        g_commands.clear();
    }
    g_cancelled.fetch_add(removed.size());
    DropCommands(removed, reason ? reason : "cancelled_all");
}

Status GetStatus() {
    Status status;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        status.pending = g_commands.size();
    }
    status.queued = g_queued.load();
    status.executed = g_executed.load();
    status.cancelled = g_cancelled.load();
    status.stale = g_stale.load();
    status.rejected = g_rejected.load();
    return status;
}

bool IsGameThread() {
    return g_gameThreadId != std::thread::id{} && std::this_thread::get_id() == g_gameThreadId;
}

} // namespace GameThreadDispatcher
