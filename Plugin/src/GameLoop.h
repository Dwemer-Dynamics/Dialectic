// GameLoop.h - Main game loop integration for Dialectic

#pragma once

#include <cstdint>
#include <string>

namespace GameLoop {

// Game state information
struct GameState {
    bool isInGame;          // Player is in-game (not main menu)
    bool isPaused;          // Game is paused
    bool isInMenu;          // Player is in a menu
    bool isInDialogue;      // Player is in vanilla dialogue
    bool isInCombat;        // Player is in combat
    bool isLoading;         // Game is loading
    float gameTime;         // Current game time (hours)
    std::string location;   // Current location name
};

// Initialize the game loop
void Initialize();

// Shutdown the game loop
void Shutdown();

// Main update tick - call from game's main loop
void Update(float deltaTime);

// Get current game state
const GameState& GetGameState();

// Whether player/actor combat state permits normal AI conversation requests.
bool IsCombatDialogueAllowed(uint32_t actorFormId = 0);

// Start AI conversation with targeted NPC
bool StartConversation();

// Stop current AI conversation
void StopConversation();

// EndConversation action hook: marks an NPC unavailable for the configured cooldown.
void ApplyEndConversationCooldown(uint32_t formId, const std::string& name);

// Send player message to AI
void SendPlayerMessage(const std::string& message);

// Check if AI conversation is active
bool IsConversationActive();

// Get current conversation partner name
const std::string& GetConversationPartner();

// Get the live reference bound to the current conversation partner
uint32_t GetConversationPartnerFormId();

// Process voice input (when voice key is held/released)
void StartVoiceInput();
void StopVoiceInput();
bool IsVoiceInputActive();

// Script command entry points for xNVSE keydown handlers.
void HaltAIActionsNow();
void RequestTextInputMenuOpen();
bool IsTextInputMenuActiveOrRecentlyClosed();
void MarkRuntimeConfigDirty();
void RequestDialecticControlMenuOpen();
void RequestModeMenuOpen();
void RequestLLMModelMenuOpen();
void RequestDynamicProfileMenuOpen();
void SubmitCapturedDialogue(const std::string& source,
                            const std::string& speaker,
                            const std::string& speakerRefId,
                            const std::string& target,
                            const std::string& targetRefId,
                            const std::string& text,
                            bool isPlayerLine,
                            bool menuMode,
                            const std::string& captureId);

// Queue a normalized gameplay event for the existing RPG comment pipeline.
void QueueRpgCommentEvent(const std::string& eventType, const std::string& eventText);

} // namespace GameLoop
