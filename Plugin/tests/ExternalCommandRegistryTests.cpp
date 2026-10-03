#include "ExternalCommandRegistry.h"

#include <iostream>
#include <string>

namespace {

int g_failures = 0;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++g_failures;
    }
}

using namespace ExternalCommandRegistry;

constexpr std::uint32_t kProbeOwner = 0x12000000;
constexpr std::uint32_t kOtherOwner = 0x13000000;
constexpr std::uint32_t kVeronica = 0x000E32A9;
constexpr std::uint32_t kBoone = 0x00092BD2;

void TestCommandParsing() {
    std::string bridge;
    std::string action;
    Expect(ParseCommand("ExtCmdParityProbe_Ping", bridge, action), "CHIM command parses");
    Expect(bridge == "ParityProbe" && action == "Ping", "bridge/action split at first underscore");
    Expect(ParseCommand("ExtCmdParityProbe_Ping_Twice", bridge, action) && action == "Ping_Twice",
        "action keeps later underscores");
    Expect(!ParseCommand("ExtCmd_Ping", bridge, action), "empty bridge rejected");
    Expect(!ParseCommand("ExtCmdParityProbe_", bridge, action), "empty action rejected");
    Expect(!ParseCommand("ExtCmdParityProbe", bridge, action), "missing separator rejected");
    Expect(!ParseCommand("IntCmdParityProbe_Ping", bridge, action), "other prefixes rejected");
    Expect(!ParseCommand("ExtCmdhttp://x_Ping", bridge, action), "bridge names are identifiers only");
    Expect(!ParseCommand("ExtCmd1Probe_Ping", bridge, action), "bridge names start with a letter");
    Expect(OwnerKeyFromScriptFormId(0x12001234) == 0x12000000, "regular plugin owner key");
    Expect(OwnerKeyFromScriptFormId(0xFE003ABC) == 0xFE000000, "FNV has no light-plugin sub-slot");
    Expect(OwnerKeyFromScriptFormId(0xFF000801) == OwnerKeyFromScriptFormId(0xFF123456),
        "runtime-compiled scripts share one owner");
}

void TestOwnershipAndDispatchEnvelope() {
    Registry registry;
    const auto now = Clock::now();
    PendingRequest request;
    Expect(registry.Begin("ExtCmdParityProbe_Ping", "hello", "Veronica", kVeronica, 7, now, request) ==
            BeginResult::BridgeNotRegistered, "unregistered bridge fails instead of falling back");
    Expect(registry.Begin("ExtCmdParityProbe_Ping", "hello", "Veronica", kVeronica, 7, now, request) ==
            BeginResult::Duplicate, "repeated rejected line reports one failure");

    Expect(registry.RegisterBridge("ParityProbe", kProbeOwner) == RegisterResult::Registered, "bridge registers");
    Expect(registry.RegisterBridge("parityprobe", kProbeOwner) == RegisterResult::AlreadyOwned,
        "same owner may re-register after load");
    Expect(registry.RegisterBridge("PARITYPROBE", kOtherOwner) == RegisterResult::OwnedByOther,
        "another plugin cannot claim the bridge");

    Expect(registry.Begin("ExtCmdParityProbe_Ping", "hello", "Veronica", 0, 7, now, request) ==
            BeginResult::InvalidActor, "missing exact speaker ref rejected");
    Expect(registry.Begin("ExtCmdParityProbe_Ping", "hello", "Player", 0x14, 7, now, request) ==
            BeginResult::InvalidActor, "player is not an external command speaker");
    Expect(registry.Begin("ExtCmdParityProbe_Ping", std::string(1001, 'x'), "Veronica", kVeronica, 7, now,
            request) == BeginResult::InvalidParameter, "oversized parameter rejected");

    const auto later = now + kDuplicateWindow + std::chrono::milliseconds(1);
    Expect(registry.Begin("ExtCmdparityprobe_Ping", "hello", "Veronica", kVeronica, 7, later, request) ==
            BeginResult::Accepted, "registered bridge accepts request");
    Expect(request.requestId != 0 && request.bridge == "ParityProbe" && request.action == "Ping" &&
            request.command == "ExtCmdparityprobe_Ping" && request.parameter == "hello" &&
            request.actorFormId == kVeronica && request.generation == 7,
        "dispatch envelope keeps CHIM command/parameter and separate identity");
    Expect(registry.IsPending(request.requestId), "accepted request is pending, not complete");

    PendingRequest duplicate;
    Expect(registry.Begin("ExtCmdParityProbe_Ping", "hello", "Veronica", kVeronica, 7, later, duplicate) ==
            BeginResult::Duplicate, "duplicate final-action pass suppressed");

    PendingRequest completed;
    Expect(registry.Complete("ParityProbe", request.requestId, kBoone, completed) == CompleteResult::ActorMismatch,
        "completion from another actor rejected");
    Expect(registry.Complete("OtherBridge", request.requestId, kVeronica, completed) ==
            CompleteResult::BridgeMismatch, "completion from another bridge rejected");
    Expect(registry.Complete("ParityProbe", request.requestId, kVeronica, completed) == CompleteResult::Completed,
        "owning bridge completes on exact actor");
    Expect(completed.requestId == request.requestId && !registry.IsPending(request.requestId),
        "completion removes pending request");
    Expect(registry.Complete("ParityProbe", request.requestId, kVeronica, completed) ==
            CompleteResult::UnknownRequest, "late duplicate completion rejected");
}

