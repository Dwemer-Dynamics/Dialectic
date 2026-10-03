#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

// Game-independent state for CHIM-style ExtCmd<Bridge>_<Action> requests.
// ExternalCommandBridge owns game-thread dispatch and server reporting.
namespace ExternalCommandRegistry {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kMaxBridges = 32;
constexpr std::size_t kMaxPending = 16;
constexpr std::size_t kMaxBridgeNameLength = 32;
constexpr std::size_t kMaxCommandLength = 96;
constexpr std::size_t kMaxTextLength = 1000;
constexpr std::size_t kMaxEventNameLength = 64;
constexpr std::size_t kMaxOwnerNameLength = 64;
constexpr std::size_t kMaxRecentOutcomes = 64;
constexpr std::size_t kMaxPluginEventsPerWindow = 20;
constexpr auto kRequestTimeout = std::chrono::seconds(30);
constexpr auto kDuplicateWindow = std::chrono::milliseconds(1500);
constexpr auto kPluginEventWindow = std::chrono::seconds(10);

bool IsValidBridgeName(std::string_view name);
bool IsValidEventName(std::string_view name);
// Owner names use the event-name character set: letters, digits, '_', '.', '-'.
bool IsValidOwnerName(std::string_view name);

// Splits "ExtCmd<Bridge>_<Action>" at the first '_' after the prefix, as CHIM does.
bool ParseCommand(std::string_view command, std::string& bridge, std::string& action);

// Plugin script owner: the FNV load-order index (top byte). Runtime-compiled
// scripts (CompileScript, JIP LN script runners) all share owner 0xFF000000.
// Owned bridges use an addon-supplied owner name and a handle instead.
std::uint32_t OwnerKeyFromScriptFormId(std::uint32_t scriptFormId);

enum class RegisterResult { Registered, AlreadyOwned, InvalidName, InvalidOwner, OwnedByOther, Full };
enum class BeginResult {
    Accepted,
    InvalidCommand,
    InvalidParameter,
    InvalidActor,
    BridgeNotRegistered,
    Duplicate,
    TooManyPending
};
enum class CompleteResult { Completed, UnknownRequest, BridgeMismatch, ActorMismatch, OwnerMismatch };
// Script-visible request status; values are part of the public API.
enum class Outcome { Unknown = 0, Pending = 1, Completed = 2, Failed = 3, TimedOut = 4, Cancelled = 5 };

const char* ToString(RegisterResult result);
const char* ToString(BeginResult result);
const char* ToString(CompleteResult result);

struct PendingRequest {
    std::uint32_t requestId = 0;
    std::string bridge;
    std::string command;
    std::string action;
    std::string parameter;
    std::string speaker;
    std::uint32_t actorFormId = 0;
    // Owned-bridge handle; 0 for bridges registered through the legacy command.
    std::uint32_t handle = 0;
    std::uint64_t generation = 0;
    Clock::time_point acceptedAt{};
    Clock::time_point deadline{};
};

class Registry {
public:
    RegisterResult RegisterBridge(std::string_view name, std::uint32_t ownerKey);
    // Registers or re-registers a bridge for a named owner. On success, handle is
    // stable for this bridge and owner for the rest of the session.
    RegisterResult RegisterOwnedBridge(std::string_view name, std::string_view owner, std::uint32_t& handle);
    // Returns the registered spelling, or an empty string.
    std::string RegisteredBridgeName(std::string_view name) const;

    BeginResult Begin(const std::string& command,
                      const std::string& parameter,
                      const std::string& speaker,
                      std::uint32_t actorFormId,
                      std::uint64_t generation,
                      Clock::time_point now,
                      PendingRequest& accepted);
    // Legacy completion by bridge name; rejects requests for owned bridges.
    CompleteResult Complete(std::string_view bridge,
                            std::uint32_t requestId,
                            std::uint32_t actorFormId,
                            PendingRequest& completed,
                            bool succeeded = true);
    CompleteResult CompleteOwned(std::uint32_t handle,
                                 std::uint32_t requestId,
                                 std::uint32_t actorFormId,
                                 bool succeeded,
                                 PendingRequest& completed);
    bool Get(std::uint32_t requestId, PendingRequest& pending) const;
    bool Cancel(std::uint32_t requestId, PendingRequest& cancelled, Outcome outcome = Outcome::Cancelled);
    bool IsPending(std::uint32_t requestId) const;
    std::size_t PendingCount() const;
    // Pending or one of the last kMaxRecentOutcomes outcomes, if handle matches.
    Outcome Status(std::uint32_t handle, std::uint32_t requestId) const;

    std::vector<PendingRequest> TakeExpired(Clock::time_point now);
    std::vector<PendingRequest> TakeStale(std::uint64_t currentGeneration);
    std::vector<PendingRequest> TakeAll();

    // Bounds plugin-event traffic per registered bridge.
    bool AllowPluginEvent(std::string_view bridge, Clock::time_point now);

private:
    struct Bridge {
        std::string name;
        std::uint32_t ownerKey = 0;
        std::string owner;
        std::uint32_t handle = 0;
    };
    struct RecentOutcome {
        std::uint32_t requestId = 0;
        std::uint32_t handle = 0;
        Outcome outcome = Outcome::Unknown;
    };

    CompleteResult CompleteLocked(std::map<std::uint32_t, PendingRequest>::iterator pending,
                                  std::uint32_t actorFormId,
                                  bool succeeded,
                                  PendingRequest& completed);
    void RecordLocked(const PendingRequest& request, Outcome outcome);

    mutable std::mutex m_mutex;
    std::map<std::string, Bridge> m_bridges;
    std::map<std::uint32_t, PendingRequest> m_pending;
    std::map<std::string, Clock::time_point> m_recent;
    std::map<std::string, std::vector<Clock::time_point>> m_pluginEvents;
    std::deque<RecentOutcome> m_outcomes;
    std::uint32_t m_nextRequestId = 1;
    std::uint32_t m_nextHandle = 1;
};

// JSON body for HTTPManager::SendEvent("pluginevent", ...).
std::string BuildPluginEventPayload(const std::string& bridge,
                                    const std::string& name,
                                    const std::string& data,
                                    const std::string& actorName,
                                    std::uint32_t actorFormId);

std::string EscapeJson(std::string_view value);
std::string FormatRefId(std::uint32_t formId);

} // namespace ExternalCommandRegistry
