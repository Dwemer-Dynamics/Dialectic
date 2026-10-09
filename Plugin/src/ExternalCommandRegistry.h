#pragma once

#include <atomic>
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
constexpr std::size_t kMaxControlledActors = 64;
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
    // True for a handle returned by RegisterOwnedBridge this session.
    bool IsOwnedHandle(std::uint32_t handle) const;
    // The registered spelling of an owned bridge, or an empty string.
    std::string OwnedBridgeName(std::uint32_t handle) const;
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

// Script-visible actor control flags; values are part of the public API.
enum ActorFlag : std::uint32_t { kTalkLock = 1, kAnimationBusy = 2 };
enum class ControlResult { Applied, StillHeldByOther, InvalidHandle, InvalidActor, Full };

// Per-actor flags claimed by owned-bridge handles. Each owner holds its own
// claim, so one addon cannot release another addon's flag. Handles are at most
// kMaxBridges, so each flag stores one bit per owner.
class ActorControlTable {
public:
    ControlResult Set(std::uint32_t handle, std::uint32_t actorFormId, ActorFlag flag, bool active);
    // Flags held by any owner, or only by handle when it is nonzero.
    std::uint32_t Flags(std::uint32_t actorFormId, std::uint32_t handle = 0) const;
    // Returns the number of actors whose flags were dropped.
    std::size_t Clear();

private:
    struct Claims {
        std::uint32_t talkLock = 0;
        std::uint32_t animationBusy = 0;
    };

    mutable std::mutex m_mutex;
    std::map<std::uint32_t, Claims> m_actors;
    // Lets hot-path readers skip the lock while no addon holds a flag.
    std::atomic<std::size_t> m_count{0};
};

// Addon agent queries. Candidates come from the existing native actor snapshot,
// already limited to loaded, living actors in the player's scene.
constexpr std::size_t kMaxAgentQueryResults = 32;
// Script-visible actor filter; values are part of the public API.
enum class AgentFilter { Agents = 0, NonAgents = 1, All = 2 };

struct AgentCandidate {
    std::uint32_t formId = 0;
    std::string name;
    float distance = 0.0f;
    bool agent = false;
};

bool IsValidAgentFilter(int filter);
// Closest first (form ID breaks ties), at most min(limit, kMaxAgentQueryResults).
// maxDistance <= 0 keeps every candidate.
std::vector<std::uint32_t> SelectAgentCandidates(std::vector<AgentCandidate> candidates,
                                                 AgentFilter filter,
                                                 int limit,
                                                 float maxDistance);
// The only registered agent whose name matches case-insensitively, or 0 when
// none or several match. Lookup only; operations stay bound to the returned ref.
std::uint32_t FindUniqueAgentByName(const std::vector<AgentCandidate>& candidates,
                                    std::string_view name,
                                    bool& ambiguous);

// Coalesces repeated addon context-refresh requests per key (actor ref, or a
// player-context key). Bounded: the oldest entries are dropped past the cap.
class RefreshThrottle {
public:
    static constexpr std::size_t kMaxKeys = 64;
    static constexpr auto kWindow = std::chrono::seconds(5);

    // True when no refresh for key started within kWindow.
    bool TryBegin(std::uint32_t key, Clock::time_point now);
    void Clear();

private:
    std::mutex m_mutex;
    std::map<std::uint32_t, Clock::time_point> m_last;
};

// JSON body for an addon context event, sent as "pluginevent". type and name follow the
// event-name rules; text is 1 to kMaxTextLength bytes. The bridge namespaces the state.
std::string BuildAddonContextPayload(const std::string& bridge,
                                     const std::string& type,
                                     const std::string& name,
                                     const std::string& text,
                                     const std::string& actorName,
                                     std::uint32_t actorFormId);

// JSON body for HTTPManager::SendEvent("pluginevent", ...).
std::string BuildPluginEventPayload(const std::string& bridge,
                                    const std::string& name,
                                    const std::string& data,
                                    const std::string& actorName,
                                    std::uint32_t actorFormId);

std::string EscapeJson(std::string_view value);
std::string FormatRefId(std::uint32_t formId);

} // namespace ExternalCommandRegistry
