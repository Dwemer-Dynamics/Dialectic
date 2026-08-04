// InputManager.cpp - Event-driven hotkey input handling for Dialectic

#include "InputManager.h"
#include "Config.h"
#include "GameLoop.h"
#include "RuntimeSnapshot.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <array>
#include <atomic>
#include <chrono>

void Log(const char* fmt, ...);

namespace InputManager {
namespace {

constexpr std::size_t kActionCount = static_cast<std::size_t>(HotkeyAction::Count);
static_assert(kActionCount <= 32, "Hotkey pending-state mask must fit in uint32_t");

std::array<std::atomic<int>, kActionCount> g_hotkeyBindings{};
std::array<ULONGLONG, kActionCount> g_lastActionClaims{};
std::atomic<uint32_t> g_pendingActions{0};
std::atomic<uint32_t> g_releasedActions{0};
std::atomic<uint32_t> g_heldActions{0};

constexpr std::size_t ActionIndex(HotkeyAction action) {
    return static_cast<std::size_t>(action);
}

constexpr uint32_t ActionBit(HotkeyAction action) {
    return uint32_t{1} << ActionIndex(action);
}

const char* ActionName(HotkeyAction action) {
    switch (action) {
        case HotkeyAction::TalkToNPC: return "TalkToNPC";
        case HotkeyAction::StopTalking: return "StopTalking";
        case HotkeyAction::ToggleVoice: return "ToggleVoice";
        case HotkeyAction::OpenMicMute: return "OpenMicMute";
        case HotkeyAction::ManualActivateNPC: return "ManualActivateNPC";
        case HotkeyAction::OpenMenu: return "OpenMenu";
        case HotkeyAction::QuickCommand: return "QuickCommand";
        case HotkeyAction::DynamicProfileMenu: return "DynamicProfileMenu";
        case HotkeyAction::ToggleModes: return "ToggleModes";
        case HotkeyAction::ToggleLLMModel: return "ToggleLLMModel";
        case HotkeyAction::PipVision: return "PipVision";
        case HotkeyAction::Count: break;
    }
    return "Unknown";
}

bool IsRuntimeInputAllowed() {
    if (!IsGameForeground() || GameLoop::IsTextInputMenuActiveOrRecentlyClosed()) {
        return false;
    }

    RuntimeSnapshot::GameState state;
    if (!RuntimeSnapshot::TryGetFreshGameState(state, std::chrono::milliseconds(500))) {
        const GameLoop::GameState& fallback = GameLoop::GetGameState();
        return fallback.isInGame && !fallback.isPaused &&
            !fallback.isInDialogue && !fallback.isLoading;
    }

    return state.inGame && !state.paused && !state.pipboyOpen &&
        !state.pauseMenuOpen && !state.dialogueMenuOpen && !state.barterMenuOpen &&
        !state.containerMenuOpen && !state.loadingMenuOpen;
}

uint32_t MatchingActionMask(int scanCode) {
    if (scanCode <= 0) return 0;

    uint32_t mask = 0;
    for (std::size_t i = 0; i < kActionCount; ++i) {
        if (g_hotkeyBindings[i].load(std::memory_order_relaxed) == scanCode) {
            mask |= uint32_t{1} << i;
        }
    }
    return mask;
}

void WriteBinding(const char* key, HotkeyAction action) {
    char buffer[32] = {};
    sprintf_s(buffer, "%d", GetHotkey(action));
    WritePrivateProfileStringA("Hotkeys", key, buffer, Config::GetCustomINIPath());
}

} // namespace

void Initialize() {
    Log("InputManager: Initializing event-driven hotkeys");
    for (auto& binding : g_hotkeyBindings) binding.store(0, std::memory_order_relaxed);
    g_lastActionClaims.fill(0);
    g_pendingActions.store(0, std::memory_order_relaxed);
    g_releasedActions.store(0, std::memory_order_relaxed);
    g_heldActions.store(0, std::memory_order_relaxed);
    LoadConfig();
}

void Shutdown() {
    Log("InputManager: Shutting down");
    for (auto& binding : g_hotkeyBindings) binding.store(0, std::memory_order_relaxed);
    g_pendingActions.store(0, std::memory_order_relaxed);
    g_releasedActions.store(0, std::memory_order_relaxed);
    g_heldActions.store(0, std::memory_order_relaxed);
}

bool IsGameForeground() {
    const HWND foreground = GetForegroundWindow();
    if (!foreground) return false;

    DWORD processId = 0;
    GetWindowThreadProcessId(foreground, &processId);
    return processId != 0 && processId == GetCurrentProcessId();
}

void Update() {
    if (IsRuntimeInputAllowed()) return;

    g_pendingActions.store(0, std::memory_order_release);
    g_releasedActions.store(0, std::memory_order_release);
    g_heldActions.store(0, std::memory_order_release);
}

bool HandleScanCodeEvent(int scanCode, bool pressed) {
    const uint32_t matchedActions = MatchingActionMask(scanCode);
    if (matchedActions == 0) return false;

    if (!pressed) {
        g_heldActions.fetch_and(~matchedActions, std::memory_order_acq_rel);
        g_releasedActions.fetch_or(matchedActions, std::memory_order_acq_rel);
        return true;
    }

    if (!IsRuntimeInputAllowed()) {
        g_heldActions.fetch_and(~matchedActions, std::memory_order_acq_rel);
        Log("InputManager: ignored scan code %d while runtime input is blocked", scanCode);
        return false;
    }

    g_heldActions.fetch_or(matchedActions, std::memory_order_acq_rel);
    g_releasedActions.fetch_and(~matchedActions, std::memory_order_acq_rel);
    g_pendingActions.fetch_or(matchedActions, std::memory_order_acq_rel);

    for (std::size_t i = 0; i < kActionCount; ++i) {
        if ((matchedActions & (uint32_t{1} << i)) != 0) {
            Log("InputManager: queued %s from scan code %d",
                ActionName(static_cast<HotkeyAction>(i)), scanCode);
        }
    }
    return true;
}

bool IsActionTriggered(HotkeyAction action, uint32_t debounceMs) {
    const uint32_t bit = ActionBit(action);
    const uint32_t previous = g_pendingActions.fetch_and(~bit, std::memory_order_acq_rel);
    return (previous & bit) != 0 && TryClaimAction(action, debounceMs);
}

bool IsActionReleased(HotkeyAction action) {
    const uint32_t bit = ActionBit(action);
    const uint32_t previous = g_releasedActions.fetch_and(~bit, std::memory_order_acq_rel);
    return (previous & bit) != 0;
}

bool IsActionHeld(HotkeyAction action) {
    if (!IsGameForeground()) return false;
    return (g_heldActions.load(std::memory_order_acquire) & ActionBit(action)) != 0;
}

bool TryClaimAction(HotkeyAction action, uint32_t debounceMs) {
    if (!IsGameForeground()) return false;

    const std::size_t index = ActionIndex(action);
    const ULONGLONG now = GetTickCount64();
    if (g_lastActionClaims[index] != 0 && now - g_lastActionClaims[index] < debounceMs) {
        return false;
    }
    g_lastActionClaims[index] = now;
    return true;
}

bool IsScanCodeHeld(int scanCode) {
    if (!IsGameForeground()) return false;
    const uint32_t matchedActions = MatchingActionMask(scanCode);
    return matchedActions != 0 &&
        (g_heldActions.load(std::memory_order_acquire) & matchedActions) != 0;
}

void SetHotkey(HotkeyAction action, int scanCode) {
    g_hotkeyBindings[ActionIndex(action)].store(scanCode > 0 ? scanCode : 0,
                                                std::memory_order_release);
}

int GetHotkey(HotkeyAction action) {
    return g_hotkeyBindings[ActionIndex(action)].load(std::memory_order_acquire);
}

void LoadConfig() {
    SetHotkey(HotkeyAction::TalkToNPC, Config::ReadINIInt("Hotkeys", "TalkToNPC", 0));
    SetHotkey(HotkeyAction::StopTalking, Config::ReadINIInt("Hotkeys", "StopTalking", 0));
    SetHotkey(HotkeyAction::ToggleVoice, Config::ReadINIInt("Hotkeys", "ToggleVoice", 0));
    SetHotkey(HotkeyAction::OpenMicMute, Config::ReadINIInt("Hotkeys", "OpenMicMute", 0));
    SetHotkey(HotkeyAction::ManualActivateNPC, Config::ReadINIInt("Hotkeys", "ManualActivate", 0));
    SetHotkey(HotkeyAction::OpenMenu, Config::ReadINIInt("Hotkeys", "OpenMenu", 0));
    SetHotkey(HotkeyAction::QuickCommand, Config::ReadINIInt("Hotkeys", "QuickCommand", 0));
    SetHotkey(HotkeyAction::DynamicProfileMenu, Config::ReadINIInt("Hotkeys", "DynamicProfileMenu", 0));
    SetHotkey(HotkeyAction::ToggleModes, Config::ReadINIInt("Hotkeys", "ToggleModes", 0));
    SetHotkey(HotkeyAction::ToggleLLMModel, Config::ReadINIInt("Hotkeys", "ToggleLLMModel", 0));
    SetHotkey(HotkeyAction::PipVision, Config::ReadINIInt("Hotkeys", "PipVision", 0));

    g_pendingActions.store(0, std::memory_order_release);
    g_releasedActions.store(0, std::memory_order_release);
    g_heldActions.store(0, std::memory_order_release);

    Log("InputManager: active scan-code hotkeys Talk=%d Voice=%d OpenMicMute=%d Stop=%d Manual=%d Modes=%d LLM=%d Dynamic=%d PipVision=%d",
        GetHotkey(HotkeyAction::TalkToNPC),
        GetHotkey(HotkeyAction::ToggleVoice),
        GetHotkey(HotkeyAction::OpenMicMute),
        GetHotkey(HotkeyAction::StopTalking),
        GetHotkey(HotkeyAction::ManualActivateNPC),
        GetHotkey(HotkeyAction::ToggleModes),
        GetHotkey(HotkeyAction::ToggleLLMModel),
        GetHotkey(HotkeyAction::DynamicProfileMenu),
        GetHotkey(HotkeyAction::PipVision));
}

void SaveConfig() {
    WriteBinding("TalkToNPC", HotkeyAction::TalkToNPC);
    WriteBinding("StopTalking", HotkeyAction::StopTalking);
    WriteBinding("ToggleVoice", HotkeyAction::ToggleVoice);
    WriteBinding("OpenMicMute", HotkeyAction::OpenMicMute);
    WriteBinding("ManualActivate", HotkeyAction::ManualActivateNPC);
    WriteBinding("OpenMenu", HotkeyAction::OpenMenu);
    WriteBinding("QuickCommand", HotkeyAction::QuickCommand);
    WriteBinding("DynamicProfileMenu", HotkeyAction::DynamicProfileMenu);
    WriteBinding("ToggleModes", HotkeyAction::ToggleModes);
    WriteBinding("ToggleLLMModel", HotkeyAction::ToggleLLMModel);
    WriteBinding("PipVision", HotkeyAction::PipVision);
}

} // namespace InputManager
