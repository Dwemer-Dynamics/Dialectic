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
void Update();
std::wstring Headers();
}
