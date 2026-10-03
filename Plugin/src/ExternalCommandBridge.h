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
int SendPluginEvent(std::uint32_t actorFormId,
                    const std::string& bridge,
                    const std::string& name,
                    const std::string& data);

// Expires timed-out requests and silently drops requests from older runtime generations.
void Update();
// Drops pending requests without a server result (halt, save/load, shutdown).
void CancelAll(const char* reason);

} // namespace ExternalCommandBridge
