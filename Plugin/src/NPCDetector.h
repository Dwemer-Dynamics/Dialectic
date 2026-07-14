// NPCDetector.h - Real NPC detection using NVSE game structures

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace NPCDetector {

// Information about a detected NPC
struct NPCInfo {
    uint32_t formId = 0;
    std::string name;
    float distance = 0.0f;
    bool isCreature = false;
    bool isDead = false;
    bool isHostile = false;
    bool isValid = false;
};

// Initialize the NPC detector
void Initialize();

// Shutdown the NPC detector
void Shutdown();

// Get the NPC under the crosshair (returns invalid NPCInfo if none)
NPCInfo GetCrosshairNPC();

// Get all NPCs within a certain radius of the player
std::vector<NPCInfo> GetNearbyNPCs(float maxDistance = 500.0f);

// Get the closest NPC to the player
NPCInfo GetClosestNPC(float maxDistance = 500.0f);

// Check if a form ID is an NPC/Creature
bool IsActor(uint32_t formId);

// Check if an actor is excluded (based on config)
bool IsExcluded(uint32_t formId, const std::string& name);

} // namespace NPCDetector
