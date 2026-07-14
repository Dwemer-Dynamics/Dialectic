// Dialectic - xNVSE plugin for AI-powered NPCs in Fallout New Vegas

// The xNVSE SDK prefix must be the first platform include. Several SDK
// headers intentionally depend on Windows aliases and min/max macros.
#include "nvse/prefix.h"
#include "nvse/PluginAPI.h"
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

// Windows headers
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// Standard library
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>

// Our logger
#include "Logger.h"
#include "XNVSEAdapter.h"

// Our subsystem headers
#include "Config.h"
#include "HTTPManager.h"
#include "AudioManager.h"
#include "SpeakManager.h"
#include "AgentManager.h"
#include "NPCDetector.h"
#include "InputManager.h"
#include "TargetManager.h"
#include "ActivationManager.h"
#include "GameLoop.h"
#include "WorldDataSyncFNV.h"
#include "VoiceSampleBatchUploadFNV.h"
#include "ImportDataSyncFNV.h"
#include "QuestJournalFNV.h"
#include "FNVRuntime.h"
#include "TaskManager.h"

#ifndef DIALECTIC_VERSION
#define DIALECTIC_VERSION "0.5.0"
#endif

#ifndef DIALECTIC_PLUGIN_INFO_VERSION
#define DIALECTIC_PLUGIN_INFO_VERSION 500
#endif

// Global variables
static PluginHandle g_pluginHandle = kPluginHandle_Invalid;
static bool g_initialized = false;
static bool g_subsystemsInitialized = false;
static NVSEScriptInterface* g_scriptInterface = nullptr;
static NVSEStringVarInterface* g_stringVarInterface = nullptr;
static std::atomic<bool> g_voiceSampleBatchRunning{ false };
static std::atomic<bool> g_importDataDetectionDone{ false };

static void DeleteBridgeFileEverywhere(const char* fileName) {
    if (!fileName || !fileName[0]) {
        return;
    }

    DeleteFileA((std::string("Data\\NVSE\\Plugins\\") + fileName).c_str());
    DeleteFileA((std::string("NVSE\\Plugins\\") + fileName).c_str());

    const char* localAppData = std::getenv("LOCALAPPDATA");
    if (localAppData && localAppData[0]) {
        DeleteFileA((std::string(localAppData) + "\\ModOrganizer\\Fallout TTW\\overwrite\\NVSE\\Plugins\\" + fileName).c_str());
        DeleteFileA((std::string(localAppData) + "\\ModOrganizer\\Fallout TTW\\overwrite\\Data\\NVSE\\Plugins\\" + fileName).c_str());
    }
}

static void ClearStartupBridgeState() {
    static constexpr const char* files[] = {
        "dialectic_bootstrap_active.tmp",
        "dialectic_action_request.tmp",
        "dialectic_action_status.txt",
        "dialectic_attack_state.tmp",
        "dialectic_attack_cleanup.tmp",
        "dialectic_follow_state.tmp",
        "dialectic_move_state.tmp",
        "dialectic_pickup_state.tmp",
        "dialectic_open_text_input.tmp",
        "dialectic_open_mode_menu.tmp",
        "dialectic_open_llm_model_menu.tmp",
        "dialectic_open_dynamic_profile_menu.tmp",
        "dialectic_hotkey_bridge_status.txt",
        "dialectic_textinput.tmp",
        "dialectic_textinput_status.tmp",
        "dialectic_mode_menu_status.tmp",
        "dialectic_llm_model_menu_status.tmp",
        "dialectic_dynamic_profile_menu_status.tmp",
        "dialectic_halt_actions.tmp",
        "dialectic_halt_actions_status.txt",
        "dialectic_subtitle.txt",
        "dialectic_subtitle_status.txt",
        "dialectic_lipsync_command.txt",
        "dialectic_lipsync_ref.txt",
        "dialectic_lipsync_status.txt"
    };

    for (const char* fileName : files) {
        DeleteBridgeFileEverywhere(fileName);
    }
}

void InitializeSubsystems();

static ParamInfo kParams_ConfigStringsFallbackInt[3] = {
    { "section", kParamType_String, 0 },
    { "key", kParamType_String, 0 },
    { "fallback", kParamType_Integer, 0 }
};

static ParamInfo kParams_ConfigStringsValueInt[3] = {
    { "section", kParamType_String, 0 },
    { "key", kParamType_String, 0 },
    { "value", kParamType_Integer, 0 }
};

static ParamInfo kParams_ConfigStringsFallbackFloat[3] = {
    { "section", kParamType_String, 0 },
    { "key", kParamType_String, 0 },
    { "fallback", kParamType_Float, 0 }
};

static ParamInfo kParams_ConfigStringsValueFloat[3] = {
    { "section", kParamType_String, 0 },
    { "key", kParamType_String, 0 },
    { "value", kParamType_Float, 0 }
};

static ParamInfo kParams_ConfigIdFallbackInt[2] = {
    { "setting id", kParamType_Integer, 0 },
    { "fallback", kParamType_Integer, 0 }
};

static ParamInfo kParams_ConfigIdValueInt[2] = {
    { "setting id", kParamType_Integer, 0 },
    { "value", kParamType_Integer, 0 }
};

static ParamInfo kParams_ConfigIdFallbackFloat[2] = {
    { "setting id", kParamType_Integer, 0 },
    { "fallback", kParamType_Float, 0 }
};

static ParamInfo kParams_ConfigIdValueFloat[2] = {
    { "setting id", kParamType_Integer, 0 },
    { "value", kParamType_Float, 0 }
};

static ParamInfo kParams_DialoguePromptCapture[2] = {
    { "speaker", kParamType_AnyForm, 0 },
    { "topic or info", kParamType_AnyForm, 0 }
};

static ParamInfo kParams_CapturedDialogue[9] = {
    { "source", kParamType_String, 0 },
    { "speaker", kParamType_String, 0 },
    { "speaker ref id", kParamType_String, 0 },
    { "target", kParamType_String, 0 },
    { "target ref id", kParamType_String, 0 },
    { "text", kParamType_StringVar, 0 },
    { "is player line", kParamType_Integer, 0 },
    { "menu mode", kParamType_Integer, 0 },
    { "capture id", kParamType_Integer, 0 }
};

static ParamInfo kParams_SetConfPayload[1] = {
    { "payload", kParamType_String, 0 }
};

static ParamInfo kParams_ActiveQuestUpdate[3] = {
    { "quest form id", kParamType_String, 0 },
    { "quest name", kParamType_String, 0 },
    { "quest editor id", kParamType_String, 0 }
};

static ParamInfo kParams_Integer[1] = {
    { "value", kParamType_Integer, 0 }
};

static std::string NormalizeConfigName(const char* value) {
    if (!value) {
        return "";
    }

    std::string normalized(value);
    normalized.erase(
        std::remove_if(normalized.begin(), normalized.end(), [](unsigned char c) {
            return c == ' ' || c == '_' || c == '-';
        }),
        normalized.end());
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return normalized;
}

static const char* CanonicalHotkeyKey(const std::string& normalizedKey) {
    if (normalizedKey == "talktonpc") { return "TalkToNPC"; }
    if (normalizedKey == "stoptalking") { return "StopTalking"; }
    if (normalizedKey == "togglevoice") { return "ToggleVoice"; }
    if (normalizedKey == "manualactivate" || normalizedKey == "manualactivatenpc") { return "ManualActivate"; }
    if (normalizedKey == "openmenu") { return "OpenMenu"; }
    if (normalizedKey == "quickcommand") { return "QuickCommand"; }
    if (normalizedKey == "dynamicprofilemenu") { return "DynamicProfileMenu"; }
    if (normalizedKey == "togglemodes") { return "ToggleModes"; }
    if (normalizedKey == "togglellmmodel") { return "ToggleLLMModel"; }
    if (normalizedKey == "openmicmute") { return "OpenMicMute"; }
    return nullptr;
}

static int GetRawHotkeyScanCode(const char* key) {
    if (!key || !*key) {
        return 0;
    }
    return Config::ReadINIInt("Hotkeys", key, 0);
}

static bool RawHotkeyMatches(const char* key, int scanCode) {
    const int configured = GetRawHotkeyScanCode(key);
    return configured > 0 && scanCode == configured;
}

static void WriteHotkeyDiagnostic(const char* handler, int scanCode, int configured, const char* status) {
    std::ofstream out("Data\\NVSE\\Plugins\\dialectic_hotkey_diagnostics.txt", std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return;
    }

    out << "handler=" << (handler ? handler : "") << "\n";
    out << "key=" << scanCode << "\n";
    out << "configured=" << configured << "\n";
    out << "status=" << (status ? status : "") << "\n";
}

static bool GetRawHotkeyConfigValue(const std::string& normalizedKey, double fallback, double& outValue) {
    const char* key = CanonicalHotkeyKey(normalizedKey);
    if (!key) {
        outValue = fallback;
        return false;
    }
    outValue = GetRawHotkeyScanCode(key);
    return true;
}

static bool SetRawHotkeyConfigValue(const std::string& normalizedKey, int scanCode) {
    const char* key = CanonicalHotkeyKey(normalizedKey);
    if (!key) {
        return false;
    }

    char buffer[32] = {};
    sprintf_s(buffer, "%d", scanCode);
    Config::WriteCustomINIValue("Hotkeys", key, buffer);
    InputManager::LoadConfig();
    return true;
}

static bool IsDuplicateHotkeyInvocation(int scanCode) {
    static int s_lastScanCode = 0;
    static DWORD s_lastTick = 0;

    const DWORD now = GetTickCount();
    if (scanCode == s_lastScanCode && (now - s_lastTick) < 250) {
        return true;
    }

    s_lastScanCode = scanCode;
    s_lastTick = now;
    return false;
}

