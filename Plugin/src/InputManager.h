// InputManager.h - Handles hotkey input for Dialectic

#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <functional>

namespace InputManager {

// Key states
enum class KeyState {
    None,
    JustPressed,
    Held,
    JustReleased
};

// Hotkey actions
enum class HotkeyAction {
    TalkToNPC,      // Start talking to targeted NPC
        StopTalking,    // Halt AI dialogue/actions killswitch
    ToggleVoice,    // Toggle voice input
    OpenMicMute,    // Toggle open mic mute
    ManualActivateNPC, // Manually activate targeted NPC as an AI agent
    OpenMenu,       // Open AI agent menu
    QuickCommand,   // Quick command mode
    DynamicProfileMenu,    // Open dynamic profile action selector
    ToggleModes,           // Open mode selector
    ToggleLLMModel         // Open LLM connector slot selector
};

// Initialize the input manager
void Initialize();

// Shutdown and cleanup
void Shutdown();

// Update input state - call this each frame
void Update();

// Check if a hotkey action was triggered this frame
bool IsActionTriggered(HotkeyAction action);

// Check if a key is currently held
bool IsKeyHeld(int virtualKey);

// Get the current state of a key
KeyState GetKeyState(int virtualKey);

// Set hotkey binding
void SetHotkey(HotkeyAction action, int virtualKey);

// Get current hotkey for action
int GetHotkey(HotkeyAction action);

// Load hotkey configuration from INI
void LoadConfig();

// Save hotkey configuration to INI
void SaveConfig();

// Convert Fallout scancode to Windows virtual key
int ScancodeToVirtualKey(int scancode);

} // namespace InputManager
