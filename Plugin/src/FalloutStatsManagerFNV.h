#pragma once

#include <cstdint>
namespace FalloutStatsManagerFNV {

void Initialize();
void Shutdown();
void Reset(const char* reason);
void Update();

bool UpdateStat(int statCode, std::int64_t value);

} // namespace FalloutStatsManagerFNV
