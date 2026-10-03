#include "ExternalCommandRegistry.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace ExternalCommandRegistry {
namespace {

constexpr std::string_view kCommandPrefix = "ExtCmd";
constexpr std::uint32_t kPlayerRefId = 0x00000014;
constexpr std::uint32_t kMaxRequestId = 0x7FFFFFFF;

std::string Lower(std::string_view value) {
    std::string lowered(value);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return lowered;
}

bool IsNameCharacter(unsigned char ch) {
    return std::isalnum(ch) != 0;
}

std::string DuplicateKey(const std::string& command, const std::string& parameter, std::uint32_t actorFormId) {
    return Lower(command) + "|" + parameter + "|" + std::to_string(actorFormId);
}

} // namespace

bool IsValidBridgeName(std::string_view name) {
    if (name.empty() || name.size() > kMaxBridgeNameLength) return false;
    if (std::isalpha(static_cast<unsigned char>(name.front())) == 0) return false;
    return std::all_of(name.begin(), name.end(), [](char ch) {
        return IsNameCharacter(static_cast<unsigned char>(ch));
    });
}

bool IsValidEventName(std::string_view name) {
    if (name.empty() || name.size() > kMaxEventNameLength) return false;
    return std::all_of(name.begin(), name.end(), [](char ch) {
        const auto value = static_cast<unsigned char>(ch);
        return IsNameCharacter(value) || value == '_' || value == '.' || value == '-';
    });
}

bool IsValidOwnerName(std::string_view name) {
    return name.size() <= kMaxOwnerNameLength && IsValidEventName(name);
}

bool ParseCommand(std::string_view command, std::string& bridge, std::string& action) {
    bridge.clear();
    action.clear();
    if (command.size() > kMaxCommandLength || command.substr(0, kCommandPrefix.size()) != kCommandPrefix) {
        return false;
    }
    const std::size_t separator = command.find('_', kCommandPrefix.size());
    if (separator == std::string_view::npos || separator == kCommandPrefix.size() ||
        separator + 1 >= command.size()) {
        return false;
    }
    const std::string_view bridgeName = command.substr(kCommandPrefix.size(), separator - kCommandPrefix.size());
    const std::string_view actionName = command.substr(separator + 1);
    const bool validAction = std::all_of(actionName.begin(), actionName.end(), [](char ch) {
        const auto value = static_cast<unsigned char>(ch);
        return IsNameCharacter(value) || value == '_';
    });
    if (!IsValidBridgeName(bridgeName) || !validAction) {
        return false;
    }
    bridge.assign(bridgeName);
    action.assign(actionName);
    return true;
}

std::uint32_t OwnerKeyFromScriptFormId(std::uint32_t scriptFormId) {
    // FNV has no light-plugin slot: TESForm::GetModIndex() is the top byte, and
    // runtime-compiled scripts all share index 0xFF.
    return scriptFormId & 0xFF000000u;
}

const char* ToString(RegisterResult result) {
    switch (result) {
        case RegisterResult::Registered: return "registered";
        case RegisterResult::AlreadyOwned: return "already_owned";
        case RegisterResult::InvalidName: return "invalid_bridge_name";
        case RegisterResult::InvalidOwner: return "invalid_owner_name";
        case RegisterResult::OwnedByOther: return "bridge_owned_by_another_plugin";
        case RegisterResult::Full: return "bridge_table_full";
    }
    return "unknown";
}

const char* ToString(BeginResult result) {
    switch (result) {
        case BeginResult::Accepted: return "accepted";
        case BeginResult::InvalidCommand: return "invalid_command";
        case BeginResult::InvalidParameter: return "invalid_parameter";
        case BeginResult::InvalidActor: return "speaker_refid_invalid";
        case BeginResult::BridgeNotRegistered: return "bridge_not_registered";
        case BeginResult::Duplicate: return "duplicate";
        case BeginResult::TooManyPending: return "too_many_pending_requests";
    }
    return "unknown";
}

