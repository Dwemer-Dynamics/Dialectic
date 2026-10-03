#pragma once

#include <cstdint>
#include <string>

// CHIM-compatible ExtCmd<Bridge>_<Action> routing to registered xNVSE addons.
// Acceptance only means the request is pending; the server receives a funcret
// result only after the addon completes it, the request fails or it times out.
namespace ExternalCommandBridge {

bool Initialize();

// Server rolecommand entry point. Returns true when the request was accepted
// for dispatch or suppressed as a duplicate.
bool HandleServerCommand(const std::string& command,
                         const std::string& parameter,
                         const std::string& speaker,
                         std::uint32_t speakerFormId,
                         std::uint64_t runtimeGeneration,
                         const char* source);

// Script command entry points (game thread). Each returns 1 on acceptance, 0 on rejection.
int RegisterBridge(const std::string& name, std::uint32_t scriptFormId);
int CompleteRequest(std::uint32_t actorFormId,
                    const std::string& bridge,
                    int requestId,
                    bool succeeded,
                    const std::string& result);
int IsRequestPending(int requestId);
// Owned-bridge entry points (game thread). RegisterOwnedBridge returns a positive
// handle, or -1 invalid bridge name, -2 invalid owner, -3 owned by another, -4 full.
int RegisterOwnedBridge(const std::string& name, const std::string& owner);
int CompleteOwnedRequest(std::uint32_t actorFormId,
                         int handle,
                         int requestId,
                         bool succeeded,
                         const std::string& result);
// ExternalCommandRegistry::Outcome value; handle 0 queries legacy-bridge requests.
int GetRequestStatus(int handle, int requestId);
int SendPluginEvent(std::uint32_t actorFormId,
                    const std::string& bridge,
                    const std::string& name,
                    const std::string& data);

// Addon control (game thread). SetActorFlag returns 1 applied, 2 released while another
// owner still holds the flag, -1 unknown handle, -2 invalid actor, -3 full (64 actors),
// -4 value other than 0 or 1. flag is an ExternalCommandRegistry::ActorFlag.
int SetActorFlag(std::uint32_t actorFormId, int handle, std::uint32_t flag, int active);
// Bitmask of ExternalCommandRegistry::ActorFlag values; handle 0 reads every owner.
int GetActorFlags(std::uint32_t actorFormId, int handle);
// Hot-path gates; a lock-free load while no addon holds a flag. Either flag blocks
// Dialectic speech; only animation busy also blocks Dialectic actions on the actor.
bool IsActorTalkBlocked(std::uint32_t actorFormId);
bool IsActorAnimationBusy(std::uint32_t actorFormId);
// "talk_locked", "animation_busy" or nullptr.
const char* ActorBlockReason(std::uint32_t actorFormId);
// 1 already in that state, 2 change accepted (synchronizing), -1 unknown handle, -2 bad value.
int SetInteractionEnabled(int handle, int enabled);
// Drops every actor flag (save load, main menu, exit, API shutdown).
void ClearActorFlags(const char* reason);

// Expires timed-out requests and silently drops requests from older runtime generations.
void Update();
// Drops pending requests without a server result (halt, save/load, shutdown).
void CancelAll(const char* reason);

} // namespace ExternalCommandBridge
