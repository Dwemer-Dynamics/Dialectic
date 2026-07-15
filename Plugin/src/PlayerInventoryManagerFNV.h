#pragma once

namespace PlayerInventoryManagerFNV {

void Initialize();
void Shutdown();
void Update();

// Coalesces event bursts and refreshes the native player inventory after the
// game has finished applying the underlying container change.
void MarkDirty(const char* reason, int delayMs = 200);
void ForceRefresh(const char* reason, int delayMs = 0);
void Reset(const char* reason);

} // namespace PlayerInventoryManagerFNV