static bool GetDialecticConfigValue(const char* section, const char* key, double fallback, double& outValue) {
    const std::string s = NormalizeConfigName(section);
    const std::string k = NormalizeConfigName(key);
    outValue = fallback;

    if (s == "hotkeys") {
        if (GetRawHotkeyConfigValue(k, fallback, outValue)) {
            return true;
        }
    }

    if (s == "hotkeys") {
        if (k == "togglevoice") {
            outValue = InputManager::GetHotkey(InputManager::HotkeyAction::ToggleVoice);
            return true;
        }
        if (k == "manualactivate" || k == "manualactivatenpc") {
            outValue = InputManager::GetHotkey(InputManager::HotkeyAction::ManualActivateNPC);
            return true;
        }
        if (k == "dynamicprofilemenu") {
            outValue = InputManager::GetHotkey(InputManager::HotkeyAction::DynamicProfileMenu);
            return true;
        }
        if (k == "togglemodes") {
            outValue = InputManager::GetHotkey(InputManager::HotkeyAction::ToggleModes);
            return true;
        }
        if (k == "togglellmmodel") {
            outValue = InputManager::GetHotkey(InputManager::HotkeyAction::ToggleLLMModel);
            return true;
        }
    }

    if (s == "audio" || s == "spatialaudio") {
        if (k == "voicevolume") { outValue = Config::voiceVolume; return true; }
        if (k == "preclipms") { outValue = Config::preClipMs; return true; }
        if (k == "postclipms") { outValue = Config::postClipMs; return true; }
        if (k == "animationresolution") { outValue = Config::animationResolution; return true; }
        if (k == "animationintensity") { outValue = Config::animationIntensity; return true; }
        if (k == "enable3dplayback" || k == "playback3d") { outValue = Config::audio3DPlaybackEnabled ? 1.0 : 0.0; return true; }
        if (k == "camerabasedaudio" || k == "camerabased") { outValue = Config::audioCameraBased ? 1.0 : 0.0; return true; }
        if (k == "panstrength" || k == "3dpanstrength") { outValue = Config::audio3DPanStrength; return true; }
        if (k == "invertheading") { outValue = Config::audioInvertHeading ? 1.0 : 0.0; return true; }
        if (k == "distancescale" || k == "voicedistancescale") { outValue = Config::audioDistanceScale; return true; }
        if (k == "playbackdropoffinteriorpercent") { outValue = Config::audioPlaybackDropoffInteriorPercent; return true; }
        if (k == "playbackdropoffexteriorpercent") { outValue = Config::audioPlaybackDropoffExteriorPercent; return true; }
        if (k == "interiorhearingdistance") { outValue = Config::spatialInteriorHearingDistance; return true; }
        if (k == "exteriorhearingdistance") { outValue = Config::spatialExteriorHearingDistance; return true; }
        if (k == "autohearingdistance") { outValue = Config::spatialAutoHearingDistance; return true; }
    }

    if (s == "behavior") {
        if (k == "enablecombatdialogue") { outValue = Config::enableCombatDialogue ? 1.0 : 0.0; return true; }
        if (k == "canceldialogueoncombat" || k == "cleardialogueenteringcombat") { outValue = Config::cancelDialogueOnCombat ? 1.0 : 0.0; return true; }
        if (k == "enablevoiceinput") { outValue = 1.0; return true; }
        if (k == "airesponsetimeout") { outValue = Config::aiResponseTimeout; return true; }
        if (k == "pausedialogueonmenu") { outValue = Config::pauseDialogueOnMenu ? 1.0 : 0.0; return true; }
        if (k == "suppressvanilladialogueduringai" || k == "pausedialogueduringvoice") { outValue = 1.0; return true; }
        if (k == "facetargetduringairesponse") { outValue = 1.0; return true; }
        if (k == "showaisubtitles" || k == "enableaisubtitles") { outValue = Config::showAISubtitles ? 1.0 : 0.0; return true; }
        if (k == "scenesafety" || k == "restrictonscene" || k == "_restrict_onscene") { outValue = Config::sceneSafetyEnabled ? 1.0 : 0.0; return true; }
    }

    if (s == "openmic" || s == "voicerecording") {
        if (k == "enabled" || k == "openmicenabled") { outValue = Config::openMicEnabled ? 1.0 : 0.0; return true; }
        if (k == "sensitivity" || k == "openmicsensitivity") { outValue = Config::openMicSensitivity; return true; }
        if (k == "enddelayseconds" || k == "enddelay" || k == "openmicenddelay") { outValue = Config::openMicEndDelaySeconds; return true; }
        if (k == "muted" || k == "openmicmuted") { outValue = Config::openMicMuted ? 1.0 : 0.0; return true; }
        if (k == "silencethreshold") { outValue = Config::silenceThreshold; return true; }
        if (k == "maxrecordingseconds") { outValue = Config::maxRecordingSeconds; return true; }
    }

    if (s == "distance") {
        if (k == "activatingnpcinterior") { outValue = Config::distanceActivatingNpcInterior; return true; }
        if (k == "activatingnpcexterior") { outValue = Config::distanceActivatingNpcExterior; return true; }
    }

    if (s == "autoactivate") {
        if (k == "enabled") { outValue = Config::autoActivateEnabled ? 1.0 : 0.0; return true; }
        if (k == "autoaddhostile") { outValue = Config::autoAddHostile ? 1.0 : 0.0; return true; }
        if (k == "autoaddcreatures") { outValue = Config::autoAddCreatures ? 1.0 : 0.0; return true; }
    }

    if (s == "rpgevents") {
        if (k == "combatbarksenabled") { outValue = Config::rpgCombatBarksEnabled ? 1.0 : 0.0; return true; }
        if (k == "combatbarkperiodseconds" || k == "combatbarksperiod") { outValue = Config::rpgCombatBarkPeriodSeconds; return true; }
    }

    if (s == "hotkeys") {
        if (k == "talktonpc") { outValue = InputManager::GetHotkey(InputManager::HotkeyAction::TalkToNPC); return true; }
        if (k == "stoptalking") { outValue = InputManager::GetHotkey(InputManager::HotkeyAction::StopTalking); return true; }
        if (k == "togglevoice") { outValue = InputManager::GetHotkey(InputManager::HotkeyAction::ToggleVoice); return true; }
        if (k == "openmicmute") { outValue = InputManager::GetHotkey(InputManager::HotkeyAction::OpenMicMute); return true; }
        if (k == "manualactivate" || k == "manualactivatenpc") { outValue = InputManager::GetHotkey(InputManager::HotkeyAction::ManualActivateNPC); return true; }
        if (k == "dynamicprofilemenu") { outValue = InputManager::GetHotkey(InputManager::HotkeyAction::DynamicProfileMenu); return true; }
        if (k == "togglemodes") { outValue = InputManager::GetHotkey(InputManager::HotkeyAction::ToggleModes); return true; }
        if (k == "togglellmmodel") { outValue = InputManager::GetHotkey(InputManager::HotkeyAction::ToggleLLMModel); return true; }
    }

    if (s == "rechat") {
        if (k == "enabled") { outValue = Config::rechatEnabled ? 1.0 : 0.0; return true; }
        if (k == "maxdepth") { outValue = Config::rechatMaxDepth; return true; }
        if (k == "smartlaunch") { outValue = Config::rechatSmartLaunch ? 1.0 : 0.0; return true; }
        if (k == "retryonempty") { outValue = Config::rechatRetryOnEmpty ? 1.0 : 0.0; return true; }
        if (k == "avoidwhensneaking") { outValue = Config::rechatAvoidWhenSneaking ? 1.0 : 0.0; return true; }
        if (k == "avoidinmenu") { outValue = Config::rechatAvoidInMenu ? 1.0 : 0.0; return true; }
        if (k == "endconversationcooldown") { outValue = Config::rechatEndConversationCooldown; return true; }
    }

    if (s == "dynamicprofile") {
        if (k == "enabled") { outValue = 1.0; return true; }
        if (k == "timerminutes" || k == "updateminutes") { outValue = Config::dynamicProfileTimerMinutes; return true; }
        if (k == "includenarrator") { outValue = Config::dynamicProfileTimerIncludeNarrator ? 1.0 : 0.0; return true; }
    }

    if (s == "boredevents") {
        if (k == "enabled") { outValue = 1.0; return true; }
        if (k == "timerseconds" || k == "boredeventtimerseconds") { outValue = Config::boredEventTimerSeconds; return true; }
        if (k == "avoidinmenu") { outValue = Config::boredAvoidInMenu ? 1.0 : 0.0; return true; }
        if (k == "avoidindialogue") { outValue = Config::boredAvoidInDialogue ? 1.0 : 0.0; return true; }
        if (k == "avoidincombat") { outValue = Config::boredAvoidInCombat ? 1.0 : 0.0; return true; }
        if (k == "avoidwhensneaking") { outValue = Config::boredAvoidWhenSneaking ? 1.0 : 0.0; return true; }
        if (k == "avoidwhenvoiceinputactive") { outValue = Config::boredAvoidWhenVoiceInputActive ? 1.0 : 0.0; return true; }
        if (k == "recentspeechcooldownseconds") { outValue = Config::boredRecentSpeechCooldownSeconds; return true; }
    }

    if (s == "narrator") {
        if (k == "mode") { outValue = Config::narratorModeEnabled ? 1.0 : 0.0; return true; }
    }

    if (s == "modes") {
        if (k == "currentindex") { outValue = Config::currentModeIndex; return true; }
    }

    return false;
}

