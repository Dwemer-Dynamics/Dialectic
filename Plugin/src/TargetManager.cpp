// TargetManager.cpp - NPC targeting/crosshair detection for Dialectic

#include "TargetManager.h"
#include "ActorEligibilityFNV.h"
#include "RuntimeSnapshot.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <unordered_set>
#include <vector>
#include <algorithm>
#include <chrono>

// Forward declare Log from main.cpp
void Log(const char* fmt, ...);

namespace TargetManager {

// Current target info
static TargetInfo g_currentTarget;
static TargetInfo g_previousTarget;

// Nearby NPC tracking (for fallback targeting)
struct NearbyNPC {
    uint32_t formId;
    std::string name;
    float distance;
    float lastSeenTime;
};
static std::vector<NearbyNPC> g_nearbyNPCs;
static float g_currentTime = 0.0f;

// Registered AI agents
static std::unordered_set<uint32_t> g_registeredAgents;
static std::vector<uint32_t> g_agentList;

// Target change callback
static TargetChangeCallback g_targetChangeCallback = nullptr;

static void ApplyNativeActor(const RuntimeSnapshot::ActorState& actor) {
    const bool sameActor = g_currentTarget.formId == actor.formId;
    const std::string existingName = sameActor ? g_currentTarget.name : "";
    g_currentTarget.formId = actor.formId;
    g_currentTarget.baseFormId = actor.baseFormId;
    g_currentTarget.name = actor.name.empty() ? existingName : actor.name;
    g_currentTarget.isActor = true;
    g_currentTarget.isAIAgent = IsAIAgent(actor.formId);
    g_currentTarget.distance = actor.distanceToPlayer;
    g_currentTarget.isAlive = !actor.dead && !actor.deleted && actor.loaded3D;
    g_currentTarget.isHostile = actor.hostileToPlayer;
}

static ActorEligibilityFNV::Metadata EligibilityMetadata(const RuntimeSnapshot::ActorState& actor) {
    ActorEligibilityFNV::Metadata metadata;
    metadata.name = actor.name;
    metadata.race = actor.raceName;
    metadata.baseType = actor.baseType;
    metadata.baseTypeKnown = actor.baseType != 0;
    metadata.isCreature = actor.creature;
    metadata.isCreatureKnown = true;
    return metadata;
}

void Initialize() {
    Log("TargetManager: Initializing...");
    
    // Clear state
    g_currentTarget = {};
    g_previousTarget = {};
    g_registeredAgents.clear();
    g_agentList.clear();
    g_targetChangeCallback = nullptr;
    
    // Find game addresses (would need to be done via pattern scanning or known offsets)
    // For now, we'll rely on NVSE script functions to provide this data
    
    Log("TargetManager: Initialized");
}

void Shutdown() {
    Log("TargetManager: Shutting down");
    g_registeredAgents.clear();
    g_agentList.clear();
}

void Update() {
    g_previousTarget = g_currentTarget;

    RuntimeSnapshot::GameState gameState;
    if (RuntimeSnapshot::TryGetFreshGameState(gameState, std::chrono::milliseconds(500))) {
        if (gameState.crosshairFormId != 0) {
            RuntimeSnapshot::ActorState actor;
            if (RuntimeSnapshot::TryGetActor(gameState.crosshairFormId, actor)) {
                ApplyNativeActor(actor);
            } else {
                g_currentTarget = {};
            }
        } else {
            g_currentTarget = {};
        }
    }
    
    // Check if target changed
    if (g_currentTarget.formId != g_previousTarget.formId) {
        if (g_targetChangeCallback) {
            g_targetChangeCallback(g_currentTarget, g_previousTarget);
        }
        
        if (g_currentTarget.formId != 0) {
            Log("TargetManager: Target changed to %s (0x%08X)", 
                g_currentTarget.name.c_str(), g_currentTarget.formId);
        } else {
            Log("TargetManager: Target cleared");
        }
    }
}

const TargetInfo& GetCurrentTarget() {
    return g_currentTarget;
}

bool HasValidNPCTarget() {
    return g_currentTarget.formId != 0 && g_currentTarget.isActor && g_currentTarget.isAlive;
}

bool IsTargetAIAgent() {
    return g_currentTarget.isAIAgent;
}

uint32_t GetTargetFormId() {
    return g_currentTarget.formId;
}

const std::string& GetTargetName() {
    return g_currentTarget.name;
}

float GetTargetDistance() {
    return g_currentTarget.distance;
}

bool FindNearestNPC(float maxDistance) {
    Log("TargetManager: Searching for nearest NPC within %.1f units...", maxDistance);

    RuntimeSnapshot::GameState gameState;
    const bool nativeRegistryAvailable = RuntimeSnapshot::TryGetFreshGameState(
        gameState, std::chrono::milliseconds(500));
    const std::vector<RuntimeSnapshot::ActorState> nativeActors = RuntimeSnapshot::GetActors();
    const RuntimeSnapshot::ActorState* nearest = nullptr;
    for (const RuntimeSnapshot::ActorState& actor : nativeActors) {
        if (actor.formId == 0 || actor.dead || actor.deleted || !actor.loaded3D ||
            actor.distanceToPlayer > maxDistance) {
            continue;
        }
        if (nativeRegistryAvailable && !RuntimeSnapshot::IsActorInScene(actor, gameState)) {
            continue;
        }
        if (!ActorEligibilityFNV::IsAutoActivationAllowed(EligibilityMetadata(actor))) {
            continue;
        }
        if (!nearest || actor.distanceToPlayer < nearest->distanceToPlayer) {
            nearest = &actor;
        }
    }
    if (nearest) {
        ApplyNativeActor(*nearest);
        Log("TargetManager: Native nearest NPC: %s (0x%08X) at %.1f units",
            g_currentTarget.name.c_str(), g_currentTarget.formId, g_currentTarget.distance);
        return true;
    }

    if (nativeRegistryAvailable) {
        Log("TargetManager: Native registry has no eligible NPC in the current scene");
        return false;
    }
    
    if (g_nearbyNPCs.empty()) {
        Log("TargetManager: No eligible NPCs in native or script fallback registry");
        return false;
    }
    
    // Find the closest NPC from our tracked list
    NearbyNPC* closest = nullptr;
    float closestDist = maxDistance;
    
    for (auto& npc : g_nearbyNPCs) {
        if (npc.distance < closestDist) {
            closest = &npc;
            closestDist = npc.distance;
        }
    }
    
    if (closest) {
        Log("TargetManager: Found nearest NPC: %s (0x%08X) at %.1f units", 
            closest->name.c_str(), closest->formId, closest->distance);
        SetCurrentTarget(closest->formId, closest->name, true);
        g_currentTarget.distance = closest->distance;
        return true;
    }
    
    Log("TargetManager: No NPCs found within %.1f units", maxDistance);
    return false;
}

void SetNearbyNPC(uint32_t formId, const std::string& name, float distance) {
    // If formId is 0, generate a hash from the name as a unique ID
    if (formId == 0 && !name.empty()) {
        // Simple hash of the name
        std::hash<std::string> hasher;
        formId = static_cast<uint32_t>(hasher(name) & 0x7FFFFFFF);  // Use lower 31 bits
    }
    
    if (formId == 0) return;  // Skip if we still don't have an ID
    
    // Update or add this NPC to our nearby list
    for (auto& npc : g_nearbyNPCs) {
        if (npc.formId == formId || npc.name == name) {  // Match by ID or name
            npc.formId = formId;
            npc.distance = distance;
            npc.lastSeenTime = g_currentTime;
            return;
        }
    }
    
    // New NPC - add to list
    g_nearbyNPCs.push_back({formId, name, distance, g_currentTime});
    Log("TargetManager: Tracking nearby NPC: %s (ID: 0x%08X) at %.1f units", 
        name.c_str(), formId, distance);
}

void SetCurrentTarget(uint32_t formId, const std::string& name, bool isActor) {
    // If formId is 0, generate a hash from the name as a unique ID
    if (formId == 0 && !name.empty()) {
        std::hash<std::string> hasher;
        formId = static_cast<uint32_t>(hasher(name) & 0x7FFFFFFF);
    }
    
    // Save previous target
    g_previousTarget = g_currentTarget;
    
    // Set new target
    g_currentTarget.formId = formId;
    g_currentTarget.name = name;
    g_currentTarget.isActor = isActor;
    g_currentTarget.isAlive = true;  // Assume alive for now
    g_currentTarget.distance = 100.0f;  // Placeholder distance
    
    // Check if this is a registered AI agent
    g_currentTarget.isAIAgent = (g_registeredAgents.find(formId) != g_registeredAgents.end());
    
    // Fire callback if target changed
    if (g_targetChangeCallback && g_currentTarget.formId != g_previousTarget.formId) {
        Log("TargetManager: Target changed to %s (ID: 0x%08X)", name.c_str(), formId);
        g_targetChangeCallback(g_currentTarget, g_previousTarget);
    }
}

void ClearCurrentTarget() {
    g_previousTarget = g_currentTarget;
    g_currentTarget = {};
    
    if (g_targetChangeCallback && g_previousTarget.formId != 0) {
        g_targetChangeCallback(g_currentTarget, g_previousTarget);
    }
}

void RegisterTargetChangeCallback(TargetChangeCallback callback) {
    g_targetChangeCallback = callback;
}

void RegisterAIAgent(uint32_t formId, const std::string& name) {
    if (g_registeredAgents.find(formId) == g_registeredAgents.end()) {
        g_registeredAgents.insert(formId);
        g_agentList.push_back(formId);
        Log("TargetManager: Registered AI agent %s (0x%08X)", name.c_str(), formId);
    }
}

void UnregisterAIAgent(uint32_t formId) {
    g_registeredAgents.erase(formId);
    if (g_currentTarget.formId == formId) {
        g_currentTarget.isAIAgent = false;
    }
    auto it = std::find(g_agentList.begin(), g_agentList.end(), formId);
    if (it != g_agentList.end()) {
        g_agentList.erase(it);
    }
    Log("TargetManager: Unregistered AI agent 0x%08X", formId);
}

bool IsAIAgent(uint32_t formId) {
    return g_registeredAgents.find(formId) != g_registeredAgents.end();
}

const std::vector<uint32_t>& GetRegisteredAgents() {
    return g_agentList;
}

// Called from NVSE scripts to update crosshair target
extern "C" {
    __declspec(dllexport) void SetCrosshairTarget(uint32_t formId, uint32_t baseFormId, 
        const char* name, bool isActor, float distance, bool isAlive, bool isHostile) {
        
        g_currentTarget.formId = formId;
        g_currentTarget.baseFormId = baseFormId;
        g_currentTarget.name = name ? name : "";
        g_currentTarget.isActor = isActor;
        g_currentTarget.distance = distance;
        g_currentTarget.isAlive = isAlive;
        g_currentTarget.isHostile = isHostile;
        g_currentTarget.isAIAgent = IsAIAgent(formId);
    }
    
    __declspec(dllexport) void ClearCrosshairTarget() {
        g_currentTarget = {};
    }
}

} // namespace TargetManager
