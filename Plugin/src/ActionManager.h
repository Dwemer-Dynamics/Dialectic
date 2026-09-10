#pragma once

#include <cstdint>
#include <string>

namespace ActionManager {

enum class ExternalFollowerAction {
    Recruit,
    Dismiss,
    Wait,
    Resume
};

bool IsActionCommand(const std::string& actionName);
// Scene actions must execute directly, with narrator powers confined to the narrator.
bool IsDirectorSceneAction(const std::string& actionName, bool narrator);
bool RequestWaitHere(uint32_t actorFormId,
                     const std::string& actorName,
                     const char* source = "ActionManager");
bool RequestExternalFollowerAction(ExternalFollowerAction action,
                                   uint32_t actorFormId,
                                   const std::string& actorName,
                                   const char* source = "xNVSEEvent");
bool HandleRoleCommandJson(const std::string& lineObject,
                           const char* source = "ActionManager",
                           uint64_t runtimeGeneration = 0);
void Update();

// Clears native package overrides during save/load, cell, and runtime generation
// transitions. Actors that are not currently loaded are cleaned when they next
// enter the native actor snapshot.
void CancelNativeRuntimeActions(const char* reason = "runtime_invalidated");

// Hard halt: stop active/pending AI action requests and ask the
// script bridge to clear current in-game AI package/combat/action state.
int HaltAIActions(const char* source = "ActionManager");

} // namespace ActionManager