static bool SetDialecticConfigValue(const char* section, const char* key, double value) {
    const std::string s = NormalizeConfigName(section);
    const std::string k = NormalizeConfigName(key);
    const bool enabled = value != 0.0;
    bool changed = false;
    bool hotkeyChanged = false;

    if (s == "hotkeys") {
        const int scanCode = static_cast<int>(value);
        if (!SetRawHotkeyConfigValue(k, scanCode)) {
            return false;
        }
        Logger::LogInfo("MCM hotkey updated: [%s] %s = %d", section ? section : "", key ? key : "", scanCode);
        return true;
    } else if (s == "audio" || s == "spatialaudio") {
        if (k == "voicevolume") { Config::voiceVolume = static_cast<float>(value); changed = true; }
        else if (k == "preclipms") { Config::preClipMs = static_cast<float>(value); changed = true; }
        else if (k == "postclipms") { Config::postClipMs = static_cast<float>(value); changed = true; }
        else if (k == "animationresolution") { Config::animationResolution = static_cast<int>(value); changed = true; }
        else if (k == "animationintensity") { Config::animationIntensity = static_cast<float>(value); changed = true; }
        else if (k == "enable3dplayback" || k == "playback3d") { Config::audio3DPlaybackEnabled = enabled; changed = true; }
        else if (k == "camerabasedaudio" || k == "camerabased") { Config::audioCameraBased = enabled; changed = true; }
        else if (k == "playback2d" || k == "force2d") { Config::audio3DPlaybackEnabled = !enabled; changed = true; }
        else if (k == "panstrength" || k == "3dpanstrength") { Config::audio3DPanStrength = static_cast<float>(value); changed = true; }
        else if (k == "invertheading") { Config::audioInvertHeading = enabled; changed = true; }
        else if (k == "distancescale" || k == "voicedistancescale") { Config::audioDistanceScale = static_cast<float>(value); changed = true; }
        else if (k == "playbackdropoffinteriorpercent") { Config::audioPlaybackDropoffInteriorPercent = static_cast<float>(value); changed = true; }
        else if (k == "playbackdropoffexteriorpercent") { Config::audioPlaybackDropoffExteriorPercent = static_cast<float>(value); changed = true; }
        else if (k == "interiorhearingdistance") { Config::spatialInteriorHearingDistance = static_cast<float>(value); changed = true; }
        else if (k == "exteriorhearingdistance") { Config::spatialExteriorHearingDistance = static_cast<float>(value); changed = true; }
        else if (k == "autohearingdistance") { Config::spatialAutoHearingDistance = static_cast<float>(value); changed = true; }
    } else if (s == "behavior") {
        if (k == "enablecombatdialogue") { Config::enableCombatDialogue = enabled; changed = true; }
        else if (k == "canceldialogueoncombat" || k == "cleardialogueenteringcombat") { Config::cancelDialogueOnCombat = enabled; changed = true; }
        else if (k == "enablevoiceinput") { return true; }
        else if (k == "airesponsetimeout") { Config::aiResponseTimeout = static_cast<int>(value); changed = true; }
        else if (k == "pausedialogueonmenu") { Config::pauseDialogueOnMenu = enabled; changed = true; }
        else if (k == "suppressvanilladialogueduringai" || k == "pausedialogueduringvoice") { Config::suppressVanillaDialogueDuringAI = true; return true; }
        else if (k == "facetargetduringairesponse") { Config::faceTargetDuringAIResponse = true; return true; }
        else if (k == "showaisubtitles" || k == "enableaisubtitles") { Config::showAISubtitles = enabled; changed = true; }
        else if (k == "scenesafety" || k == "restrictonscene" || k == "_restrict_onscene") { Config::sceneSafetyEnabled = enabled; changed = true; }
    } else if (s == "openmic" || s == "voicerecording") {
        if (k == "enabled" || k == "openmicenabled") { Config::openMicEnabled = enabled; changed = true; }
        else if (k == "sensitivity" || k == "openmicsensitivity") { Config::openMicSensitivity = static_cast<float>(std::max(1.0, value)); changed = true; }
        else if (k == "enddelayseconds" || k == "enddelay" || k == "openmicenddelay") { Config::openMicEndDelaySeconds = static_cast<float>(std::max(0.1, value)); changed = true; }
        else if (k == "muted" || k == "openmicmuted") { Config::openMicMuted = enabled; changed = true; }
        else if (k == "silencethreshold") { Config::silenceThreshold = static_cast<int>(std::max(1.0, value)); changed = true; }
        else if (k == "maxrecordingseconds") { Config::maxRecordingSeconds = static_cast<int>(std::max(1.0, value)); changed = true; }
    } else if (s == "distance") {
        if (k == "activatingnpcinterior") { Config::distanceActivatingNpcInterior = static_cast<float>(value); changed = true; }
        else if (k == "activatingnpcexterior") { Config::distanceActivatingNpcExterior = static_cast<float>(value); changed = true; }
    } else if (s == "autoactivate") {
        if (k == "enabled") { Config::autoActivateEnabled = enabled; changed = true; }
        else if (k == "autoaddhostile") { Config::autoAddHostile = enabled; changed = true; }
        else if (k == "autoaddcreatures") { Config::autoAddCreatures = enabled; changed = true; }
    } else if (s == "rpgevents") {
        if (k == "combatbarksenabled") { Config::rpgCombatBarksEnabled = enabled; changed = true; }
        else if (k == "combatbarkperiodseconds" || k == "combatbarksperiod") {
            Config::rpgCombatBarkPeriodSeconds = std::max(5, static_cast<int>(value));
            changed = true;
        }
    } else if (s == "rechat") {
        if (k == "enabled") { Config::rechatEnabled = enabled; changed = true; }
        else if (k == "maxdepth") { Config::rechatMaxDepth = static_cast<int>(value); changed = true; }
        else if (k == "smartlaunch") { Config::rechatSmartLaunch = enabled; changed = true; }
        else if (k == "retryonempty") { Config::rechatRetryOnEmpty = enabled; changed = true; }
        else if (k == "avoidwhensneaking") { Config::rechatAvoidWhenSneaking = enabled; changed = true; }
        else if (k == "avoidinmenu") { Config::rechatAvoidInMenu = enabled; changed = true; }
        else if (k == "endconversationcooldown") { Config::rechatEndConversationCooldown = static_cast<int>(value); changed = true; }
    } else if (s == "dynamicprofile") {
        if (k == "timerminutes" || k == "updateminutes") { Config::dynamicProfileTimerMinutes = std::max(1, static_cast<int>(value)); changed = true; }
        else if (k == "includenarrator") { Config::dynamicProfileTimerIncludeNarrator = enabled; changed = true; }
    } else if (s == "boredevents") {
        if (k == "enabled") { Config::boredEventsEnabled = true; return true; }
        else if (k == "timerseconds" || k == "boredeventtimerseconds") { Config::boredEventTimerSeconds = std::max(5, static_cast<int>(value)); changed = true; }
        else if (k == "avoidinmenu") { Config::boredAvoidInMenu = enabled; changed = true; }
        else if (k == "avoidindialogue") { Config::boredAvoidInDialogue = enabled; changed = true; }
        else if (k == "avoidincombat") { Config::boredAvoidInCombat = enabled; changed = true; }
        else if (k == "avoidwhensneaking") { Config::boredAvoidWhenSneaking = enabled; changed = true; }
        else if (k == "avoidwhenvoiceinputactive") { Config::boredAvoidWhenVoiceInputActive = enabled; changed = true; }
        else if (k == "recentspeechcooldownseconds") { Config::boredRecentSpeechCooldownSeconds = std::max(0, static_cast<int>(value)); changed = true; }
    } else if (s == "narrator") {
        if (k == "mode") { Config::narratorModeEnabled = enabled; changed = true; }
    } else if (s == "modes") {
        if (k == "currentindex") { Config::currentModeIndex = std::clamp(static_cast<int>(value), 0, 7); changed = true; }
    }

    if (!changed) {
        return false;
    }

    if (hotkeyChanged) {
        InputManager::SaveConfig();
    } else {
        Config::Save();
    }

    Logger::LogInfo("MCM config updated: [%s] %s = %.3f", section ? section : "", key ? key : "", value);
    return true;
}

struct DialecticSettingRef {
    const char* section;
    const char* key;
};

static bool ResolveDialecticSettingId(int settingId, DialecticSettingRef& outSetting) {
    switch (settingId) {
        case 1: outSetting = { "AutoActivate", "Enabled" }; return true;
        case 2: outSetting = { "AutoActivate", "AutoAddHostile" }; return true;
        case 3: outSetting = { "AutoActivate", "AutoAddCreatures" }; return true;
        case 17: outSetting = { "Behavior", "ShowAISubtitles" }; return true;
        case 13: outSetting = { "OpenMic", "Enabled" }; return true;
        case 14: outSetting = { "OpenMic", "Sensitivity" }; return true;
        case 15: outSetting = { "OpenMic", "EndDelaySeconds" }; return true;
        case 16: outSetting = { "OpenMic", "Muted" }; return true;
        case 20: outSetting = { "Rechat", "Enabled" }; return true;
        case 21: outSetting = { "Rechat", "SmartLaunch" }; return true;
        case 22: outSetting = { "Rechat", "RetryOnEmpty" }; return true;
        case 23: outSetting = { "Rechat", "AvoidWhenSneaking" }; return true;
        case 24: outSetting = { "Rechat", "AvoidInMenu" }; return true;
        case 25: outSetting = { "Rechat", "MaxDepth" }; return true;
        case 26: outSetting = { "Rechat", "EndConversationCooldown" }; return true;
        case 31: outSetting = { "DynamicProfile", "IncludeNarrator" }; return true;
        case 32: outSetting = { "DynamicProfile", "TimerMinutes" }; return true;
        case 33: outSetting = { "Narrator", "Mode" }; return true;
        case 35: outSetting = { "BoredEvents", "TimerSeconds" }; return true;
        case 40: outSetting = { "Hotkeys", "ManualActivate" }; return true;
        case 41: outSetting = { "Hotkeys", "ToggleVoice" }; return true;
        case 42: outSetting = { "Hotkeys", "DynamicProfileMenu" }; return true;
        case 45: outSetting = { "Hotkeys", "ToggleModes" }; return true;
        case 48: outSetting = { "Hotkeys", "ToggleLLMModel" }; return true;
        case 49: outSetting = { "Hotkeys", "OpenMicMute" }; return true;
        case 46: outSetting = { "Hotkeys", "TalkToNPC" }; return true;
        case 47: outSetting = { "Hotkeys", "StopTalking" }; return true;
        case 50: outSetting = { "Audio", "VoiceVolume" }; return true;
        case 51: outSetting = { "Audio", "PanStrength" }; return true;
        case 52: outSetting = { "Audio", "DistanceScale" }; return true;
        case 53: outSetting = { "Audio", "CameraBasedAudio" }; return true;
        case 60: outSetting = { "Distance", "ActivatingNpcInterior" }; return true;
        case 61: outSetting = { "Distance", "ActivatingNpcExterior" }; return true;
        case 62: outSetting = { "SpatialAudio", "InteriorHearingDistance" }; return true;
        case 63: outSetting = { "SpatialAudio", "ExteriorHearingDistance" }; return true;
        case 64: outSetting = { "SpatialAudio", "AutoHearingDistance" }; return true;
        default:
            outSetting = { "", "" };
            return false;
    }
}

