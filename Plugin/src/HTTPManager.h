#pragma once

#include <string>
#include <functional>
#include <cstdint>
#include <cstddef>

// Undefine Windows SendMessage macro to avoid conflicts
#ifdef SendMessage
#undef SendMessage
#endif

namespace TaskManager { class CancellationToken; }

namespace HTTPManager {
    struct QueueStatus {
        uint64_t generation = 0;
        uint64_t responseQueueGeneration = 0;
        bool streamInProgress = false;
        bool cancellationRequested = false;
        int activeStreamTasks = 0;
        std::size_t pendingHttpTasks = 0;
        std::size_t activeHttpTasks = 0;
        uint64_t totalHttpTasksQueued = 0;
        uint64_t totalHttpTasksCompleted = 0;
        uint64_t totalHttpTasksCancelled = 0;
        std::size_t httpResponsesQueued = 0;
        std::size_t responseDialogueQueued = 0;
        std::size_t responseActionsQueued = 0;
        uint64_t totalResponsesQueued = 0;
        uint64_t totalResponsesDispatched = 0;
        uint64_t totalResponsesDroppedStale = 0;
        std::string activeStreamName;
        std::string activeHttpTaskSummary;
    };

    // Initialize HTTP subsystem
    void Initialize();
    void Shutdown();

    // Log event to server
    void LogEvent(const std::string& msg);
    void LogEvent(const std::string& msg, const std::string& forcedActor);
    
    // Stream input text to server
    void Stream(const std::string& msg);
    void Stream(const std::string& msg, int rechatDepth);
    
    // Send a game event to DialecticServer.
    void SendEvent(const std::string& eventType,
                   const std::string& payload,
                   const std::string& audienceSnapshotJson = "");

    // Drop queued/stale responses and invalidate in-flight response continuations.
    void CancelPendingResponses();
    void DiscardInteractionResponses();
    int CancelTasksByType(const std::string& taskType);
    int CancelTasksByKey(const std::string& taskKey);

    // Send structured JSON to a DialecticServer endpoint such as gamedata.php.
    std::string SendJson(const std::string& endpoint, const std::string& jsonBody);

    // Read the server semantic version used by the startup compatibility check.
    std::string GetServerVersionRaw();

    // Acknowledge dialogue playback delivery state to DialecticServer.
    void SendDialogueDeliveryAck(const std::string& speaker,
                                 uint32_t actorFormId,
                                 const std::string& text,
                                 const std::string& ttsCacheKey,
                                 const std::string& utteranceId,
                                 const std::string& state,
                                 const std::string& requestId = "");
    
    // Send player input text
    void SendPlayerInput(const std::string& npcName, const std::string& message);

    // Send a priority standalone Player TTS playback request.
    bool SendPlayerTtsPlay(const std::string& message);

    // Queue standalone Player TTS without blocking the main inputtext request.
    bool QueuePlayerTtsPlay(const std::string& message);

    // Queue exact actor-bound NPC TTS through the dedicated public API endpoint.
    bool QueueNpcTtsPlay(uint32_t actorFormId,
                         const std::string& actorName,
                         const std::string& message);
    
    // Utility functions
    std::string EscapeJson(const std::string& input);
    std::string UrlEncode(const std::string& value);
    
    // Audio upload for STT
    std::string UploadAudioForSTT(const std::string& wavData,
                                  const TaskManager::CancellationToken* token = nullptr);

    // Upload a manual PipVision screenshot and structured capture metadata.
    std::string UploadPipVisionImage(const std::string& imageData,
                                     const std::string& metadataJson,
                                     const std::string& fileName,
                                     const TaskManager::CancellationToken* token = nullptr);

    // Upload a resolved NPC voice sample to DialecticServer's voice sample extractor.
    std::string UploadVoiceSample(const std::string& audioData,
                                  const std::string& actorName,
                                  const std::string& originalName,
                                  const std::string& referenceText = "");

    // Upload a CSV import file to DialecticServer's csv_import.php endpoint.
    std::string UploadCSVFile(const std::string& csvData,
                              const std::string& filename,
                              const std::string& fileType);

    // GameLoop integration
    void SendMessage(const std::string& endpoint, const std::string& data);
    bool HasPendingResponse();
    QueueStatus GetQueueStatus();
    bool IsStreamInProgress();
}