void TestBoundsTimeoutAndCancellation() {
    Registry registry;
    registry.RegisterBridge("ParityProbe", kProbeOwner);
    const auto now = Clock::now();
    PendingRequest request;
    for (std::size_t index = 0; index < kMaxPending; ++index) {
        Expect(registry.Begin("ExtCmdParityProbe_Ping", std::to_string(index), "Veronica", kVeronica, 3, now,
                request) == BeginResult::Accepted, "request within pending bound accepted");
    }
    Expect(registry.Begin("ExtCmdParityProbe_Ping", "overflow", "Veronica", kVeronica, 3, now, request) ==
            BeginResult::TooManyPending, "pending table is bounded");

    Expect(registry.TakeExpired(now + kRequestTimeout - std::chrono::milliseconds(1)).empty(),
        "requests do not expire early");
    Expect(registry.TakeStale(3).empty(), "current generation is kept");
    Expect(registry.TakeStale(4).size() == kMaxPending, "save/load generation change cancels pending requests");
    Expect(registry.PendingCount() == 0, "stale requests removed");

    Expect(registry.Begin("ExtCmdParityProbe_Ping", "timeout", "Veronica", kVeronica, 4, now, request) ==
            BeginResult::Accepted, "request accepted after cancellation");
    Expect(registry.TakeExpired(now + kRequestTimeout).size() == 1, "request times out at deadline");
    Expect(registry.Complete("ParityProbe", request.requestId, kVeronica, request) ==
            CompleteResult::UnknownRequest, "timed-out request cannot report completion");
}