static bool GetConfigById(int settingId, double fallback, double& outValue) {
    DialecticSettingRef setting;
    if (!ResolveDialecticSettingId(settingId, setting)) {
        outValue = fallback;
        Logger::LogWarning("Unknown Dialectic MCM setting id: %d", settingId);
        return false;
    }
    return GetDialecticConfigValue(setting.section, setting.key, fallback, outValue);
}

static bool SetConfigById(int settingId, double value) {
    DialecticSettingRef setting;
    if (!ResolveDialecticSettingId(settingId, setting)) {
        Logger::LogWarning("Unknown Dialectic MCM setting id: %d", settingId);
        return false;
    }
    return SetDialecticConfigValue(setting.section, setting.key, value);
}

static bool ExtractConfigIntArgs(COMMAND_ARGS, char* section, char* key, int* value) {
    if (!g_scriptInterface || !g_scriptInterface->ExtractArgsEx) {
        Logger::LogWarning("Dialectic config command called before script interface was available");
        return false;
    }
    return g_scriptInterface->ExtractArgsEx(paramInfo, scriptData, opcodeOffsetPtr, scriptObj, eventList, section, key, value);
}

static bool ExtractConfigIdIntArgs(COMMAND_ARGS, int* settingId, int* value) {
    if (!g_scriptInterface || !g_scriptInterface->ExtractArgsEx) {
        Logger::LogWarning("Dialectic config command called before script interface was available");
        return false;
    }
    return g_scriptInterface->ExtractArgsEx(paramInfo, scriptData, opcodeOffsetPtr, scriptObj, eventList, settingId, value);
}

static bool ExtractConfigIdFloatArgs(COMMAND_ARGS, int* settingId, float* value) {
    if (!g_scriptInterface || !g_scriptInterface->ExtractArgsEx) {
        Logger::LogWarning("Dialectic config command called before script interface was available");
        return false;
    }
    return g_scriptInterface->ExtractArgsEx(paramInfo, scriptData, opcodeOffsetPtr, scriptObj, eventList, settingId, value);
}

static bool ExtractConfigFloatArgs(COMMAND_ARGS, char* section, char* key, float* value) {
    if (!g_scriptInterface || !g_scriptInterface->ExtractArgsEx) {
        Logger::LogWarning("Dialectic config command called before script interface was available");
        return false;
    }
    return g_scriptInterface->ExtractArgsEx(paramInfo, scriptData, opcodeOffsetPtr, scriptObj, eventList, section, key, value);
}

static bool ExtractDialoguePromptCaptureArgs(
    COMMAND_ARGS,
    void** speakerRef,
    void** topicOrInfo) {
    if (!g_scriptInterface || !g_scriptInterface->ExtractArgsEx) {
        Logger::LogWarning("Dialectic dialogue prompt command called before script interface was available");
        return false;
    }

    return g_scriptInterface->ExtractArgsEx(
        paramInfo,
        scriptData,
        opcodeOffsetPtr,
        scriptObj,
        eventList,
        speakerRef,
        topicOrInfo);
}

static bool ExtractCapturedDialogueArgs(COMMAND_ARGS,
                                         char* source,
                                         char* speaker,
                                         char* speakerRefId,
                                         char* target,
                                         char* targetRefId,
                                         UInt32* textStringId,
                                         int* isPlayerLine,
                                         int* menuMode,
                                         int* captureId) {
    if (!g_scriptInterface || !g_scriptInterface->ExtractArgsEx) return false;
    return g_scriptInterface->ExtractArgsEx(
        paramInfo, scriptData, opcodeOffsetPtr, scriptObj, eventList,
        source, speaker, speakerRefId, target, targetRefId, textStringId,
        isPlayerLine, menuMode, captureId);
}

static bool ExtractSetConfPayloadArgs(COMMAND_ARGS, char* payload) {
    if (!g_scriptInterface || !g_scriptInterface->ExtractArgsEx) {
        Logger::LogWarning("Dialectic setconf command called before script interface was available");
        return false;
    }

    return g_scriptInterface->ExtractArgsEx(
        paramInfo,
        scriptData,
        opcodeOffsetPtr,
        scriptObj,
        eventList,
        payload);
}

static bool ExtractActiveQuestUpdateArgs(COMMAND_ARGS, char* formId, char* name, char* editorId) {
    if (!g_scriptInterface || !g_scriptInterface->ExtractArgsEx) {
        Logger::LogWarning("Dialectic active quest command called before script interface was available");
        return false;
    }

    return g_scriptInterface->ExtractArgsEx(
        paramInfo,
        scriptData,
        opcodeOffsetPtr,
        scriptObj,
        eventList,
        formId,
        name,
        editorId);
}

static bool ExtractIntegerArgs(COMMAND_ARGS, int* value) {
    if (!g_scriptInterface || !g_scriptInterface->ExtractArgsEx) {
        return false;
    }

    return g_scriptInterface->ExtractArgsEx(paramInfo, scriptData, opcodeOffsetPtr, scriptObj, eventList, value);
}

static std::string TrimBridgeValue(std::string value) {
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char c) {
        return !std::isspace(c);
    }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char c) {
        return !std::isspace(c);
    }).base(), value.end());
    return value;
}

static std::string NormalizeBridgeValue(const char* rawValue) {
    std::string value = rawValue ? rawValue : "";
    for (char& ch : value) {
        if (ch == '\r' || ch == '\n' || ch == '\0') {
            ch = ' ';
        }
    }
    return TrimBridgeValue(value);
}

static std::string NormalizeBridgeValue(std::string value) {
    for (char& ch : value) {
        if (ch == '\r' || ch == '\n' || ch == '\0') {
            ch = ' ';
        }
    }
    return TrimBridgeValue(value);
}

static std::string EscapeBridgeJson(const std::string& input);

static std::string NormalizeSetConfPayload(std::string payload) {
    payload = NormalizeBridgeValue(std::move(payload));
    if (payload.empty()) {
        return "";
    }

    const char first = payload.front();
    if (first == '{' || first == '[') {
        return payload;
    }

    const size_t atPos = payload.find('@');
    if (atPos == std::string::npos) {
        return payload;
    }

    const std::string setting = TrimBridgeValue(payload.substr(0, atPos));
    const std::string value = TrimBridgeValue(payload.substr(atPos + 1));
    if (setting.empty()) {
        return payload;
    }

    std::ostringstream json;
    json << "{"
         << "\"schema\":\"dialectic.setconf.v1\","
         << "\"setting\":\"" << EscapeBridgeJson(setting) << "\","
         << "\"value\":\"" << EscapeBridgeJson(value) << "\""
         << "}";
    return json.str();
}

static std::string FormatHex(UInt32 value) {
    std::ostringstream out;
    out << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << value;
    return out.str();
}

static std::string EscapeBridgeJson(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (char c : input) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

static void WriteTextFile(const std::string& path, const std::string& data, bool warnOnFailure = true) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        if (warnOnFailure) {
            Logger::LogWarning("Failed to write %s", path.c_str());
        }
        return;
    }

    file << data;
}

static bool WriteDialoguePromptCapture(
    const std::string& prompt,
    const std::string& speakerName,
    const std::string& speakerRefId,
    const std::string& targetName,
    const std::string& targetRefId,
    UInt32 topicFormId,
    const std::string& promptSource,
    UInt32 parentTopicFormId) {
    if (prompt.empty()) {
        return false;
    }

    const auto captureId = GetTickCount64();
    std::ostringstream id;
    id << "topic_prompt_" << FormatHex(topicFormId) << "_" << captureId;
    GameLoop::SubmitCapturedDialogue(
        "dialogue_menu_player",
        speakerName,
        speakerRefId,
        targetName,
        targetRefId,
        prompt,
        true,
        true,
        id.str());
    Logger::LogDebug(
        "Queued native player dialogue prompt topic=0x%08X parent=0x%08X source=%s",
        topicFormId,
        parentTopicFormId,
        promptSource.c_str());
    return true;
}