const char* ToString(CompleteResult result) {
    switch (result) {
        case CompleteResult::Completed: return "completed";
        case CompleteResult::UnknownRequest: return "unknown_or_expired_request";
        case CompleteResult::BridgeMismatch: return "bridge_mismatch";
        case CompleteResult::ActorMismatch: return "actor_mismatch";
        case CompleteResult::OwnerMismatch: return "owner_mismatch";
    }
    return "unknown";
}

RegisterResult Registry::RegisterBridge(std::string_view name, std::uint32_t ownerKey) {
    if (!IsValidBridgeName(name)) return RegisterResult::InvalidName;
    std::lock_guard<std::mutex> lock(m_mutex);
    const std::string key = Lower(name);
    const auto existing = m_bridges.find(key);
    if (existing != m_bridges.end()) {
        return existing->second.handle == 0 && existing->second.ownerKey == ownerKey
            ? RegisterResult::AlreadyOwned : RegisterResult::OwnedByOther;
    }
    if (m_bridges.size() >= kMaxBridges) return RegisterResult::Full;
    m_bridges.emplace(key, Bridge{std::string(name), ownerKey, std::string(), 0});
    return RegisterResult::Registered;
}

RegisterResult Registry::RegisterOwnedBridge(std::string_view name, std::string_view owner, std::uint32_t& handle) {
    handle = 0;
    if (!IsValidBridgeName(name)) return RegisterResult::InvalidName;
    if (!IsValidOwnerName(owner)) return RegisterResult::InvalidOwner;
    std::lock_guard<std::mutex> lock(m_mutex);
    const std::string key = Lower(name);
    const std::string ownerKey = Lower(owner);
    const auto existing = m_bridges.find(key);
    if (existing != m_bridges.end()) {
        if (existing->second.handle == 0 || existing->second.owner != ownerKey) return RegisterResult::OwnedByOther;
        handle = existing->second.handle;
        return RegisterResult::AlreadyOwned;
    }
    if (m_bridges.size() >= kMaxBridges) return RegisterResult::Full;
    // Bridges are never removed, so at most kMaxBridges handles are issued per session.
    handle = m_nextHandle++;
    m_bridges.emplace(key, Bridge{std::string(name), 0, ownerKey, handle});
    return RegisterResult::Registered;
}

std::string Registry::RegisteredBridgeName(std::string_view name) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto existing = m_bridges.find(Lower(name));
    return existing == m_bridges.end() ? std::string() : existing->second.name;
}

BeginResult Registry::Begin(const std::string& command,
                            const std::string& parameter,
                            const std::string& speaker,
                            std::uint32_t actorFormId,
                            std::uint64_t generation,
                            Clock::time_point now,
                            PendingRequest& accepted) {
    accepted = PendingRequest{};
    std::string bridge;
    std::string action;
    if (!ParseCommand(command, bridge, action)) return BeginResult::InvalidCommand;
    if (parameter.size() > kMaxTextLength) return BeginResult::InvalidParameter;
    if (actorFormId == 0 || actorFormId == kPlayerRefId) return BeginResult::InvalidActor;

    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto it = m_recent.begin(); it != m_recent.end();) {
        it = now - it->second > kDuplicateWindow ? m_recent.erase(it) : std::next(it);
    }
    const std::string duplicateKey = DuplicateKey(command, parameter, actorFormId);
    if (m_recent.count(duplicateKey) != 0) return BeginResult::Duplicate;
    for (const auto& [id, pending] : m_pending) {
        if (DuplicateKey(pending.command, pending.parameter, pending.actorFormId) == duplicateKey) {
            return BeginResult::Duplicate;
        }
    }
    // Rejections are also remembered so a repeated response line reports one failure.
    m_recent[duplicateKey] = now;
    const auto registered = m_bridges.find(Lower(bridge));
    if (registered == m_bridges.end()) return BeginResult::BridgeNotRegistered;
    if (m_pending.size() >= kMaxPending) return BeginResult::TooManyPending;

    std::uint32_t requestId = m_nextRequestId;
    while (m_pending.count(requestId) != 0) {
        requestId = requestId >= kMaxRequestId ? 1 : requestId + 1;
    }
    m_nextRequestId = requestId >= kMaxRequestId ? 1 : requestId + 1;

    accepted.requestId = requestId;
    accepted.bridge = registered->second.name;
    accepted.command = command;
    accepted.action = action;
    accepted.parameter = parameter;
    accepted.speaker = speaker;
    accepted.actorFormId = actorFormId;
    accepted.handle = registered->second.handle;
    accepted.generation = generation;
    accepted.acceptedAt = now;
    accepted.deadline = now + kRequestTimeout;
    m_pending.emplace(requestId, accepted);
    return BeginResult::Accepted;
}

