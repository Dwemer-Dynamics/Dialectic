#pragma once

#include <string>
#include <vector>
#include <unordered_set>
#include <cstdint>

namespace Config {
    // Server configuration
    extern std::string serverHost;
    extern int serverPort;
    extern std::string serverPath;
    extern std::string localSoundcachePath;
    
    // Player configuration
    extern std::string playerName;
    
    // Audio configuration
    extern float preClipMs;
    extern float postClipMs;
    extern int animationResolution;
    extern float animationIntensity;
    extern float voiceVolume;
    extern bool audio3DPlaybackEnabled;
    extern bool audioCameraBased;
    extern float audio3DPanStrength;
    extern bool audioInvertHeading;
    extern float audioDistanceScale;
    extern float audioPlaybackDropoffInteriorPercent;
    extern float audioPlaybackDropoffExteriorPercent;
    
    // Behavior configuration
    extern bool enableCombatDialogue;
    extern bool cancelDialogueOnCombat;
    extern int aiResponseTimeout;
    extern bool pauseDialogueOnMenu;
    extern bool suppressVanillaDialogueDuringAI;
    extern bool faceTargetDuringAIResponse;
    extern bool showAISubtitles;
    extern bool sceneSafetyEnabled;

    // Vanilla/radiant dialogue capture configuration
    extern bool dialogueCaptureEnabled;
    extern bool dialogueCaptureRadiant;
    extern bool dialogueCaptureDialogueMenu;
    extern bool dialogueCapturePlayerMenuChoices;
    extern int dialogueCaptureRepeatWindowMs;
    extern bool dialogueCaptureRequireKnownSpeaker;
    
    // Voice recording configuration
    extern int silenceThreshold;
    extern int maxRecordingSeconds;
    extern int voiceRecordingDeviceId;
    extern std::string voiceRecordingDeviceName;
    extern bool openMicEnabled;
    extern float openMicSensitivity;
    extern float openMicEndDelaySeconds;
    extern bool openMicMuted;
    
    // Distance thresholds
    extern float distanceActivatingNpcInterior;
    extern float distanceActivatingNpcExterior;

    // Auto-activation configuration
    extern bool autoActivateEnabled;
    extern bool autoAddHostile;
    extern bool autoAddCreatures;

    // RPG event trigger configuration
    extern bool rpgCombatBarksEnabled;
    extern int rpgCombatBarkPeriodSeconds;

    // Spatial audio configuration
    extern bool spatialAudioEnabled;
    extern float spatialMaxAirDistance;
    extern float spatialImmediateDistance;
    extern float spatialAutoHearingDistance;
    extern float spatialDistanceScaler;
    extern float spatialInteriorHearingDistance;
    extern float spatialExteriorHearingDistance;
    extern float spatialMinDistanceFactor;
    extern float spatialInteriorBaseModifier;
    extern float spatialExteriorBaseModifier;
    extern float spatialOpenDoorPenaltyBase;
    extern float spatialAroundCornerPenalty;
    extern float spatialDoorTriangulationPercentTolerance;
    extern float spatialDoorTriangulationAbsoluteTolerance;
    extern float spatialMinimumAudibleVolume;
    extern float spatialPathRatioReject;
    extern float spatialPathRatioDistanceReject;
    extern float spatialPathRatioDistanceRejectMinAir;
    extern float spatialPathComplexityStartRatio;
    extern float spatialPathComplexityScale;
    extern float spatialPathComplexityMin;
    extern float spatialNavmeshSnapDistance;
    extern bool spatialAllowPathUnavailableFallback;

    // World context configuration
    extern bool worldContextEnabled;
    extern float worldContextUpdateSeconds;
    extern bool worldContextSendOnChange;
    extern bool worldContextSendBeforePlayerInput;
    extern bool worldContextIncludeWeather;
    extern bool worldContextIncludeCell;
    extern bool worldContextIncludeWorldspace;

    // Nearby actor snapshot configuration
    extern bool nearbyActorsEnabled;
    extern float nearbyActorsUpdateSeconds;
    extern bool nearbyActorsSendOnChange;
    extern bool nearbyActorsSendBeforePlayerInput;
    extern int nearbyActorsMaxActors;
    extern float nearbyActorsMaxDistance;