static bool Cmd_DialecticCaptureDialoguePrompt_Execute(COMMAND_ARGS) {
    void* speakerRef = nullptr;
    void* topicOrInfo = nullptr;
    *result = 0;

    if (!ExtractDialoguePromptCaptureArgs(
              PASS_COMMAND_ARGS,
              &speakerRef,
              &topicOrInfo)) {
        WriteTextFile(
            "Data\\NVSE\\Plugins\\dialectic_dialogue_prompt_debug.tmp",
            "source=topic_prompt_command\nstate=extract_args_failed\n");
        Logger::LogWarning("Dialectic dialogue prompt command failed to extract arguments");
        return true;
    }

    XNVSEAdapter::NativeDialoguePrompt capture;
    const void* targetRef = speakerRef ? speakerRef : thisObj;
    XNVSEAdapter::CaptureNativeDialoguePrompt(targetRef, topicOrInfo, capture);

    std::ostringstream debug;
    debug << "source=topic_prompt_command\n";
    debug << "topic_refid=" << FormatHex(capture.topicFormId) << "\n";
    debug << "parent_topic_refid=" << FormatHex(capture.parentTopicFormId) << "\n";
    debug << "form_type=0x" << std::uppercase << std::hex << std::setw(2) << std::setfill('0')
          << static_cast<int>(capture.formType) << "\n";
    debug << "prompt_source=" << capture.source << "\n";
    debug << "prompt=" << capture.prompt << "\n";

    if (capture.prompt.empty()) {
        debug << "state=empty_prompt\n";
        WriteTextFile("Data\\NVSE\\Plugins\\dialectic_dialogue_prompt_debug.tmp", debug.str());
        Logger::LogInfo("Dialogue prompt capture found no prompt for topic/info 0x%08X type=0x%02X",
            capture.topicFormId, capture.formType);
        return true;
    }

    std::string targetName = capture.targetName.empty() ? "Unknown" : capture.targetName;
    uint32_t targetFormId = capture.targetFormId;
    if (targetFormId == 0x00000014 || _stricmp(targetName.c_str(), "Player") == 0) {
        const auto& currentTarget = TargetManager::GetCurrentTarget();
        if (currentTarget.formId != 0 && currentTarget.isActor && !currentTarget.name.empty()) {
            targetName = currentTarget.name;
            targetFormId = currentTarget.formId;
        }
    }

    const bool emitted = WriteDialoguePromptCapture(
        capture.prompt,
        "Player",
        "00000014",
        targetName,
        FormatHex(targetFormId),
        capture.topicFormId,
        capture.source,
        capture.parentTopicFormId);

    debug << "state=" << (emitted ? "emit_prompt" : "write_failed") << "\n";
    debug << "speaker=Player\n";
    debug << "target=" << targetName << "\n";
    debug << "target_refid=" << FormatHex(targetFormId) << "\n";
    WriteTextFile("Data\\NVSE\\Plugins\\dialectic_dialogue_prompt_debug.tmp", debug.str());

    if (emitted) {
        Logger::LogInfo("Captured dialogue menu player prompt via topic info 0x%08X: %s",
            capture.topicFormId, capture.prompt.c_str());
        *result = 1;
    }

    return true;
}

static bool Cmd_DialecticCaptureDialogue_Execute(COMMAND_ARGS) {
    char source[64] = {};
    char speaker[256] = {};
    char speakerRefId[32] = {};
    char target[256] = {};
    char targetRefId[32] = {};
    UInt32 textStringId = 0;
    int captureId = 0;
    int isPlayerLine = 0;
    int menuMode = 0;
    *result = 0;

    if (!ExtractCapturedDialogueArgs(
            PASS_COMMAND_ARGS,
            source, speaker, speakerRefId, target, targetRefId, &textStringId,
            &isPlayerLine, &menuMode, &captureId)) {
        Logger::LogWarning("DialecticCaptureDialogue failed to extract arguments");
        return true;
    }
    if (!g_stringVarInterface || !g_stringVarInterface->GetString) {
        Logger::LogWarning("DialecticCaptureDialogue called without the NVSE string variable interface");
        return true;
    }
    const char* text = g_stringVarInterface->GetString(textStringId);
    if (!text || !text[0]) {
        Logger::LogWarning("DialecticCaptureDialogue received an empty text string variable");
        return true;
    }
    if (!g_subsystemsInitialized) InitializeSubsystems();
    GameLoop::SubmitCapturedDialogue(
        source, speaker, speakerRefId, target, targetRefId, text,
        isPlayerLine != 0, menuMode != 0, std::to_string(captureId));
    *result = 1;
    return true;
}

static bool Cmd_DialecticGetConfigInt_Execute(COMMAND_ARGS) {
    char section[256] = { 0 };
    char key[256] = { 0 };
    int fallback = 0;
    *result = 0;

    if (ExtractConfigIntArgs(PASS_COMMAND_ARGS, section, key, &fallback)) {
        if (!g_subsystemsInitialized) {
            InitializeSubsystems();
        }
        double value = fallback;
        *result = GetDialecticConfigValue(section, key, fallback, value) ? static_cast<int>(value) : fallback;
    }
    return true;
}

static bool Cmd_DialecticSetConfigInt_Execute(COMMAND_ARGS) {
    char section[256] = { 0 };
    char key[256] = { 0 };
    int value = 0;
    *result = 0;

    if (ExtractConfigIntArgs(PASS_COMMAND_ARGS, section, key, &value)) {
        if (!g_subsystemsInitialized) {
            InitializeSubsystems();
        }
        *result = SetDialecticConfigValue(section, key, value) ? 1 : 0;
    }
    return true;
}

static bool Cmd_DialecticGetConfigFloat_Execute(COMMAND_ARGS) {
    char section[256] = { 0 };
    char key[256] = { 0 };
    float fallback = 0.0f;
    *result = 0.0;

    if (ExtractConfigFloatArgs(PASS_COMMAND_ARGS, section, key, &fallback)) {
        if (!g_subsystemsInitialized) {
            InitializeSubsystems();
        }
        double value = fallback;
        *result = GetDialecticConfigValue(section, key, fallback, value) ? value : fallback;
    }
    return true;
}

static bool Cmd_DialecticSetConfigFloat_Execute(COMMAND_ARGS) {
    char section[256] = { 0 };
    char key[256] = { 0 };
    float value = 0.0f;
    *result = 0;

    if (ExtractConfigFloatArgs(PASS_COMMAND_ARGS, section, key, &value)) {
        if (!g_subsystemsInitialized) {
            InitializeSubsystems();
        }
        *result = SetDialecticConfigValue(section, key, value) ? 1 : 0;
    }
    return true;
}

static bool Cmd_DialecticReloadConfig_Execute(COMMAND_ARGS) {
    Config::Load();
    if (g_subsystemsInitialized) {
        InputManager::LoadConfig();
    }
    Logger::LogInfo("Dialectic config reloaded from NVSE command");
    *result = 1;
    return true;
}

static bool Cmd_DialecticSaveConfig_Execute(COMMAND_ARGS) {
    Config::Save();
    if (g_subsystemsInitialized) {
        InputManager::SaveConfig();
    }
    Logger::LogInfo("Dialectic config saved from NVSE command");
    *result = 1;
    return true;
}

static bool Cmd_DialecticSyncWorldData_Execute(COMMAND_ARGS) {
    if (!g_subsystemsInitialized) {
        InitializeSubsystems();
    }
    WorldDataSyncFNV::RequestSync();
    *result = 1;
    return true;
}

bool Dialectic_RequestVoiceSampleBatch(const char* source) {
    if (!g_subsystemsInitialized) {
        InitializeSubsystems();
    }

    bool expected = false;
    if (!g_voiceSampleBatchRunning.compare_exchange_strong(expected, true)) {
        Logger::LogInfo("DialecticSendAllVoiceSamples ignored from %s; a voice sample batch is already running",
            source ? source : "unknown");
        return true;
    }

    if (TaskManager::Enqueue("voice_sample_batch", "all_voice_samples", 0, false,
        std::chrono::minutes(15), [](const TaskManager::CancellationToken& token) {
        VoiceSampleBatchUploadFNV::BatchUploadSummary summary;
        const auto uploadResult = VoiceSampleBatchUploadFNV::SendAllVoiceSamples(summary,
            [&token]() { return token.IsCancellationRequested(); });
        Logger::LogInfo(
            "DialecticSendAllVoiceSamples result=%d mappings=%d uploaded=%d missing=%d failed=%d timedOut=%d",
            static_cast<int>(uploadResult),
            summary.totalMappings,
            summary.uploaded,
            summary.missing,
            summary.failed,
            summary.timedOut ? 1 : 0);
        g_voiceSampleBatchRunning = false;
    }) == 0) {
        g_voiceSampleBatchRunning = false;
        Logger::LogWarning("DialecticSendAllVoiceSamples could not queue background task");
        return false;
    }

    Logger::LogInfo("DialecticSendAllVoiceSamples started in background from %s",
        source ? source : "unknown");
    return true;
}

static bool Cmd_DialecticSendAllVoiceSamples_Execute(COMMAND_ARGS) {
    Dialectic_RequestVoiceSampleBatch("NVSE command");
    *result = 1;
    return true;
}

static bool Cmd_DialecticSendSetConf_Execute(COMMAND_ARGS) {
    char payloadBuffer[1024] = {};
    *result = 0;

    if (!ExtractSetConfPayloadArgs(PASS_COMMAND_ARGS, payloadBuffer)) {
        return true;
    }

    std::string payload = NormalizeSetConfPayload(payloadBuffer);
    if (payload.empty()) {
        Logger::LogWarning("DialecticSendSetConf called with empty payload");
        return true;
    }

    if (!g_subsystemsInitialized) {
        InitializeSubsystems();
    }

    Logger::LogInfo("DialecticSendSetConf: %s", payload.c_str());
    HTTPManager::SendEvent("setconf", payload);
    *result = 1;
    return true;
}

