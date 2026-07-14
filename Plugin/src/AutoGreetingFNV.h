#pragma once

#include <cstdint>
#include <string>

namespace AutoGreetingFNV {

void HandleServerResponse(const std::string& response, std::uint64_t runtimeGeneration);
void Update();
void Cancel(const char* reason);

} // namespace AutoGreetingFNV
