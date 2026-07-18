#pragma once

namespace PlayerSurvivalManagerFNV {

void Initialize();
void Shutdown();
void Reset(const char* reason);
void ForceRefresh(const char* reason, int delayMs = 0);
void Update();

} // namespace PlayerSurvivalManagerFNV