CompleteResult Registry::Complete(std::string_view bridge,
                                  std::uint32_t requestId,
                                  std::uint32_t actorFormId,
                                  PendingRequest& completed,
                                  bool succeeded) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto pending = m_pending.find(requestId);
    if (pending == m_pending.end()) return CompleteResult::UnknownRequest;
    if (Lower(pending->second.bridge) != Lower(bridge)) return CompleteResult::BridgeMismatch;
    if (pending->second.handle != 0) return CompleteResult::OwnerMismatch;
    return CompleteLocked(pending, actorFormId, succeeded, completed);
}

CompleteResult Registry::CompleteOwned(std::uint32_t handle,
                                       std::uint32_t requestId,
                                       std::uint32_t actorFormId,
                                       bool succeeded,
                                       PendingRequest& completed) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto pending = m_pending.find(requestId);
    if (pending == m_pending.end()) return CompleteResult::UnknownRequest;
    if (handle == 0 || pending->second.handle != handle) return CompleteResult::OwnerMismatch;
    return CompleteLocked(pending, actorFormId, succeeded, completed);
}

CompleteResult Registry::CompleteLocked(std::map<std::uint32_t, PendingRequest>::iterator pending,
                                        std::uint32_t actorFormId,
                                        bool succeeded,
                                        PendingRequest& completed) {
    if (pending->second.actorFormId != actorFormId) return CompleteResult::ActorMismatch;
    completed = std::move(pending->second);
    m_pending.erase(pending);
    RecordLocked(completed, succeeded ? Outcome::Completed : Outcome::Failed);
    return CompleteResult::Completed;
}

void Registry::RecordLocked(const PendingRequest& request, Outcome outcome) {
    if (m_outcomes.size() >= kMaxRecentOutcomes) m_outcomes.pop_front();
    m_outcomes.push_back(RecentOutcome{request.requestId, request.handle, outcome});
}

bool Registry::Get(std::uint32_t requestId, PendingRequest& pending) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto existing = m_pending.find(requestId);
    if (existing == m_pending.end()) return false;
    pending = existing->second;
    return true;
}

bool Registry::Cancel(std::uint32_t requestId, PendingRequest& cancelled, Outcome outcome) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto pending = m_pending.find(requestId);
    if (pending == m_pending.end()) return false;
    cancelled = std::move(pending->second);
    m_pending.erase(pending);
    RecordLocked(cancelled, outcome);
    return true;
}

bool Registry::IsOwnedHandle(std::uint32_t handle) const {
    if (handle == 0) return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    return std::any_of(m_bridges.begin(), m_bridges.end(), [handle](const auto& bridge) {
        return bridge.second.handle == handle;
    });
}

std::string Registry::OwnedBridgeName(std::uint32_t handle) const {
    if (handle == 0) return {};
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& bridge : m_bridges) {
        if (bridge.second.handle == handle) return bridge.second.name;
    }
    return {};
}

bool Registry::IsPending(std::uint32_t requestId) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_pending.count(requestId) != 0;
}

std::size_t Registry::PendingCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_pending.size();
}

Outcome Registry::Status(std::uint32_t handle, std::uint32_t requestId) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto pending = m_pending.find(requestId);
    if (pending != m_pending.end()) {
        return pending->second.handle == handle ? Outcome::Pending : Outcome::Unknown;
    }
    for (auto it = m_outcomes.rbegin(); it != m_outcomes.rend(); ++it) {
        if (it->requestId == requestId) return it->handle == handle ? it->outcome : Outcome::Unknown;
    }
    return Outcome::Unknown;
}

