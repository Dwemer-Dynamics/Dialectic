#pragma once

#include <cstddef>
#include <cstdint>

namespace SpatialSnapshotManagerFNV {

struct Status {
    std::uint32_t cellFormId{0};
    std::uint64_t generation{0};
    std::size_t candidates{0};
    std::size_t nextCandidate{0};
    std::uint64_t evaluations{0};
};

void Update(std::size_t maxEvaluations = 1);
void Invalidate();
Status GetStatus();

} // namespace SpatialSnapshotManagerFNV
