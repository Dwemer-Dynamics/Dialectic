// ResponseQueueFNV.h - CHIM-style parsed response queue for Dialectic JSON lines

#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace ResponseQueueFNV {

struct DialogueLine {
    std::string text;
    std::string speaker;
    std::string displayName;
    uint32_t actorFormId = 0;
    bool isFinalResponseLine = false;
    bool directorScene = false;
    std::string listenerHint;
    std::string rechatTargetHint;
    uint32_t listenerFormId = 0;
    uint32_t rechatTargetFormId = 0;
    int rechatDepth = 0;
    std::string ttsCacheKey;
    std::string utteranceId;
    std::string requestId;
    uint64_t responseGeneration = 0;
    uint64_t runtimeGeneration = 0;
};

struct QueueStatus {
    bool unfinished = false;
    uint64_t activeGeneration = 0;
    uint64_t activeRuntimeGeneration = 0;
    std::size_t pendingItems = 0;
    std::size_t pendingDialogueLines = 0;
    std::size_t pendingActionLines = 0;
    uint64_t totalQueued = 0;
    uint64_t totalDispatched = 0;
    uint64_t totalDroppedStale = 0;
    std::string unfinishedSource;
};

// Track terminal speech delivery once per scene, including chunks dropped before playback.
void BeginDirectorScene(const std::string& id, const std::vector<DialogueLine>& lines, std::size_t actions);
void CompleteDirectorSpeech(const std::string& id, const std::string& utteranceId, const std::string& state);

void SetActiveGeneration(uint64_t generation, const char* source = "ResponseQueueFNV");
bool IsCurrentGeneration(uint64_t generation);
void MarkUnfinished(bool unfinished, const char* source = "ResponseQueueFNV", uint64_t generation = 0);
bool IsUnfinished();

void EnqueueDialogue(const DialogueLine& line, const char* source = "ResponseQueueFNV");
void EnqueueAction(const std::string& lineObject, const char* source = "ResponseQueueFNV", uint64_t responseGeneration = 0, bool directorScene = false, const std::string& directorSceneId = "");

bool DispatchPending(std::size_t maxItems = 64);
void Clear(const char* reason = "clear");
bool HasPending();
QueueStatus GetStatus();

} // namespace ResponseQueueFNV
