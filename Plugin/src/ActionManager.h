#pragma once

#include <cstdint>
#include <string>

namespace ActionManager {

bool IsActionCommand(const std::string& actionName);
bool HandleRoleCommandJson(const std::string& lineObject,
                           const char* source = "ActionManager",
                           uint64_t runtimeGeneration = 0);
bool ShouldDelayUntilAfterDialogue(const std::string& actionName);
bool QueuePostDialogueActionJson(const std::string& lineObject,
                                 const char* source = "ActionManager",
                                 uint64_t runtimeGeneration = 0);
bool HasPendingPostDialogueActionForSpeaker(const std::string& speaker,
                                            uint32_t actorFormId);
bool FlushPostDialogueActionsForSpeaker(const std::string& speaker,
                                        uint32_t actorFormId,
                                        const char* source = "ActionManager");
void ClearPostDialogueActions();
void ClearScriptBridgeRequest();
void Update();

// Clears native package overrides during save/load, cell, and runtime generation
// transitions. Actors that are not currently loaded are cleaned when they next
// enter the native actor snapshot.
void CancelNativeRuntimeActions(const char* reason = "runtime_invalidated");

// Hard halt: stop active/pending AI action requests and ask the
// script bridge to clear current in-game AI package/combat/action state.
int HaltAIActions(const char* source = "ActionManager");

} // namespace ActionManager
