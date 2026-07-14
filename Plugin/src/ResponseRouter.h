#pragma once

#include <cstdint>
#include <string>

namespace ResponseRouter {

bool ProcessJsonResponse(const std::string& response, const char* source = "ResponseRouter", uint64_t responseGeneration = 0);
bool ProcessJsonActionsOnly(const std::string& response, const char* source = "ResponseRouter", uint64_t responseGeneration = 0);

} // namespace ResponseRouter
