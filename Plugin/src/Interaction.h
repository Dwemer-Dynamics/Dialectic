#pragma once
#include <cstdint>
#include <string>
namespace Interaction {
bool Allowed();
bool ManualInputAllowed();
bool IsTrigger(const std::string& type);
int Status();
uint64_t Epoch();
bool IsCurrent(uint64_t epoch);
uint64_t Generation();
void Toggle();
// Game thread. Idempotent: 1 when already in that state, 2 when a change was accepted or is
// already synchronizing toward it. Status() reads 2 until the server confirms, or 3 on failure.
int Request(bool enabled);
void Update();
std::wstring Headers();
}
