// InputManager.h - Handles event-driven hotkey input for Dialectic

#pragma once

#include <cstdint>

namespace InputManager {

enum class HotkeyAction {
    TalkToNPC,
    StopTalking,
    ToggleVoice,
    OpenMicMute,
    ManualActivateNPC,
    OpenMenu,
    QuickCommand,
    DynamicProfileMenu,
    ToggleModes,
    ToggleLLMModel,
    PipVision,
    Count
};

void Initialize();
void Shutdown();

// Clears queued/held input when Fallout loses focus or enters blocking UI.
void Update();

bool IsGameForeground();
bool IsActionTriggered(HotkeyAction action, uint32_t debounceMs = 250);
bool IsActionReleased(HotkeyAction action);
bool IsActionHeld(HotkeyAction action);
bool TryClaimAction(HotkeyAction action, uint32_t debounceMs = 250);

// Called by the JIP LN key event bridge with Fallout DirectInput scan codes.
bool HandleScanCodeEvent(int scanCode, bool pressed);
bool IsScanCodeHeld(int scanCode);

void SetHotkey(HotkeyAction action, int scanCode);
int GetHotkey(HotkeyAction action);
void LoadConfig();
void SaveConfig();

} // namespace InputManager
