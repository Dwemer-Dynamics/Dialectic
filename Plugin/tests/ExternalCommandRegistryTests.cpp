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
    TestPluginEventEnvelope();
    if (g_failures != 0) {
        std::cerr << g_failures << " external command registry test(s) failed\n";
        return 1;
    }
    std::cout << "External command registry tests passed\n";
    return 0;
}