static bool Cmd_DialecticUpdateActiveQuest_Execute(COMMAND_ARGS) {
    char formId[256] = {};
    char name[512] = {};
    char editorId[256] = {};
    *result = 0;

    if (!ExtractActiveQuestUpdateArgs(PASS_COMMAND_ARGS, formId, name, editorId)) {
        Logger::LogWarning("DialecticUpdateActiveQuest failed to extract arguments");
        return true;
    }

    if (!g_subsystemsInitialized) {
        InitializeSubsystems();
    }

    const std::string normalizedFormId = NormalizeBridgeValue(formId);
    const std::string normalizedName = NormalizeBridgeValue(name);
    const std::string normalizedEditorId = NormalizeBridgeValue(editorId);
    Logger::LogInfo(
        "DialecticUpdateActiveQuest form=%s name=%s editor=%s",
        normalizedFormId.c_str(),
        normalizedName.c_str(),
        normalizedEditorId.c_str());
    QuestJournalFNV::UpdateActiveQuestFromScript(
        normalizedFormId.c_str(),
        normalizedName.c_str(),
        normalizedEditorId.c_str());
    *result = 1;
    return true;
}

static bool Cmd_DialecticOpenModeMenu_Execute(COMMAND_ARGS) {
    if (!g_subsystemsInitialized) {
        InitializeSubsystems();
    }
    GameLoop::RequestModeMenuOpen();
    *result = 1;
    return true;
}

static bool Cmd_DialecticOpenLLMModelMenu_Execute(COMMAND_ARGS) {
    if (!g_subsystemsInitialized) {
        InitializeSubsystems();
    }
    GameLoop::RequestLLMModelMenuOpen();
    *result = 1;
    return true;
}

static bool Cmd_DialecticOpenDynamicProfileMenu_Execute(COMMAND_ARGS) {
    if (!g_subsystemsInitialized) {
        InitializeSubsystems();
    }
    GameLoop::RequestDynamicProfileMenuOpen();
    *result = 1;
    return true;
}

static bool Cmd_DialecticManageAIAgents_Execute(COMMAND_ARGS) {
    int action = -1;
    *result = 0;
    if (!ExtractIntegerArgs(PASS_COMMAND_ARGS, &action)) {
        Logger::LogWarning("DialecticManageAIAgents failed to extract action");
        return true;
    }
    if (!g_subsystemsInitialized) {
        InitializeSubsystems();
    }
    GameLoop::ManageAIAgents(action);
    *result = 1;
    return true;
}

static bool Cmd_DialecticHandleHaltHotkey_Execute(COMMAND_ARGS) {
    int scanCode = 0;
    *result = 0;

    if (!ExtractIntegerArgs(PASS_COMMAND_ARGS, &scanCode)) {
        WriteHotkeyDiagnostic("halt", 0, GetRawHotkeyScanCode("StopTalking"), "extract_failed");
        return true;
    }

    const int configured = GetRawHotkeyScanCode("StopTalking");
    Logger::LogInfo("GameLoop: Halt hotkey bridge scan=%d configured=%d", scanCode, configured);

    if (scanCode <= 0) {
        WriteHotkeyDiagnostic("halt", scanCode, configured, "invalid_key");
        return true;
    }

    if (configured <= 0) {
        WriteHotkeyDiagnostic("halt", scanCode, configured, "unbound");
        Logger::LogInfo("GameLoop: Halt AI Actions ignored because StopTalking is unbound");
        return true;
    }

    if (scanCode != configured) {
        WriteHotkeyDiagnostic("halt", scanCode, configured, "mismatch");
        Logger::LogInfo("GameLoop: Halt AI Actions ignored because key %d does not match configured %d",
                        scanCode,
                        configured);
        return true;
    }

    if (IsDuplicateHotkeyInvocation(scanCode)) {
        WriteHotkeyDiagnostic("halt", scanCode, configured, "duplicate");
        return true;
    }

    if (scanCode == 1 || scanCode == 15) {
        WriteHotkeyDiagnostic("halt", scanCode, configured, "blocked_menu_key");
        Logger::LogWarning("GameLoop: Ignoring Halt AI Actions because StopTalking is mapped to a game menu scan code (%d)", scanCode);
        return true;
    }

    if (!g_subsystemsInitialized) {
        InitializeSubsystems();
    }

    if (GameLoop::IsTextInputMenuActiveOrRecentlyClosed()) {
        WriteHotkeyDiagnostic("halt", scanCode, configured, "chatbox_active");
        Logger::LogInfo("GameLoop: Ignoring Halt AI Actions while chatbox is active or just submitted");
        return true;
    }

    WriteHotkeyDiagnostic("halt", scanCode, configured, "matched");
    Logger::LogInfo("GameLoop: Halt AI Actions hotkey pressed via dedicated xNVSE scan code %d", scanCode);
    GameLoop::HaltAIActionsNow();
    *result = 1;
    return true;
}

static bool Cmd_DialecticHandleHotkey_Execute(COMMAND_ARGS) {
    int scanCode = 0;
    *result = 0;

    if (!ExtractIntegerArgs(PASS_COMMAND_ARGS, &scanCode)) {
        return true;
    }

    if (scanCode <= 0) {
        return true;
    }

    if (!g_subsystemsInitialized) {
        InitializeSubsystems();
    }

    if (RawHotkeyMatches("TalkToNPC", scanCode)) {
        if (IsDuplicateHotkeyInvocation(scanCode)) {
            return true;
        }
        Logger::LogInfo("GameLoop: Chatbox hotkey pressed via xNVSE scan code %d", scanCode);
        GameLoop::RequestTextInputMenuOpen();
        *result = 1;
        return true;
    }

    if (RawHotkeyMatches("ToggleModes", scanCode)) {
        if (IsDuplicateHotkeyInvocation(scanCode)) {
            return true;
        }
        Logger::LogInfo("GameLoop: ToggleModes hotkey pressed via xNVSE scan code %d", scanCode);
        GameLoop::RequestModeMenuOpen();
        *result = 1;
        return true;
    }

    if (RawHotkeyMatches("ToggleLLMModel", scanCode)) {
        if (IsDuplicateHotkeyInvocation(scanCode)) {
            return true;
        }
        Logger::LogInfo("GameLoop: ToggleLLMModel hotkey pressed via xNVSE scan code %d", scanCode);
        GameLoop::RequestLLMModelMenuOpen();
        *result = 1;
        return true;
    }

    if (RawHotkeyMatches("DynamicProfileMenu", scanCode)) {
        if (IsDuplicateHotkeyInvocation(scanCode)) {
            return true;
        }
        Logger::LogInfo("GameLoop: DynamicProfileMenu hotkey pressed via xNVSE scan code %d", scanCode);
        GameLoop::RequestDynamicProfileMenuOpen();
        *result = 1;
        return true;
    }

    WriteHotkeyDiagnostic("generic", scanCode, 0, "unmatched");
    return true;
}

static bool Cmd_DialecticGetConfigIntById_Execute(COMMAND_ARGS) {
    int settingId = 0;
    int fallback = 0;
    *result = 0;

    if (ExtractConfigIdIntArgs(PASS_COMMAND_ARGS, &settingId, &fallback)) {
        if (!g_subsystemsInitialized) {
            InitializeSubsystems();
        }
        double value = fallback;
        *result = GetConfigById(settingId, fallback, value) ? static_cast<int>(value) : fallback;
    }
    return true;
}

static bool Cmd_DialecticSetConfigIntById_Execute(COMMAND_ARGS) {
    int settingId = 0;
    int value = 0;
    *result = 0;

    if (ExtractConfigIdIntArgs(PASS_COMMAND_ARGS, &settingId, &value)) {
        if (!g_subsystemsInitialized) {
            InitializeSubsystems();
        }
        *result = SetConfigById(settingId, value) ? 1 : 0;
    }
    return true;
}

static bool Cmd_DialecticGetConfigFloatById_Execute(COMMAND_ARGS) {
    int settingId = 0;
    float fallback = 0.0f;
    *result = 0.0;

    if (ExtractConfigIdFloatArgs(PASS_COMMAND_ARGS, &settingId, &fallback)) {
        if (!g_subsystemsInitialized) {
            InitializeSubsystems();
        }
        double value = fallback;
        *result = GetConfigById(settingId, fallback, value) ? value : fallback;
    }
    return true;
}

static bool Cmd_DialecticSetConfigFloatById_Execute(COMMAND_ARGS) {
    int settingId = 0;
    float value = 0.0f;
    *result = 0;

    if (ExtractConfigIdFloatArgs(PASS_COMMAND_ARGS, &settingId, &value)) {
        if (!g_subsystemsInitialized) {
            InitializeSubsystems();
        }
        *result = SetConfigById(settingId, value) ? 1 : 0;
    }
    return true;
}

static CommandInfo kCommandInfo_DialecticGetConfigInt = {
    "DialecticGetConfigInt", "", 0, "Gets a Dialectic integer INI setting.", 0, 3,
    kParams_ConfigStringsFallbackInt, Cmd_DialecticGetConfigInt_Execute, nullptr, nullptr, 0
};

static CommandInfo kCommandInfo_DialecticSetConfigInt = {
    "DialecticSetConfigInt", "", 0, "Sets a Dialectic integer INI setting.", 0, 3,
    kParams_ConfigStringsValueInt, Cmd_DialecticSetConfigInt_Execute, nullptr, nullptr, 0
};

static CommandInfo kCommandInfo_DialecticGetConfigFloat = {
    "DialecticGetConfigFloat", "", 0, "Gets a Dialectic float INI setting.", 0, 3,
    kParams_ConfigStringsFallbackFloat, Cmd_DialecticGetConfigFloat_Execute, nullptr, nullptr, 0
};

static CommandInfo kCommandInfo_DialecticSetConfigFloat = {
    "DialecticSetConfigFloat", "", 0, "Sets a Dialectic float INI setting.", 0, 3,
    kParams_ConfigStringsValueFloat, Cmd_DialecticSetConfigFloat_Execute, nullptr, nullptr, 0
};

static CommandInfo kCommandInfo_DialecticReloadConfig = {
    "DialecticReloadConfig", "", 0, "Reloads Dialectic INI settings.", 0, 0,
    nullptr, Cmd_DialecticReloadConfig_Execute, nullptr, nullptr, 0
};

