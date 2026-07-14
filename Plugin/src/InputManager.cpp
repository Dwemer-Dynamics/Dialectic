// InputManager.cpp - Hotkey input handling for Dialectic

#include "InputManager.h"
#include "Config.h"
#include <unordered_map>
#include <array>

// Forward declare Log from main.cpp
void Log(const char* fmt, ...);

namespace InputManager {

// Hotkey bindings (action -> virtual key code)
static std::unordered_map<HotkeyAction, int> g_hotkeyBindings;

// Previous frame key states
static std::array<bool, 256> g_prevKeyStates;
static std::array<bool, 256> g_currKeyStates;

// Action triggered flags for this frame
static std::unordered_map<HotkeyAction, bool> g_actionTriggered;

// Scancode to VK mapping (Fallout/Bethesda games use DirectInput scancodes)
static std::unordered_map<int, int> g_scancodeToVK = {
    {1, VK_ESCAPE},
    {2, '1'}, {3, '2'}, {4, '3'}, {5, '4'}, {6, '5'},
    {7, '6'}, {8, '7'}, {9, '8'}, {10, '9'}, {11, '0'},
    {12, VK_OEM_MINUS}, {13, VK_OEM_PLUS}, {14, VK_BACK}, {15, VK_TAB},
    {16, 'Q'}, {17, 'W'}, {18, 'E'}, {19, 'R'}, {20, 'T'},
    {21, 'Y'}, {22, 'U'}, {23, 'I'}, {24, 'O'}, {25, 'P'},
    {26, VK_OEM_4}, {27, VK_OEM_6}, {28, VK_RETURN}, {29, VK_CONTROL},
    {30, 'A'}, {31, 'S'}, {32, 'D'}, {33, 'F'}, {34, 'G'},
    {35, 'H'}, {36, 'J'}, {37, 'K'}, {38, 'L'},
    {39, VK_OEM_1}, {40, VK_OEM_7}, {41, VK_OEM_3}, {42, VK_SHIFT},
    {43, VK_OEM_5}, {44, 'Z'}, {45, 'X'}, {46, 'C'}, {47, 'V'},
    {48, 'B'}, {49, 'N'}, {50, 'M'},
    {51, VK_OEM_COMMA}, {52, VK_OEM_PERIOD}, {53, VK_OEM_2},
    {54, VK_RSHIFT}, {55, VK_MULTIPLY}, {56, VK_MENU}, {57, VK_SPACE},
    {58, VK_CAPITAL},
    {59, VK_F1}, {60, VK_F2}, {61, VK_F3}, {62, VK_F4}, {63, VK_F5},
    {64, VK_F6}, {65, VK_F7}, {66, VK_F8}, {67, VK_F9}, {68, VK_F10},
    {69, VK_NUMLOCK}, {70, VK_SCROLL},
    {71, VK_NUMPAD7}, {72, VK_NUMPAD8}, {73, VK_NUMPAD9}, {74, VK_SUBTRACT},
    {75, VK_NUMPAD4}, {76, VK_NUMPAD5}, {77, VK_NUMPAD6}, {78, VK_ADD},
    {79, VK_NUMPAD1}, {80, VK_NUMPAD2}, {81, VK_NUMPAD3},
    {82, VK_NUMPAD0}, {83, VK_DECIMAL},
    {87, VK_F11}, {88, VK_F12},
    {156, VK_RETURN}, {157, VK_RCONTROL}, {181, VK_DIVIDE},
    {183, VK_SNAPSHOT}, {184, VK_RMENU},
    {199, VK_HOME}, {200, VK_UP}, {201, VK_PRIOR},
    {203, VK_LEFT}, {205, VK_RIGHT},
    {207, VK_END}, {208, VK_DOWN}, {209, VK_NEXT},
    {210, VK_INSERT}, {211, VK_DELETE},
    // Mouse buttons
    {256, VK_LBUTTON}, {257, VK_RBUTTON}, {258, VK_MBUTTON},
    {259, VK_XBUTTON1}, {260, VK_XBUTTON2}
};

void Initialize() {
    Log("InputManager: Initializing...");
    
    // Clear states
    g_prevKeyStates.fill(false);
    g_currKeyStates.fill(false);
    g_actionTriggered.clear();
    
    // Default hotkeys are unbound. V is handled separately by the in-game text script.
    g_hotkeyBindings[HotkeyAction::TalkToNPC] = 0;
    g_hotkeyBindings[HotkeyAction::StopTalking] = 0;
    g_hotkeyBindings[HotkeyAction::ToggleVoice] = 0;
    g_hotkeyBindings[HotkeyAction::OpenMicMute] = 0;
    g_hotkeyBindings[HotkeyAction::ManualActivateNPC] = 0;
    g_hotkeyBindings[HotkeyAction::OpenMenu] = 0;
    g_hotkeyBindings[HotkeyAction::QuickCommand] = 0;
    g_hotkeyBindings[HotkeyAction::DynamicProfileMenu] = 0;
    g_hotkeyBindings[HotkeyAction::ToggleModes] = 0;
    g_hotkeyBindings[HotkeyAction::ToggleLLMModel] = 0;
    
    // Load from config
    LoadConfig();
    
    Log("InputManager: Initialized with hotkeys - Talk: 0x%02X, Voice: 0x%02X, ManualActivate: 0x%02X",
        g_hotkeyBindings[HotkeyAction::TalkToNPC],
        g_hotkeyBindings[HotkeyAction::ToggleVoice],
        g_hotkeyBindings[HotkeyAction::ManualActivateNPC]);
}

void Shutdown() {
    Log("InputManager: Shutting down");
    g_hotkeyBindings.clear();
    g_actionTriggered.clear();
}

void Update() {
    // Save previous states
    g_prevKeyStates = g_currKeyStates;
    
    // Poll current key states
    for (int i = 0; i < 256; i++) {
        g_currKeyStates[i] = (GetAsyncKeyState(i) & 0x8000) != 0;
    }
    
    // Check hotkey actions
    g_actionTriggered.clear();
    for (const auto& [action, vk] : g_hotkeyBindings) {
        if (vk <= 0 || vk >= 256) {
            continue;
        }
        // Action triggers on key press (not held)
        if (g_currKeyStates[vk] && !g_prevKeyStates[vk]) {
            g_actionTriggered[action] = true;
        }
    }
}

bool IsActionTriggered(HotkeyAction action) {
    auto it = g_actionTriggered.find(action);
    return it != g_actionTriggered.end() && it->second;
}

bool IsKeyHeld(int virtualKey) {
    if (virtualKey <= 0 || virtualKey >= 256) return false;
    return g_currKeyStates[virtualKey];
}

KeyState GetKeyState(int virtualKey) {
    if (virtualKey <= 0 || virtualKey >= 256) return KeyState::None;
    
    bool curr = g_currKeyStates[virtualKey];
    bool prev = g_prevKeyStates[virtualKey];
    
    if (curr && !prev) return KeyState::JustPressed;
    if (curr && prev) return KeyState::Held;
    if (!curr && prev) return KeyState::JustReleased;
    return KeyState::None;
}

void SetHotkey(HotkeyAction action, int virtualKey) {
    g_hotkeyBindings[action] = virtualKey;
}

int GetHotkey(HotkeyAction action) {
    auto it = g_hotkeyBindings.find(action);
    return (it != g_hotkeyBindings.end()) ? it->second : 0;
}

void LoadConfig() {
    // User overrides win over shipped defaults.
    int talkKey = Config::ReadINIInt("Hotkeys", "TalkToNPC", 0);
    int stopKey = Config::ReadINIInt("Hotkeys", "StopTalking", 0);
    int voiceKey = Config::ReadINIInt("Hotkeys", "ToggleVoice", 0);
    int openMicMuteKey = Config::ReadINIInt("Hotkeys", "OpenMicMute", 0);
    int manualActivateKey = Config::ReadINIInt("Hotkeys", "ManualActivate", 0);
    int menuKey = Config::ReadINIInt("Hotkeys", "OpenMenu", 0);
    int commandKey = Config::ReadINIInt("Hotkeys", "QuickCommand", 0);
    int dynamicProfileMenuKey = Config::ReadINIInt("Hotkeys", "DynamicProfileMenu", 0);
    int toggleModesKey = Config::ReadINIInt("Hotkeys", "ToggleModes", 0);
    int toggleLLMModelKey = Config::ReadINIInt("Hotkeys", "ToggleLLMModel", 0);
    
    auto normalizeConfiguredKey = [](HotkeyAction action, int keyCode) {
        if (keyCode <= 0) {
            return 0;
        }

        // MCM Extender keyboard keybinds are stored as Fallout/xNVSE scancodes.
        // Existing Dialectic defaults are Windows VK codes, so preserve those two
        // legacy defaults while accepting scancode values written by the MCM menu.
        if ((action == HotkeyAction::StopTalking && keyCode == VK_ESCAPE) ||
            (action == HotkeyAction::ToggleVoice && keyCode == 'G') ||
            (action == HotkeyAction::ManualActivateNPC && keyCode == 'H')) {
            return keyCode;
        }

        const int mapped = ScancodeToVirtualKey(keyCode);
        if (mapped > 0) {
            Log("InputManager: mapped configured key %d to virtual key 0x%02X", keyCode, mapped);
            return mapped;
        }

        return keyCode;
    };

    g_hotkeyBindings[HotkeyAction::TalkToNPC] = normalizeConfiguredKey(HotkeyAction::TalkToNPC, talkKey);
    g_hotkeyBindings[HotkeyAction::StopTalking] = normalizeConfiguredKey(HotkeyAction::StopTalking, stopKey);
    g_hotkeyBindings[HotkeyAction::ToggleVoice] = normalizeConfiguredKey(HotkeyAction::ToggleVoice, voiceKey);
    g_hotkeyBindings[HotkeyAction::OpenMicMute] = normalizeConfiguredKey(HotkeyAction::OpenMicMute, openMicMuteKey);
    g_hotkeyBindings[HotkeyAction::ManualActivateNPC] = normalizeConfiguredKey(HotkeyAction::ManualActivateNPC, manualActivateKey);
    g_hotkeyBindings[HotkeyAction::OpenMenu] = normalizeConfiguredKey(HotkeyAction::OpenMenu, menuKey);
    g_hotkeyBindings[HotkeyAction::QuickCommand] = normalizeConfiguredKey(HotkeyAction::QuickCommand, commandKey);
    g_hotkeyBindings[HotkeyAction::DynamicProfileMenu] = normalizeConfiguredKey(HotkeyAction::DynamicProfileMenu, dynamicProfileMenuKey);
    g_hotkeyBindings[HotkeyAction::ToggleModes] = normalizeConfiguredKey(HotkeyAction::ToggleModes, toggleModesKey);
    g_hotkeyBindings[HotkeyAction::ToggleLLMModel] = normalizeConfiguredKey(HotkeyAction::ToggleLLMModel, toggleLLMModelKey);

    Log("InputManager: loaded raw hotkeys Talk=%d Voice=%d OpenMicMute=%d Stop=%d Manual=%d Modes=%d LLM=%d Dynamic=%d",
        talkKey,
        voiceKey,
        openMicMuteKey,
        stopKey,
        manualActivateKey,
        toggleModesKey,
        toggleLLMModelKey,
        dynamicProfileMenuKey);
    Log("InputManager: active VK hotkeys Talk=0x%02X Voice=0x%02X OpenMicMute=0x%02X Stop=0x%02X Manual=0x%02X Modes=0x%02X LLM=0x%02X Dynamic=0x%02X",
        g_hotkeyBindings[HotkeyAction::TalkToNPC],
        g_hotkeyBindings[HotkeyAction::ToggleVoice],
        g_hotkeyBindings[HotkeyAction::OpenMicMute],
        g_hotkeyBindings[HotkeyAction::StopTalking],
        g_hotkeyBindings[HotkeyAction::ManualActivateNPC],
        g_hotkeyBindings[HotkeyAction::ToggleModes],
        g_hotkeyBindings[HotkeyAction::ToggleLLMModel],
        g_hotkeyBindings[HotkeyAction::DynamicProfileMenu]);
}

void SaveConfig() {
    const char* iniPath = Config::GetCustomINIPath();
    char buffer[32];
    
    // Write hotkey bindings to INI
    sprintf_s(buffer, "%d", g_hotkeyBindings[HotkeyAction::TalkToNPC]);
    WritePrivateProfileStringA("Hotkeys", "TalkToNPC", buffer, iniPath);

    sprintf_s(buffer, "%d", g_hotkeyBindings[HotkeyAction::StopTalking]);
    WritePrivateProfileStringA("Hotkeys", "StopTalking", buffer, iniPath);
    
    sprintf_s(buffer, "%d", g_hotkeyBindings[HotkeyAction::ToggleVoice]);
    WritePrivateProfileStringA("Hotkeys", "ToggleVoice", buffer, iniPath);

    sprintf_s(buffer, "%d", g_hotkeyBindings[HotkeyAction::OpenMicMute]);
    WritePrivateProfileStringA("Hotkeys", "OpenMicMute", buffer, iniPath);
    
    sprintf_s(buffer, "%d", g_hotkeyBindings[HotkeyAction::ManualActivateNPC]);
    WritePrivateProfileStringA("Hotkeys", "ManualActivate", buffer, iniPath);
    
    sprintf_s(buffer, "%d", g_hotkeyBindings[HotkeyAction::OpenMenu]);
    WritePrivateProfileStringA("Hotkeys", "OpenMenu", buffer, iniPath);
    
    sprintf_s(buffer, "%d", g_hotkeyBindings[HotkeyAction::QuickCommand]);
    WritePrivateProfileStringA("Hotkeys", "QuickCommand", buffer, iniPath);

    sprintf_s(buffer, "%d", g_hotkeyBindings[HotkeyAction::DynamicProfileMenu]);
    WritePrivateProfileStringA("Hotkeys", "DynamicProfileMenu", buffer, iniPath);

    sprintf_s(buffer, "%d", g_hotkeyBindings[HotkeyAction::ToggleModes]);
    WritePrivateProfileStringA("Hotkeys", "ToggleModes", buffer, iniPath);

    sprintf_s(buffer, "%d", g_hotkeyBindings[HotkeyAction::ToggleLLMModel]);
    WritePrivateProfileStringA("Hotkeys", "ToggleLLMModel", buffer, iniPath);
}

int ScancodeToVirtualKey(int scancode) {
    auto it = g_scancodeToVK.find(scancode);
    return (it != g_scancodeToVK.end()) ? it->second : 0;
}

} // namespace InputManager
