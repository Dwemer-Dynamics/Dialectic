// TargetManager.h - Handles NPC targeting/crosshair detection for Dialectic

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace TargetManager {

// Basic reference info from game
struct TargetInfo {
    uint32_t formId;           // Reference form ID
    uint32_t baseFormId;       // Base form ID
    std::string name;          // Display name
    std::string editorId;      // Editor ID if available
    bool isActor;              // Is this an actor/NPC
    bool isAIAgent;            // Is this actor registered as AI agent
    float distance;            // Distance from player
    bool isAlive;              // Is the actor alive
    bool isHostile;            // Is the actor hostile to player
};

// Initialize the target manager
void Initialize();

// Shutdown and cleanup
void Shutdown();

// Update targeting - call each frame
void Update();

// Get the currently targeted reference (under crosshair)
const TargetInfo& GetCurrentTarget();

// Check if there's a valid NPC target
bool HasValidNPCTarget();

// Check if the current target is an AI agent
bool IsTargetAIAgent();

// Get the targeted actor's form ID
uint32_t GetTargetFormId();

// Get the targeted actor's name
const std::string& GetTargetName();

// Get distance to target
float GetTargetDistance();

// Find the nearest NPC within a certain distance (fallback if no crosshair target)
bool FindNearestNPC(float maxDistance = 300.0f);

// Set the current target from script/external source (e.g., NVSE GetCrosshairRef)
void SetCurrentTarget(uint32_t formId, const std::string& name, bool isActor = true);

// Set a nearby NPC (used for fallback targeting)
void SetNearbyNPC(uint32_t formId, const std::string& name, float distance);

// Clear the current target
void ClearCurrentTarget();

// Register a callback for when target changes
typedef void (*TargetChangeCallback)(const TargetInfo& newTarget, const TargetInfo& oldTarget);
void RegisterTargetChangeCallback(TargetChangeCallback callback);

// Mark an actor as an AI agent
void RegisterAIAgent(uint32_t formId, const std::string& name);

// Unregister an AI agent
void UnregisterAIAgent(uint32_t formId);

// Check if a form ID is registered as an AI agent
bool IsAIAgent(uint32_t formId);

// Get all registered AI agent form IDs
const std::vector<uint32_t>& GetRegisteredAgents();

} // namespace TargetManager