static CommandInfo kCommandInfo_DialecticSaveConfig = {
    "DialecticSaveConfig", "", 0, "Saves Dialectic INI settings.", 0, 0,
    nullptr, Cmd_DialecticSaveConfig_Execute, nullptr, nullptr, 0
};

static CommandInfo kCommandInfo_DialecticSyncWorldData = {
    "DialecticSyncWorldData", "", 0, "Syncs Fallout factions and locations to DialecticServer.", 0, 0,
    nullptr, Cmd_DialecticSyncWorldData_Execute, nullptr, nullptr, 0
};

static CommandInfo kCommandInfo_DialecticSendAllVoiceSamples = {
    "DialecticSendAllVoiceSamples", "", 0, "Uploads configured Fallout voice samples to DialecticServer.", 0, 0,
    nullptr, Cmd_DialecticSendAllVoiceSamples_Execute, nullptr, nullptr, 0
};

static CommandInfo kCommandInfo_DialecticSendSetConf = {
    "DialecticSendSetConf", "", 0, "Sends a DialecticServer setconf payload.", 0, 1,
    kParams_SetConfPayload, Cmd_DialecticSendSetConf_Execute, nullptr, nullptr, 0
};

static CommandInfo kCommandInfo_DialecticUpdateActiveQuest = {
    "DialecticUpdateActiveQuest", "", 0, "Sends the currently selected Fallout quest to DialecticServer.", 0, 3,
    kParams_ActiveQuestUpdate, Cmd_DialecticUpdateActiveQuest_Execute, nullptr, nullptr, 0
};

static CommandInfo kCommandInfo_DialecticOpenModeMenu = {
    "DialecticOpenModeMenu", "", 0, "Requests the Dialectic mode selector menu.", 0, 0,
    nullptr, Cmd_DialecticOpenModeMenu_Execute, nullptr, nullptr, 0
};

static CommandInfo kCommandInfo_DialecticOpenLLMModelMenu = {
    "DialecticOpenLLMModelMenu", "", 0, "Requests the Dialectic LLM model selector menu.", 0, 0,
    nullptr, Cmd_DialecticOpenLLMModelMenu_Execute, nullptr, nullptr, 0
};

static CommandInfo kCommandInfo_DialecticOpenDynamicProfileMenu = {
    "DialecticOpenDynamicProfileMenu", "", 0, "Requests the Dialectic dynamic profile selector menu.", 0, 0,
    nullptr, Cmd_DialecticOpenDynamicProfileMenu_Execute, nullptr, nullptr, 0
};

static CommandInfo kCommandInfo_DialecticManageAIAgents = {
    "DialecticManageAIAgents", "", 0, "Runs an in-game Dialectic AI Agent management operation.", 0, 1,
    kParams_Integer, Cmd_DialecticManageAIAgents_Execute, nullptr, nullptr, 0
};

static CommandInfo kCommandInfo_DialecticHandleHotkey = {
    "DialecticHandleHotkey", "", 0, "Routes a raw xNVSE/Fallout scan-code hotkey through Dialectic.", 0, 1,
    kParams_Integer, Cmd_DialecticHandleHotkey_Execute, nullptr, nullptr, 0
};

static CommandInfo kCommandInfo_DialecticHandleHaltHotkey = {
    "DialecticHandleHaltHotkey", "", 0, "Routes the configured Halt AI Actions xNVSE hotkey through Dialectic.", 0, 1,
    kParams_Integer, Cmd_DialecticHandleHaltHotkey_Execute, nullptr, nullptr, 0
};

static CommandInfo kCommandInfo_DialecticGetConfigIntById = {
    "DialecticGetConfigIntById", "", 0, "Gets a Dialectic integer INI setting by stable MCM id.", 0, 2,
    kParams_ConfigIdFallbackInt, Cmd_DialecticGetConfigIntById_Execute, nullptr, nullptr, 0
};

static CommandInfo kCommandInfo_DialecticSetConfigIntById = {
    "DialecticSetConfigIntById", "", 0, "Sets a Dialectic integer INI setting by stable MCM id.", 0, 2,
    kParams_ConfigIdValueInt, Cmd_DialecticSetConfigIntById_Execute, nullptr, nullptr, 0
};

static CommandInfo kCommandInfo_DialecticGetConfigFloatById = {
    "DialecticGetConfigFloatById", "", 0, "Gets a Dialectic float INI setting by stable MCM id.", 0, 2,
    kParams_ConfigIdFallbackFloat, Cmd_DialecticGetConfigFloatById_Execute, nullptr, nullptr, 0
};

static CommandInfo kCommandInfo_DialecticSetConfigFloatById = {
    "DialecticSetConfigFloatById", "", 0, "Sets a Dialectic float INI setting by stable MCM id.", 0, 2,
    kParams_ConfigIdValueFloat, Cmd_DialecticSetConfigFloatById_Execute, nullptr, nullptr, 0
};

static CommandInfo kCommandInfo_DialecticCaptureDialoguePrompt = {
    "DialecticCaptureDialoguePrompt", "", 0, "Captures the player prompt from a dialogue TopicInfo.", 0, 2,
    kParams_DialoguePromptCapture, Cmd_DialecticCaptureDialoguePrompt_Execute, nullptr, nullptr, 0
};

static CommandInfo kCommandInfo_DialecticCaptureDialogue = {
    "DialecticCaptureDialogue", "", 0, "Submits captured Fallout dialogue directly to the Dialectic runtime.", 0, 9,
    kParams_CapturedDialogue, Cmd_DialecticCaptureDialogue_Execute, nullptr, nullptr, 0
};

static void RegisterDialecticScriptCommands(const NVSEInterface* nvse) {
    constexpr UInt32 kDialecticOpcodeBase = 0x6D00;
    nvse->SetOpcodeBase(kDialecticOpcodeBase);

    CommandInfo* commands[] = {
        &kCommandInfo_DialecticGetConfigInt,
        &kCommandInfo_DialecticSetConfigInt,
        &kCommandInfo_DialecticGetConfigFloat,
        &kCommandInfo_DialecticSetConfigFloat,
        &kCommandInfo_DialecticReloadConfig,
        &kCommandInfo_DialecticSaveConfig,
        &kCommandInfo_DialecticGetConfigIntById,
        &kCommandInfo_DialecticSetConfigIntById,
        &kCommandInfo_DialecticGetConfigFloatById,
        &kCommandInfo_DialecticSetConfigFloatById,
        &kCommandInfo_DialecticSyncWorldData,
        &kCommandInfo_DialecticSendAllVoiceSamples,
        &kCommandInfo_DialecticCaptureDialoguePrompt,
        &kCommandInfo_DialecticCaptureDialogue,
        &kCommandInfo_DialecticSendSetConf,
        &kCommandInfo_DialecticUpdateActiveQuest,
        &kCommandInfo_DialecticOpenModeMenu,
        &kCommandInfo_DialecticOpenLLMModelMenu,
        &kCommandInfo_DialecticOpenDynamicProfileMenu,
        &kCommandInfo_DialecticManageAIAgents,
        &kCommandInfo_DialecticHandleHotkey,
        &kCommandInfo_DialecticHandleHaltHotkey
    };

    for (CommandInfo* command : commands) {
        if (nvse->RegisterCommand(command)) {
            Logger::LogInfo("Registered NVSE command: %s", command->longName);
        } else {
            Logger::LogWarning("Failed to register NVSE command: %s", command->longName);
        }
    }
}

// NVSE interfaces reserved for future event integrations.
NVSEEventManagerInterface* g_eventInterface = nullptr;
NVSEArrayVarInterface* g_arrayInterface = nullptr;

// Legacy Log function wrapper for backwards compatibility
void Log(const char* fmt, ...) {
    char buffer[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    Logger::LogInfo(buffer);
}

// Forward declarations from other modules
namespace Config {
    void Load();
}

namespace HTTPManager {
    void Initialize();
    void Shutdown();
}

namespace AudioManager {
    void Initialize();
    void Shutdown();
}

namespace SpeakManager {
    void Initialize();
    void Shutdown();
}

namespace AgentManager {
    void Initialize();
    void Shutdown();
}

namespace NPCDetector {
    void Initialize();
    void Shutdown();
}

namespace InputManager {
    void Initialize();
    void Shutdown();
}

namespace TargetManager {
    void Initialize();
    void Shutdown();
    void SetCurrentTarget(uint32_t formId, const std::string& name, bool isActor);
    void SetNearbyNPC(uint32_t formId, const std::string& name, float distance);
    void ClearCurrentTarget();
}

namespace ActivationManager {
    void Initialize();
    void Shutdown();
}

namespace GameLoop {
    void Initialize();
    void Shutdown();
    void Update(float deltaTime);  // Frame update function
}

// Lazy initialization of subsystems - called on first use
void InitializeSubsystems() {
    if (g_subsystemsInitialized) return;
    
    Logger::LogSection("INITIALIZING SUBSYSTEMS");
    
    try {
        Config::Load();
        Logger::LogInfo("Config loaded successfully");
    } catch (...) {
        Logger::LogWarning("Config load failed, using defaults");
    }

    try {
        HTTPManager::Initialize();
        Logger::LogInfo("HTTP Manager initialized");

        AudioManager::Initialize();
        Logger::LogInfo("Audio Manager initialized");

        SpeakManager::Initialize();
        Logger::LogInfo("Speak Manager initialized");

        AgentManager::Initialize();
        Logger::LogInfo("Agent Manager initialized");
        
        NPCDetector::Initialize();
        Logger::LogInfo("NPC Detector initialized");

        InputManager::Initialize();
        Logger::LogInfo("Input Manager initialized");

        TargetManager::Initialize();
        Logger::LogInfo("Target Manager initialized");

        ActivationManager::Initialize();
        Logger::LogInfo("Activation Manager initialized");

        GameLoop::Initialize();
        g_subsystemsInitialized = true;
        Logger::LogInfo("Game Loop initialized on native xNVSE frame pump");

        bool expectedImportDetection = false;
        if (g_importDataDetectionDone.compare_exchange_strong(expectedImportDetection, true)) {
            if (TaskManager::Enqueue("import_detection", "csv_import", 0, false,
                std::chrono::minutes(5), [](const TaskManager::CancellationToken& token) {
                ImportDataSyncFNV::DetectAndUploadImportDataFiles(
                    [&token]() { return token.IsCancellationRequested(); });
            }) != 0) {
                Logger::LogInfo("Dialectic CSV import data detection started in background");
            } else {
                g_importDataDetectionDone = false;
                Logger::LogWarning("Dialectic CSV import data detection could not be queued");
            }
        }

        Logger::LogSection("ALL SUBSYSTEMS INITIALIZED");
    }
    catch (...) {
        Logger::LogError("FATAL: Exception during subsystem initialization");
    }
}

// DLL entry point
extern "C" {

__declspec(dllexport) BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hinstDLL);
            // Initialize logger first
            Logger::Initialize();
            ClearStartupBridgeState();
            Logger::LogInfo("=== Dialectic DLL Attached ===");
            break;
        case DLL_PROCESS_DETACH:
            if (g_subsystemsInitialized) {
                Logger::LogInfo("DLL detaching, shutting down subsystems...");
                GameLoop::Shutdown();
                ActivationManager::Shutdown();
                TargetManager::Shutdown();
                InputManager::Shutdown();
                AgentManager::Shutdown();
                SpeakManager::Shutdown();
                AudioManager::Shutdown();
                HTTPManager::Shutdown();
            }
            FNVRuntime::Shutdown();
            Logger::LogInfo("=== Dialectic DLL Detached ===");
            Logger::Shutdown();
            break;
    }
    return TRUE;
}

