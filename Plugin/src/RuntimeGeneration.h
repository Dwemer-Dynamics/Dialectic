#pragma once

#include <cstdint>

namespace RuntimeGeneration {

std::uint64_t Current();
std::uint64_t Advance(const char* reason);
bool IsCurrent(std::uint64_t generation);
const char* LastReason();

} // namespace RuntimeGeneration
