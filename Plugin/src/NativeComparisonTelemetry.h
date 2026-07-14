#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace NativeComparisonTelemetry {

enum class Result {
    Match,
    Mismatch,
    Unavailable
};

struct DomainStatus {
    std::string domain;
    std::uint64_t matches{0};
    std::uint64_t mismatches{0};
    std::uint64_t unavailable{0};
    Result lastResult{Result::Unavailable};
    std::string lastDetail;
};

void Record(const std::string& domain, Result result, const std::string& detail = {});
std::vector<DomainStatus> Snapshot();
void Clear();

} // namespace NativeComparisonTelemetry