// NVSE query function - called when NVSE loads the plugin
__declspec(dllexport) bool NVSEPlugin_Query(const NVSEInterface* nvse, PluginInfo* info) {
    Logger::LogInfo("NVSEPlugin_Query called (NVSE v%d, runtime 0x%08X)", 
        nvse->nvseVersion, nvse->runtimeVersion);

    // Fill in plugin info
    info->infoVersion = PluginInfo::kInfoVersion;
    info->name = "Dialectic";
    info->version = DIALECTIC_PLUGIN_INFO_VERSION;

    // In GECK, load only the command registration layer used by maintained scripts.
    if (nvse->isEditor) {
        Logger::LogInfo("GECK editor detected; loading command registration layer only");
    }

    // Version checks
    if (nvse->nvseVersion < NVSE_VERSION_INTEGER) {
        Logger::LogError("NVSE version too old (%d < %d)", nvse->nvseVersion, NVSE_VERSION_INTEGER);
        return false;
    }

    Logger::LogInfo("Query successful");
    return true;
}

// NVSE load function - called after all plugins are queried
__declspec(dllexport) bool NVSEPlugin_Load(const NVSEInterface* nvse) {
    Logger::LogInfo("NVSEPlugin_Load called - Dialectic v%s", DIALECTIC_VERSION);

    g_pluginHandle = nvse->GetPluginHandle();
    Logger::LogDebug("Plugin handle: 0x%08X", g_pluginHandle);

    if (!nvse->isEditor) {
        FNVRuntime::Initialize(nvse, g_pluginHandle);
    }

    g_stringVarInterface = (NVSEStringVarInterface*)nvse->QueryInterface(kInterface_StringVar);
    if (g_stringVarInterface && g_stringVarInterface->GetString) {
        Logger::LogInfo("String Var Interface acquired at 0x%p", g_stringVarInterface);
    } else {
        Logger::LogWarning("String Var Interface not available; dynamic dialogue capture will be disabled");
    }

    g_scriptInterface = (NVSEScriptInterface*)nvse->QueryInterface(kInterface_Script);
    if (g_scriptInterface && g_scriptInterface->ExtractArgsEx) {
        Logger::LogInfo("Script Interface acquired at 0x%p", g_scriptInterface);
        RegisterDialecticScriptCommands(nvse);
    } else {
        Logger::LogWarning("Script Interface not available; Dialectic MCM commands will not be registered");
    }

    if (nvse->isEditor) {
        Logger::LogInfo("Editor load complete; runtime subsystems are disabled in GECK");
        return true;
    }

    // Query for Event Manager interface
    g_eventInterface = (NVSEEventManagerInterface*)nvse->QueryInterface(kInterface_EventManager);
    if (g_eventInterface) {
        Logger::LogInfo("Event Manager Interface acquired at 0x%p", g_eventInterface);
    } else {
        Logger::LogWarning("Event Manager Interface not available");
    }

    // Query for Array Var interface
    g_arrayInterface = (NVSEArrayVarInterface*)nvse->QueryInterface(kInterface_ArrayVar);
    if (g_arrayInterface) {
        Logger::LogInfo("Array Var Interface acquired at 0x%p", g_arrayInterface);
    } else {
        Logger::LogWarning("Array Var Interface not available");
    }

    g_initialized = true;

    Logger::LogInfo("Plugin loaded successfully (subsystems initialize from the game-frame pump)");
    return true;
}

// Frame counter for auto-initialization
static int g_frameCounter = 0;

// Exported function for NVSE scripts to trigger initialization
__declspec(dllexport) void Dialectic_Initialize() {
    InitializeSubsystems();
}

// Exported function to check if plugin is ready
__declspec(dllexport) bool Dialectic_IsReady() {
    return g_subsystemsInitialized;
}

// Exported function to pass crosshair ref from NVSE script
__declspec(dllexport) void Dialectic_SetCrosshairRef(uint32_t formId, const char* name) {
    if (g_subsystemsInitialized && formId != 0 && name != nullptr) {
        Logger::LogDebug("Dialectic_SetCrosshairRef: %s (0x%08X)", name, formId);
        TargetManager::SetCurrentTarget(formId, name, true);
    } else if (formId == 0) {
        // Clear target if formId is 0
        TargetManager::ClearCurrentTarget();
    }
}

// Exported function to pass nearby NPC data from NVSE script
__declspec(dllexport) void Dialectic_SetNearbyNPC(uint32_t formId, const char* name, float distance) {
    if (g_subsystemsInitialized && formId != 0 && name != nullptr) {
        Logger::LogDebug("Dialectic_SetNearbyNPC: %s (0x%08X) at %.1f units", name, formId, distance);
        TargetManager::SetNearbyNPC(formId, name, distance);
    }
}

// Main update function called every frame from NVSE script
__declspec(dllexport) void Dialectic_UpdateFrame(float deltaTime) {
    FNVRuntime::PumpLegacyFrameFallback();
    // Auto-initialize subsystems after a few frames (game has had time to load)
    if (!g_subsystemsInitialized && g_frameCounter++ > 10) {
        Log("Dialectic: Auto-initializing subsystems (frame %d)", g_frameCounter);
        InitializeSubsystems();
    }
    
    // Only update if initialized
    if (g_subsystemsInitialized) {
        GameLoop::Update(deltaTime);
    }
}

// Runtime INI bridge for the FNV MCM quest/script layer.
__declspec(dllexport) float Dialectic_GetConfigFloat(const char* section, const char* key, float fallback) {
    try {
        if (!g_subsystemsInitialized) {
            InitializeSubsystems();
        }

        double value = fallback;
        return GetDialecticConfigValue(section, key, fallback, value)
            ? static_cast<float>(value)
            : fallback;
    } catch (...) {
        Logger::LogWarning("Dialectic_GetConfigFloat failed for [%s] %s", section ? section : "", key ? key : "");
        return fallback;
    }
}

__declspec(dllexport) int Dialectic_GetConfigInt(const char* section, const char* key, int fallback) {
    try {
        if (!g_subsystemsInitialized) {
            InitializeSubsystems();
        }

        double value = fallback;
        return GetDialecticConfigValue(section, key, fallback, value)
            ? static_cast<int>(value)
            : fallback;
    } catch (...) {
        Logger::LogWarning("Dialectic_GetConfigInt failed for [%s] %s", section ? section : "", key ? key : "");
        return fallback;
    }
}

__declspec(dllexport) int Dialectic_SetConfigFloat(const char* section, const char* key, float value) {
    try {
        if (!g_subsystemsInitialized) {
            InitializeSubsystems();
        }
        return SetDialecticConfigValue(section, key, value) ? 1 : 0;
    } catch (...) {
        Logger::LogWarning("Dialectic_SetConfigFloat failed for [%s] %s", section ? section : "", key ? key : "");
        return 0;
    }
}

__declspec(dllexport) int Dialectic_SetConfigInt(const char* section, const char* key, int value) {
    try {
        if (!g_subsystemsInitialized) {
            InitializeSubsystems();
        }
        return SetDialecticConfigValue(section, key, value) ? 1 : 0;
    } catch (...) {
        Logger::LogWarning("Dialectic_SetConfigInt failed for [%s] %s", section ? section : "", key ? key : "");
        return 0;
    }
}

__declspec(dllexport) void Dialectic_ReloadConfig() {
    try {
        Config::Load();
        if (g_subsystemsInitialized) {
            InputManager::LoadConfig();
        }
        Logger::LogInfo("Dialectic config reloaded");
    } catch (...) {
        Logger::LogWarning("Dialectic_ReloadConfig failed");
    }
}

__declspec(dllexport) void Dialectic_SaveConfig() {
    try {
        Config::Save();
        if (g_subsystemsInitialized) {
            InputManager::SaveConfig();
        }
        Logger::LogInfo("Dialectic config saved");
    } catch (...) {
        Logger::LogWarning("Dialectic_SaveConfig failed");
    }
}

}; // extern "C"
