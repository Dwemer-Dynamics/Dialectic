#include "RuntimeGeneration.h"

#include "Logger.h"

#include <atomic>
#include <mutex>
#include <string>

namespace RuntimeGeneration {
namespace {

std::atomic<std::uint64_t> g_generation{1};
std::mutex g_reasonMutex;
std::string g_lastReason{"startup"};

} // namespace

std::uint64_t Current() {
    return g_generation.load(std::memory_order_acquire);
}

std::uint64_t Advance(const char* reason) {
    const std::uint64_t next = g_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    {
        std::lock_guard<std::mutex> lock(g_reasonMutex);
        g_lastReason = reason && reason[0] ? reason : "unspecified";
    }
    Logger::LogInfo("RuntimeGeneration: advanced to %llu reason=%s",
        static_cast<unsigned long long>(next),
        reason && reason[0] ? reason : "unspecified");
    return next;
}

bool IsCurrent(std::uint64_t generation) {
    return generation == Current();
}

const char* LastReason() {
    thread_local std::string copy;
    std::lock_guard<std::mutex> lock(g_reasonMutex);
    copy = g_lastReason;
    return copy.c_str();
}

} // namespace RuntimeGeneration
