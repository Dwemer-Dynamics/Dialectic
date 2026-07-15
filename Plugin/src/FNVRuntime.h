#pragma once

#include <cstdint>

namespace FNVRuntime {

bool Initialize(const void* nvseInterface, std::uint32_t pluginHandle);
void Shutdown();
bool IsAvailable();
void PumpLegacyFrameFallback();
void RecordScriptBridgeTick(std::uint32_t bridgeId);

} // namespace FNVRuntime