    // Activity status snapshot configuration
    extern bool activityStatusEnabled;
    extern float activityStatusUpdateSeconds;
    extern bool activityStatusSendOnChange;
    extern bool activityStatusSendBeforePlayerInput;
    extern int activityStatusMaxActors;
    extern float activityStatusMaxDistance;

    // Nearby item snapshot configuration
    extern bool nearbyItemsEnabled;
    extern float nearbyItemsUpdateSeconds;
    extern bool nearbyItemsSendOnChange;
    extern bool nearbyItemsSendBeforePlayerInput;
    extern int nearbyItemsMaxItems;
    extern float nearbyItemsMaxDistance;
    extern bool nearbyItemsIncludeStealing;
    extern bool nearbyItemsIncludeLookingAt;
    extern bool nearbyItemsIncludeHeldItem;
    extern bool nearbyItemsHeldItemPriority;

    // Points of interest snapshot configuration
    extern bool pointsOfInterestEnabled;
    extern float pointsOfInterestUpdateSeconds;
    extern bool pointsOfInterestSendOnChange;
    extern bool pointsOfInterestSendBeforePlayerInput;
    extern int pointsOfInterestMaxPois;
    extern float pointsOfInterestMaxDistance;
    extern bool pointsOfInterestIncludeDoors;
    extern bool pointsOfInterestIncludeLocked;
    extern bool pointsOfInterestIncludeLookingAt;

    // Dynamic profile trigger configuration
    extern int dynamicProfileTimerMinutes;
    extern bool dynamicProfileTimerIncludeNarrator;

    // Bored/idle event trigger configuration
    extern bool boredEventsEnabled;
    extern int boredEventTimerSeconds;
    extern bool boredAvoidInMenu;
    extern bool boredAvoidInDialogue;
    extern bool boredAvoidInCombat;
    extern bool boredAvoidWhenSneaking;
    extern bool boredAvoidWhenVoiceInputActive;
    extern int boredRecentSpeechCooldownSeconds;

    // Narrator interaction configuration
    extern bool narratorModeEnabled;

    // Mode selector configuration
    extern int currentModeIndex;
    extern std::string currentMode;
    extern int currentProfileModelSlot;

    // Rechat launch policy configuration
    extern bool rechatEnabled;
    extern int rechatMaxDepth;
    extern bool rechatSmartLaunch;
    extern bool rechatRetryOnEmpty;
    extern bool rechatAvoidWhenSneaking;
    extern bool rechatAvoidInMenu;
    extern int rechatEndConversationCooldown;
    
    // Exclusion lists (script/INI controlled)
    extern std::unordered_set<std::string> excludedRaces;
    extern std::unordered_set<std::string> excludedNPCNames;
    extern std::unordered_set<uint32_t> excludedFormIDs;
    
    // Initialize and load configuration from INI
    void Load();
    void Save();

    // dialectic.ini contains shipped defaults. User and runtime changes belong in
    // dialectic_custom.ini so mod updates cannot overwrite them.
    const char* GetDefaultINIPath();
    const char* GetCustomINIPath();
    int ReadINIInt(const char* section, const char* key, int fallback);
    bool WriteCustomINIValue(const char* section, const char* key, const char* value);
    
    // Get/Set functions for runtime configuration
    void SetServerHost(const std::string& host);
    void SetServerPort(int port);
    void SetServerPath(const std::string& path);

    // Exclusion checking functions
    bool IsRaceExcluded(const std::string& raceName);
    bool IsNPCExcluded(const std::string& npcName);
    bool IsFormIDExcluded(uint32_t formId);
    
    // Runtime exclusion management
    void AddExcludedRace(const std::string& raceName);
    void AddExcludedNPC(const std::string& npcName);
    void AddExcludedFormID(uint32_t formId);
    void RemoveExcludedRace(const std::string& raceName);
    void RemoveExcludedNPC(const std::string& npcName);
    void RemoveExcludedFormID(uint32_t formId);
}