void TestRuntimeOwnersAndOutcomes() {
    constexpr std::uint32_t kRuntimeOwner = 0xFF000000;
    Registry registry;
    std::uint32_t probe = 0;
    std::uint32_t other = 0;
    Expect(registry.RegisterOwnedBridge("ParityProbe", "ParityProbe.Example", probe) == RegisterResult::Registered &&
            probe != 0, "owned bridge returns a handle");
    Expect(registry.RegisterOwnedBridge("PARITYPROBE", "parityprobe.example", other) ==
            RegisterResult::AlreadyOwned && other == probe, "same owner keeps its handle after reload");
    Expect(registry.RegisterOwnedBridge("ParityProbe", "OtherAddon", other) == RegisterResult::OwnedByOther &&
            other == 0, "second runtime addon cannot claim an owned bridge");
    Expect(registry.RegisterBridge("ParityProbe", kRuntimeOwner) == RegisterResult::OwnedByOther,
        "legacy runtime owner cannot claim an owned bridge");
    Expect(registry.RegisterBridge("LegacyProbe", kRuntimeOwner) == RegisterResult::Registered &&
            registry.RegisterOwnedBridge("LegacyProbe", "OtherAddon", other) == RegisterResult::OwnedByOther,
        "owned registration cannot take a legacy bridge");
    Expect(registry.RegisterOwnedBridge("OtherBridge", "bad owner", other) == RegisterResult::InvalidOwner,
        "owner names are bounded identifiers");
    Expect(registry.RegisterOwnedBridge("OtherBridge", "OtherAddon", other) == RegisterResult::Registered &&
            other != 0 && other != probe, "distinct runtime owners get distinct handles");

    const auto now = Clock::now();
    PendingRequest first;
    PendingRequest second;
    Expect(registry.Begin("ExtCmdParityProbe_Ping", "a", "Veronica", kVeronica, 9, now, first) ==
            BeginResult::Accepted && first.handle == probe, "owned request carries its owner handle");
    Expect(registry.Begin("ExtCmdOtherBridge_Ping", "b", "Veronica", kVeronica, 9, now, second) ==
            BeginResult::Accepted && second.handle == other, "other owner's request carries its own handle");

    PendingRequest completed;
    Expect(registry.Complete("ParityProbe", first.requestId, kVeronica, completed) == CompleteResult::OwnerMismatch,
        "bridge name alone cannot complete an owned request");
    Expect(registry.CompleteOwned(other, first.requestId, kVeronica, true, completed) ==
            CompleteResult::OwnerMismatch, "another owner's handle cannot complete the request");
    Expect(registry.Status(other, first.requestId) == Outcome::Unknown &&
            registry.Status(probe, first.requestId) == Outcome::Pending, "status is scoped to the owner handle");
    Expect(registry.CompleteOwned(probe, first.requestId, kVeronica, true, completed) == CompleteResult::Completed,
        "owner completes on the exact actor");
    Expect(registry.CompleteOwned(probe, first.requestId, kVeronica, false, completed) ==
            CompleteResult::UnknownRequest && registry.Status(probe, first.requestId) == Outcome::Completed,
        "duplicate completion is rejected and the first outcome kept");
    Expect(registry.CompleteOwned(other, second.requestId, kVeronica, false, completed) ==
            CompleteResult::Completed && registry.Status(other, second.requestId) == Outcome::Failed,
        "addon-reported failure is visible to its owner");

    Expect(registry.Begin("ExtCmdParityProbe_Ping", "timeout", "Veronica", kVeronica, 9, now, first) ==
            BeginResult::Accepted && registry.TakeExpired(now + kRequestTimeout).size() == 1 &&
            registry.Status(probe, first.requestId) == Outcome::TimedOut, "timeout is reported to the owner");
    Expect(registry.Begin("ExtCmdParityProbe_Ping", "load", "Veronica", kVeronica, 9, now, first) ==
            BeginResult::Accepted && registry.TakeStale(10).size() == 1 &&
            registry.Status(probe, first.requestId) == Outcome::Cancelled, "save/load cancellation is local");
    Expect(registry.CompleteOwned(probe, first.requestId, kVeronica, true, completed) ==
            CompleteResult::UnknownRequest, "cancelled request cannot report to the server");
}

void TestPluginEventEnvelope() {
    Registry registry;
    const auto now = Clock::now();
    Expect(!registry.AllowPluginEvent("ParityProbe", now), "plugin events require a registered bridge");
    registry.RegisterBridge("ParityProbe", kProbeOwner);
    for (std::size_t index = 0; index < kMaxPluginEventsPerWindow; ++index) {
        registry.AllowPluginEvent("ParityProbe", now);
    }
    Expect(!registry.AllowPluginEvent("ParityProbe", now), "plugin events are rate limited");
    Expect(registry.AllowPluginEvent("ParityProbe", now + kPluginEventWindow), "rate window recovers");
    Expect(IsValidEventName("ping.state") && !IsValidEventName("bad name"), "event names are bounded identifiers");

    const std::string payload = BuildPluginEventPayload("ParityProbe", "ping", "a \"quoted\"\nline", "Veronica",
        kVeronica);
    Expect(payload == "{\"schema\":\"dialectic.plugin_event.v1\",\"bridge\":\"ParityProbe\",\"name\":\"ping\","
                      "\"data\":\"a \\\"quoted\\\"\\nline\",\"actor\":\"Veronica\",\"actor_refid\":\"0x000E32A9\"}",
        "plugin event envelope is escaped and actor-bound");
    Expect(BuildPluginEventPayload("ParityProbe", "state", "", "", 0).find("actor") == std::string::npos,
        "global plugin event omits actor fields");
}

} // namespace

int main() {
    TestCommandParsing();
    TestOwnershipAndDispatchEnvelope();
    TestBoundsTimeoutAndCancellation();
    TestRuntimeOwnersAndOutcomes();
    TestPluginEventEnvelope();
    if (g_failures != 0) {
        std::cerr << g_failures << " external command registry test(s) failed\n";
        return 1;
    }
    std::cout << "External command registry tests passed\n";
    return 0;
}
