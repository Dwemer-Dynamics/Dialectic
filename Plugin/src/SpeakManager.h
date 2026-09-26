// SpeakManager.h - Speech/dialogue queue management for Dialectic

#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <cstddef>

namespace SpeakManager {
// Passive listener presentation never resolves actors or reports AI delivery. Game thread only.
void PlaySharedDialogue(const std::string& speaker, const std::string& text,
                        const std::string& utterance, const std::vector<uint8_t>& audio);
void StopSharedDialogue();
void UpdateSharedDialogue(bool remotePaused);
struct QueueStatus {
    uint64_t audioGeneration = 0;
    int dialogueLinesQueued = 0;
    int ttsDownloadsInProgress = 0;
    int preparedAudioCount = 0;
    int ttsTasksPending = 0;
    int ttsTasksActive = 0;
    uint64_t totalTtsTasksQueued = 0;
    uint64_t totalTtsTasksCompleted = 0;
    bool isProcessing = false;
    bool isPlaying = false;
    bool currentPlaybackLineActive = false;
    std::string currentSpeaker;
    std::string currentTextPreview;
};

struct SpeechDiagnostics {
    uint64_t dialogueLinesReceived = 0;
    uint64_t playerAudioLinesReceived = 0;
    uint64_t playerTextOnlyLinesReceived = 0;
    uint64_t audioPrepareStarted = 0;
    uint64_t audioReady = 0;
    uint64_t audioFailed = 0;
    uint64_t playbackStarted = 0;
    uint64_t playbackCompleted = 0;
    uint64_t playbackFailed = 0;
    uint64_t lipSyncCommandsRequested = 0;
    uint64_t nativeMfgApplied = 0;
    uint64_t scriptMfgFallbacks = 0;
    uint64_t subtitleScriptFallbacks = 0;
    uint64_t nativeDialogueGuards = 0;
    uint64_t scriptDialogueGuards = 0;
    uint64_t dialogueTurnCancellations = 0;
};

// Initialize the speak manager
void Initialize();

// Shutdown the speak manager
void Shutdown();

// Queue dialogue for playback
void QueueDialogue(const std::string& text,
                   const std::string& speaker,
                   uint32_t actorFormId,
                   bool isFinalResponseLine = false,
                   const std::string& listenerHint = "",
                   const std::string& rechatTargetHint = "",
                   int rechatDepth = 0,
                   const std::string& ttsCacheKey = "",
                   const std::string& utteranceId = "",
                   const std::string& requestId = "",
                   uint64_t runtimeGeneration = 0,
                   uint32_t listenerFormId = 0,
                   uint32_t rechatTargetFormId = 0,
                   const std::string& displayName = "",
                   bool directorScene = false);

// Suppress vanilla/radiant dialogue for an actor while an AI turn is pending.
void GuardActorForPendingDialogue(uint32_t actorFormId, const std::string& actorName);

// Preserve the player-centered audience for later rechat requests.
void SetPlayerTurnAudience(const std::string& peoplePipe);

// Rechat chain state
bool BeginRechatAttempt(const std::string& speaker);
void QueueRechatRetry(const std::string& speaker,
                      const std::string& listenerHint,
                      const std::string& explicitTarget,
                      const std::string& originLine,
                      int rechatDepth,
                      uint32_t speakerFormId = 0,
                      uint32_t listenerFormId = 0,
                      uint32_t targetFormId = 0);
void CompleteRechatAttempt(const std::string& speaker, bool success);
bool IsRechatInFlightFor(const std::string& speaker);
void ResetRechatChainState();
void StartRechatChainForAutonomousEvent();
bool IsRechatChainClosed();
std::string GetLastRechatter();
void SetLastRechatter(const std::string& speaker);
std::string EnsureRechatChainId(const std::string& speaker,
                                const std::string& listenerHint,
                                const std::string& explicitTarget);
int Rechat(const std::string& speaker,
           const std::string& listenerHint,
           int rechatDepth,
           const std::string& originLine,
           const std::string& explicitTarget = "",
           uint32_t speakerFormId = 0,
           uint32_t listenerFormId = 0,
           uint32_t targetFormId = 0);

// Compatibility aliases for incremental porting.
bool beginRechatAttempt(const std::string& speaker);
void queueRechatRetry(const std::string& speaker,
                      const std::string& listenerHint,
                      const std::string& explicitTarget,
                      const std::string& originLine,
                      int rechatDepth,
                      uint32_t speakerFormId = 0,
                      uint32_t listenerFormId = 0,
                      uint32_t targetFormId = 0);
void completeRechatAttempt(const std::string& speaker, bool success);
bool isRechatInFlightFor(const std::string& speaker);
void resetRechatChainState();
bool isRechatChainClosed();
std::string getLastRechatter();
void setLastRechatter(const std::string& speaker);
std::string ensureRechatChainId(const std::string& speaker,
                                const std::string& listenerHint,
                                const std::string& explicitTarget);
int rechat(const std::string& speaker,
           const std::string& listenerHint,
           int rechatDepth,
           const std::string& originLine,
           const std::string& explicitTarget = "",
           uint32_t speakerFormId = 0,
           uint32_t listenerFormId = 0,
           uint32_t targetFormId = 0);

// Process the dialogue queue
void ProcessQueue();

// Update active playback-only systems such as spatial audio and lip sync.
void UpdatePlaybackFrame();

// Check if currently speaking
bool IsSpeaking();
QueueStatus GetQueueStatus();
SpeechDiagnostics GetDiagnostics();

// True when text matches a currently/recently displayed Dialectic AI subtitle.
// Used by vanilla dialogue capture to avoid logging AI subtitles as background chat.
bool IsRecentAISubtitleText(const std::string& text);

// Stop current speech, preserving queued player TTS for normal interruption.
void StopSpeaking();

// Hard kill all queued/active speech, including player sidecar lines.
void HaltAllSpeech();

// Discard future lines while letting current playback finish.
void DiscardPendingInteraction();

// Clear all queued/active speech for lifecycle resets without reporting a hard halt.
void ClearAllSpeech(const char* reason);

// Stop active/pending NPC dialogue and suppress rechat briefly before player input.
void InterruptForPlayerInput();

// Centralized CHIM-style cancellation entry point for player input, halt, scene,
// combat, and lifecycle resets.
void CancelDialogueTurn(const char* reason, bool preservePlayerLines, bool suppressRechatBriefly);

// Temporarily hold NPC playback while the player input TTS sidecar catches up.
void BeginPlayerInputTtsGate();

// Release the player input TTS gate when the sidecar request fails or is skipped.
void ClearPlayerInputTtsGate(const char* reason);

} // namespace SpeakManager
