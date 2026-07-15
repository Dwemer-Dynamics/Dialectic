#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>
#include <cstdint>
#include <utility>

namespace AgentManager {
    enum class RegistrationSource {
        Auto,
        Manual
    };

    struct EquipmentItem {
        int slot;
        std::string slotName;
        std::string name;
        std::string baseid;
        float condition;

        EquipmentItem()
            : slot(-1), condition(-1.0f) {}
    };

    struct InventoryItem {
        std::string name;
        std::string baseid;
        int count;
        bool equipped;
        int type;
        float condition;
        std::string ammo;
        std::vector<std::string> mods;

        InventoryItem()
            : count(0), equipped(false), type(0), condition(-1.0f) {}
    };
    
    // Fallout NV NPC data structure
    struct NPCData {
        // Basic Info
        std::string displayName;
        std::string baseName;
        std::string gender;
        std::string race;
        std::string voiceId;
        std::string voiceFormId;
        std::string voiceName;
        uint32_t refID;
        
        // SPECIAL Stats (7 attributes)
        int strength;
        int perception;
        int endurance;
        int charisma;
        int intelligence;
        int agility;
        int luck;
        
        // Skills (13 Fallout skills)
        int barter;
        int energyWeapons;
        int explosives;
        int guns;
        int lockpick;
        int medicine;
        int meleeWeapons;
        int repair;
        int science;
        int sneak;
        int speech;
        int survival;
        int unarmed;
        
        // Equipment (8 slots, format: name^formid)
        std::string head;
        std::string upperBody;
        std::string leftHand;
        std::string rightHand;
        std::string hair;
        std::string weapon;
        std::string upperBodyAddon;
        std::string lowerBodyAddon;
        std::vector<EquipmentItem> equipment;
        
        // Stats
        int level;
        float health;
        float healthMax;
        float actionPoints;
        float actionPointsMax;
        float scale;
        int xp;
        
        // Karma
        int karma;
        
        // Factions (formID:rank pairs)
        std::vector<std::pair<uint32_t, int>> factions;
        
        // Reputation (factionID:value pairs)
        std::vector<std::pair<uint32_t, int>> reputation;

        // Inventory snapshot from script-runner metadata.
        std::vector<InventoryItem> inventory;
        
        NPCData() : refID(0), strength(5), perception(5), endurance(5), charisma(5),
                    intelligence(5), agility(5), luck(5), barter(0), energyWeapons(0),
                    explosives(0), guns(0), lockpick(0), medicine(0), meleeWeapons(0),
                    repair(0), science(0), sneak(0), speech(0), survival(0), unarmed(0),
                    level(1), health(0), healthMax(0), actionPoints(0), actionPointsMax(0),
                    scale(1.0f), xp(0), karma(0) {}
    };
    
    // Collect NPC data from the latest script-runner snapshot.
    NPCData CollectNPCData(uint32_t expectedRefID);

    // Send actor profile JSON to the server for a known Fallout reference id.
    void SendActorProfile(const std::string& npcName, uint32_t refID);

    // Send actor profile updates from already collected metadata.
    void SendActorProfile(const NPCData& data);

    // Refresh and upload inventory/equipment metadata for a known actor ref.
    void RefreshActorMetadata(uint32_t refID, const std::string& npcName);

    // Queue actor profile, equipment, and inventory from the current native snapshot.
    // This never waits for script snapshots or network I/O on the game thread.
    bool RefreshActorMetadataForPrompt(uint32_t refID, const std::string& npcName, int timeoutMs = 900);
    
    // Check if actor is already registered as AI agent
    bool IsAIAgent(uint32_t formID);
    
    // Register actor as AI agent.
    void RegisterAIAgent(uint32_t formID, const std::string& name);

    // Register actor as AI agent and record whether activation was automatic or manual.
    void RegisterAIAgent(uint32_t formID, const std::string& name, RegistrationSource source, float distance = 0.0f);

    // Remove one or all actors from the runtime AI-agent registry.
    bool UnregisterAIAgent(uint32_t formID);
    std::size_t UnregisterAllAIAgents();

    // Update transient tracking data for an already registered AI agent.
    void MarkAgentSeen(uint32_t formID, float distance = 0.0f);

    // Check source state for registered actors.
    bool IsManuallyActivated(uint32_t formID);
    bool IsAutoManaged(uint32_t formID);
    std::string GetAgentName(uint32_t formID);
    uint32_t FindAgentFormIdByName(const std::string& name);
    std::vector<std::pair<uint32_t, std::string>> GetRegisteredAgentSnapshot();
    bool SelectLeastBoredNearbyAgent(float maxDistance, uint32_t& outFormID, std::string& outName);
    bool SelectLeastBoredNearbyAgent(
        const std::vector<std::pair<uint32_t, std::string>>& candidates,
        uint32_t& outFormID,
        std::string& outName);
    void MarkBoredEventFired(uint32_t formID);

    // Request and wait for script-runner actor snapshots.
    void RequestActorSnapshot(uint32_t refID, const std::string& npcName);
    void ClearActorSnapshotRequest();
    bool WaitForFreshActorSnapshot(uint32_t expectedRefID, int timeoutMs = 600,
                                   const std::function<bool()>& cancelRequested = {});
    
    // Initialize/Shutdown
    void Initialize();
    void Shutdown();
}

