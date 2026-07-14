// ActivationManager.h - AI agent activation policy for Dialectic

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ActivationManager {

enum class ActivationSource {
    Auto,
    Manual
};

void Initialize();
void Shutdown();
void Update();

// Activates the current target or crosshair NPC as an AI agent.
bool ActivateCurrentTarget(ActivationSource source);

// Agent-management operations used by the in-game MCM manager.
bool ActivateActor(uint32_t formId, ActivationSource source);
std::size_t ActivateNearbyActors(ActivationSource source);
bool DeactivateActor(uint32_t formId);
std::size_t DeactivateAllActors();
std::vector<std::pair<uint32_t, std::string>> GetNearbyManageableActors();

} // namespace ActivationManager