std::vector<PendingRequest> Registry::TakeExpired(Clock::time_point now) {
    std::vector<PendingRequest> expired;
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto it = m_pending.begin(); it != m_pending.end();) {
        if (now >= it->second.deadline) {
            RecordLocked(it->second, Outcome::TimedOut);
            expired.push_back(std::move(it->second));
            it = m_pending.erase(it);
        } else {
            ++it;
        }
    }
    return expired;
}

std::vector<PendingRequest> Registry::TakeStale(std::uint64_t currentGeneration) {
    std::vector<PendingRequest> stale;
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto it = m_pending.begin(); it != m_pending.end();) {
        if (it->second.generation != currentGeneration) {
            RecordLocked(it->second, Outcome::Cancelled);
            stale.push_back(std::move(it->second));
            it = m_pending.erase(it);
        } else {
            ++it;
        }
    }
    return stale;
}

std::vector<PendingRequest> Registry::TakeAll() {
    std::vector<PendingRequest> all;
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& [id, pending] : m_pending) {
        RecordLocked(pending, Outcome::Cancelled);
        all.push_back(std::move(pending));
    }
    m_pending.clear();
    m_recent.clear();
    return all;
}

bool Registry::AllowPluginEvent(std::string_view bridge, Clock::time_point now) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const std::string key = Lower(bridge);
    if (m_bridges.count(key) == 0) return false;
    auto& sent = m_pluginEvents[key];
    sent.erase(std::remove_if(sent.begin(), sent.end(), [now](Clock::time_point at) {
        return now - at >= kPluginEventWindow;
    }), sent.end());
    if (sent.size() >= kMaxPluginEventsPerWindow) return false;
    sent.push_back(now);
    return true;
}

ControlResult ActorControlTable::Set(std::uint32_t handle,
                                     std::uint32_t actorFormId,
                                     ActorFlag flag,
                                     bool active) {
    if (handle == 0 || handle > kMaxBridges) return ControlResult::InvalidHandle;
    if (actorFormId == 0 || actorFormId == kPlayerRefId) return ControlResult::InvalidActor;
    const std::uint32_t bit = 1u << (handle - 1);
    std::lock_guard<std::mutex> lock(m_mutex);
    auto existing = m_actors.find(actorFormId);
    if (existing == m_actors.end()) {
        if (!active) return ControlResult::Applied;
        if (m_actors.size() >= kMaxControlledActors) return ControlResult::Full;
        existing = m_actors.emplace(actorFormId, Claims{}).first;
    }
    std::uint32_t& owners = flag == kTalkLock ? existing->second.talkLock : existing->second.animationBusy;
    owners = active ? owners | bit : owners & ~bit;
    const bool stillHeld = !active && owners != 0;
    if (existing->second.talkLock == 0 && existing->second.animationBusy == 0) m_actors.erase(existing);
    m_count.store(m_actors.size(), std::memory_order_release);
    return stillHeld ? ControlResult::StillHeldByOther : ControlResult::Applied;
}

std::uint32_t ActorControlTable::Flags(std::uint32_t actorFormId, std::uint32_t handle) const {
    if (actorFormId == 0 || m_count.load(std::memory_order_acquire) == 0) return 0;
    const std::uint32_t mask = handle == 0 ? ~0u : handle <= kMaxBridges ? 1u << (handle - 1) : 0u;
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto existing = m_actors.find(actorFormId);
    if (existing == m_actors.end()) return 0;
    return ((existing->second.talkLock & mask) != 0 ? kTalkLock : 0u) |
        ((existing->second.animationBusy & mask) != 0 ? kAnimationBusy : 0u);
}

std::size_t ActorControlTable::Clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    const std::size_t cleared = m_actors.size();
    m_actors.clear();
    m_count.store(0, std::memory_order_release);
    return cleared;
}

bool IsValidAgentFilter(int filter) {
    return filter >= static_cast<int>(AgentFilter::Agents) && filter <= static_cast<int>(AgentFilter::All);
}

