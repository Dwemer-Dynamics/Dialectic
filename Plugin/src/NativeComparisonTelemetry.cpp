#include "NativeComparisonTelemetry.h"

#include "Logger.h"

#include <chrono>
#include <map>
#include <mutex>

namespace NativeComparisonTelemetry {
namespace {

struct Entry {
    DomainStatus status;
    std::chrono::steady_clock::time_point lastLog{};
};

std::mutex g_mutex;
std::map<std::string, Entry> g_entries;
constexpr auto kHeartbeat = std::chrono::seconds(30);

const char* ResultName(Result result) {
    switch (result) {
        case Result::Match: return "match";
        case Result::Mismatch: return "mismatch";
        default: return "unavailable";
    }
}

} // namespace

void Record(const std::string& domain, Result result, const std::string& detail) {
    if (domain.empty()) return;

    const auto now = std::chrono::steady_clock::now();
    bool shouldLog = false;
    DomainStatus snapshot;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        Entry& entry = g_entries[domain];
        entry.status.domain = domain;
        switch (result) {
            case Result::Match: ++entry.status.matches; break;
            case Result::Mismatch: ++entry.status.mismatches; break;
            case Result::Unavailable: ++entry.status.unavailable; break;
        }
        shouldLog = entry.status.lastResult != result || entry.status.lastDetail != detail ||
            entry.lastLog.time_since_epoch().count() == 0 || now - entry.lastLog >= kHeartbeat;
        entry.status.lastResult = result;
        entry.status.lastDetail = detail;
        if (shouldLog) entry.lastLog = now;
        snapshot = entry.status;
    }

    if (shouldLog) {
        Logger::LogInfo(
            "[NATIVE_COMPARE] domain=%s result=%s matches=%llu mismatches=%llu unavailable=%llu detail=%s",
            domain.c_str(), ResultName(result),
            static_cast<unsigned long long>(snapshot.matches),
            static_cast<unsigned long long>(snapshot.mismatches),
            static_cast<unsigned long long>(snapshot.unavailable),
            detail.empty() ? "none" : detail.c_str());
    }
}

std::vector<DomainStatus> Snapshot() {
    std::vector<DomainStatus> result;
    std::lock_guard<std::mutex> lock(g_mutex);
    result.reserve(g_entries.size());
    for (const auto& entry : g_entries) result.push_back(entry.second.status);
    return result;
}

void Clear() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_entries.clear();
}

} // namespace NativeComparisonTelemetry
