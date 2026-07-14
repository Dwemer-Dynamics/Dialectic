#include "RuntimeEventBus.h"

#include "Logger.h"

#include <algorithm>
#include <atomic>
#include <deque>
#include <mutex>

namespace RuntimeEventBus {
namespace {

constexpr std::size_t kMaxPendingEvents = 1024;
std::mutex g_mutex;
std::deque<Event> g_events;
std::atomic<std::uint64_t> g_sequence{0};
std::atomic<std::uint64_t> g_droppedEvents{0};

} // namespace

void Publish(Event event) {
    event.sequence = g_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_events.size() >= kMaxPendingEvents) {
        const std::uint64_t dropped = g_droppedEvents.fetch_add(1, std::memory_order_relaxed) + 1;
        // Keep overflow visible without turning synchronous logging into a
        // second performance problem during an event storm.
        if ((dropped & (dropped - 1)) == 0) {
            Logger::LogWarning(
                "RuntimeEventBus: dropped %llu event(s) because queue reached %zu",
                static_cast<unsigned long long>(dropped),
                kMaxPendingEvents);
        }
        g_events.pop_front();
    }
    g_events.push_back(std::move(event));
}

std::vector<Event> Drain(std::size_t maxEvents) {
    std::vector<Event> result;
    std::lock_guard<std::mutex> lock(g_mutex);
    const std::size_t count = std::min(maxEvents, g_events.size());
    result.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        result.push_back(std::move(g_events.front()));
        g_events.pop_front();
    }
    return result;
}

void Clear() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_events.clear();
    g_droppedEvents.store(0, std::memory_order_relaxed);
}

std::size_t PendingCount() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_events.size();
}

} // namespace RuntimeEventBus