std::vector<std::uint32_t> SelectAgentCandidates(std::vector<AgentCandidate> candidates,
                                                 AgentFilter filter,
                                                 int limit,
                                                 float maxDistance) {
    std::vector<std::uint32_t> selected;
    if (limit <= 0) return selected;
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
        [filter, maxDistance](const AgentCandidate& candidate) {
            return candidate.formId == 0 ||
                (filter == AgentFilter::Agents && !candidate.agent) ||
                (filter == AgentFilter::NonAgents && candidate.agent) ||
                (maxDistance > 0.0f && candidate.distance > maxDistance);
        }), candidates.end());
    std::sort(candidates.begin(), candidates.end(), [](const AgentCandidate& left, const AgentCandidate& right) {
        return left.distance != right.distance ? left.distance < right.distance : left.formId < right.formId;
    });
    const std::size_t count = std::min({ candidates.size(), static_cast<std::size_t>(limit), kMaxAgentQueryResults });
    selected.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        selected.push_back(candidates[i].formId);
    }
    return selected;
}

std::uint32_t FindUniqueAgentByName(const std::vector<AgentCandidate>& candidates,
                                    std::string_view name,
                                    bool& ambiguous) {
    ambiguous = false;
    if (name.empty()) return 0;
    std::uint32_t match = 0;
    for (const AgentCandidate& candidate : candidates) {
        if (!candidate.agent || candidate.formId == 0 || candidate.name.size() != name.size() ||
            !std::equal(name.begin(), name.end(), candidate.name.begin(), [](char left, char right) {
                return std::tolower(static_cast<unsigned char>(left)) ==
                    std::tolower(static_cast<unsigned char>(right));
            })) {
            continue;
        }
        if (match != 0 && match != candidate.formId) {
            ambiguous = true;
            return 0;
        }
        match = candidate.formId;
    }
    return match;
}

bool RefreshThrottle::TryBegin(std::uint32_t key, Clock::time_point now) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_last.find(key);
    if (it != m_last.end() && now - it->second < kWindow) return false;
    if (it == m_last.end() && m_last.size() >= kMaxKeys) {
        m_last.erase(std::min_element(m_last.begin(), m_last.end(),
            [](const auto& left, const auto& right) { return left.second < right.second; }));
    }
    m_last[key] = now;
    return true;
}

void RefreshThrottle::Clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_last.clear();
}

std::string EscapeJson(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (const char ch : value) {
        switch (ch) {
            case '"': escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned char>(ch));
                    escaped += buffer;
                } else {
                    escaped += ch;
                }
        }
    }
    return escaped;
}

std::string FormatRefId(std::uint32_t formId) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "0x%08X", formId);
    return buffer;
}

std::string BuildPluginEventPayload(const std::string& bridge,
                                    const std::string& name,
                                    const std::string& data,
                                    const std::string& actorName,
                                    std::uint32_t actorFormId) {
    std::string payload = "{\"schema\":\"dialectic.plugin_event.v1\",\"bridge\":\"" + EscapeJson(bridge) +
        "\",\"name\":\"" + EscapeJson(name) + "\",\"data\":\"" + EscapeJson(data) + "\"";
    if (actorFormId != 0) {
        payload += ",\"actor\":\"" + EscapeJson(actorName) + "\",\"actor_refid\":\"" + FormatRefId(actorFormId) + "\"";
    }
    payload += "}";
    return payload;
}

std::string BuildAddonContextPayload(const std::string& bridge,
                                     const std::string& type,
                                     const std::string& name,
                                     const std::string& text,
                                     const std::string& actorName,
                                     std::uint32_t actorFormId) {
    std::string payload = "{\"schema\":\"dialectic.addon_context.v1\",\"bridge\":\"" + EscapeJson(bridge) +
        "\",\"type\":\"" + EscapeJson(type) + "\",\"name\":\"" + EscapeJson(name) +
        "\",\"text\":\"" + EscapeJson(text) + "\"";
    if (actorFormId != 0) {
        payload += ",\"actor\":\"" + EscapeJson(actorName) + "\",\"actor_refid\":\"" + FormatRefId(actorFormId) + "\"";
    }
    payload += "}";
    return payload;
}

} // namespace ExternalCommandRegistry
