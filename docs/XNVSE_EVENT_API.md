# Public xNVSE Event API

Dialectic registers actor-bound xNVSE events so another mod can request supported
dialogue and follower behavior without depending on Dialectic's internal scripts.
Dispatch every event on the exact actor reference that should receive the request.

```geck
VeronicaREF.DispatchEventAlt "DialecticSpeakExact", "The road is quieter than usual."
VeronicaREF.DispatchEventAlt "DialecticComment"
VeronicaREF.DispatchEventAlt "DialecticReact", "React with alarm to the nearby explosion."
VeronicaREF.DispatchEventAlt "DialecticAsk", "What do you make of this place?"
VeronicaREF.DispatchEventAlt "DialecticOpenPrompt"
VeronicaREF.DispatchEventAlt "DialecticRecruit"
VeronicaREF.DispatchEventAlt "DialecticDismiss"
VeronicaREF.DispatchEventAlt "DialecticWait"
VeronicaREF.DispatchEventAlt "DialecticResume"
```

## Events

| Event | String argument | Behavior |
| --- | --- | --- |
| `DialecticSpeakExact` | Required | Speaks the supplied text through the actor's TTS voice without an LLM turn or conversation-memory entry. |
| `DialecticComment` | None | Requests one brief contextual, in-character observation. Generated actions are disabled. |
| `DialecticReact` | Required | Treats the string as scene direction and requests one brief spoken reaction. Generated actions are disabled. |
| `DialecticAsk` | Required | Sends the string through the normal player-input conversation path for this exact actor. |
| `DialecticOpenPrompt` | None | Opens Dialectic's text prompt with this exact actor retained as the submit target. |
| `DialecticRecruit` | None | Deterministically recruits the actor and starts following behavior. |
| `DialecticDismiss` | None | Deterministically dismisses a current teammate. |
| `DialecticWait` | None | Makes a current teammate wait using Dialectic's persistent Wait Here lifecycle. |
| `DialecticResume` | None | Resumes a current teammate only when Dialectic is tracking that actor in Wait Here state. |

## Contract and safety

- The calling reference is authoritative. Dialectic never substitutes the
  crosshair target or nearest NPC.
- String arguments must contain 1 to 1000 bytes after surrounding whitespace is
  removed.
- The actor must be alive, loaded, eligible, and in the player's current scene.
- Contextual comment and reaction requests honor menu, dialogue, combat,
  activity, and busy-pipeline gates.
- `DialecticAsk` and `DialecticOpenPrompt` refuse to replace a conversation
  currently owned by another actor.
- Follower dismissal, waiting, and resuming require a current teammate.
- Events are asynchronous. Acceptance and rejection details are written to the
  Dialectic plugin log; no callback result is dispatched in version 1.
- If another plugin already registered one of these names, Dialectic logs the
  collision and does not attach its handler to that event.

The events are intentionally a fixed allowlist. They do not expose arbitrary
server endpoints, action names, JSON payloads, or remote actor selection.

## Plugin extension API

These commands give xNVSE addons extension points comparable to CHIM's. Every
command below returns immediately; `1` means accepted and `0` means rejected,
with the reason in the Dialectic plugin log. Requests to the server and
in-game speech remain asynchronous.

The plugin version reported to `GetPluginVersion "Dialectic"` does not change
with this API, so it cannot tell a capable DLL from an older one. Before
compiling a script that calls these commands, check each command with xNVSE's
`GetCommandOpcode` (xNVSE 6.1 beta 6 and later), which returns `0` for a name
no plugin registered:

```geck
if GetCommandOpcode "DialecticRegisterExternalBridge" == 0
    return
endif
call (CompileScript "MyAddon/Register.txt")
```

Keep the check in a script that calls only xNVSE commands, and call the
Dialectic commands only in scripts compiled after it passes. A script that
names an unknown command fails to compile as a whole.

| Command | Calling reference | Returns |
| --- | --- | --- |
| `DialecticRegisterExternalBridge "Name"` | None | `1` when the calling plugin owns the bridge name (repeat calls from the same plugin also return `1`); `0` for an invalid name, a name owned by another plugin, or a full table (32 bridges). |
| `actor.DialecticCompleteExternalCommand "Name", requestId, succeeded, "result"` | Required; must be the request's actor | `1` when the result was handed to the server `funcret` path; `0` for an unknown, expired, cancelled, wrong-bridge or wrong-actor request. |
| `DialecticIsExternalCommandPending requestId` | None | `1` while the request can still be completed. |
| `[actor.]DialecticSendPluginEvent "Name", "event", "data"` | Optional | `1` when one `pluginevent` was queued for the server; `0` if the bridge is unregistered, the event name or data is invalid, the actor is not in the current scene, Dialectic is off, or the bridge exceeded 20 events in 10 seconds. |
| `actor.DialecticIsActorTalking` | Required | `1` while Dialectic speech plays for this actor. |
| `actor.DialecticIsActorAvailable` | Required | `1` when this actor passes the same exact scene and eligibility gate as the public events. Busy-pipeline and menu gates are checked only when a request is made. |
| `DialecticGetInteractionState` | None | `0` off, `1` on, `2` updating, `3` connection failed. |
| `DialecticStopAllDialogue` | None | `1` after stopping Dialectic speech and pending replies. Actor actions and conversation history are unchanged. |

Bridge names are 1 to 32 ASCII letters or digits and start with a letter.
They are case-insensitive. Ownership is the FNV load-order index of the calling
script's form ID (its top byte; FNV has no light-plugin sub-slots). Scripts
compiled at runtime (for example JIP LN script-runner files and `CompileScript`
UDFs) all share index `0xFF`, so two runtime-compiled addons that pick the same
bridge name both register successfully and both receive its commands. Event
names are 1 to 64 letters, digits, `_`, `.` or `-`. Strings are limited to
1000 bytes.

Bridge ownership only prevents accidental name collisions between trusted,
installed mods. It is not a sandbox or an authorization check: every xNVSE
script runs with full game access, completion and plugin-event calls check the
bridge name, request ID and actor but not the calling plugin, and any loaded
script can call them. Only Dialectic can dispatch `DialecticExternalCommand`
because the event is registered without `kFlag_AllowScriptDispatch`.

### Server actions: `ExtCmd<Bridge>_<Action>`

DialecticServer can emit a `rolecommand` line whose `command_name` uses CHIM's
form, for example `ExtCmdParityProbe_Ping`, with the parameter string as the
first `command_args` entry. The bridge is the text between `ExtCmd` and the
first `_`. The line must also carry `speaker_refid` (or `speaker_formid`).
Dialectic never resolves these commands by speaker name, crosshair or distance.

1. The client validates the command, a registered bridge, the exact speaker
   ref and the pending limit (16), then assigns a positive request ID.
   The same command, parameter and actor is ignored while pending and for 1.5
   seconds after it is first received, because a response can deliver it twice.
2. On the game thread, the actor must still be loaded, alive and in the current
   scene. Dialectic then dispatches `DialecticExternalCommand` on that exact
   reference with arguments `bridge`, `command`, `parameter`, `requestId`,
   `actor`. Only Dialectic can dispatch this event.
3. The addon reports the outcome with `DialecticCompleteExternalCommand`, in
   the same frame or later. Acceptance and dispatch never report completion.

```geck
SetEventHandlerAlt "DialecticExternalCommand" MyHandler 1::"MyBridge"
; MyHandler: begin function {sBridge, sCommand, sParameter, iRequestId, rActor}
rActor.DialecticCompleteExternalCommand "MyBridge" iRequestId 1 "Done."
```

The server receives one `funcret` event with schema
`dialectic.action_result.v1`: `action` is the full command, `target` is the
parameter, `result` is the addon text (or `<command> completed.`/`failed.`), and
`status`, `bridge` and `request_id` identify the outcome. `request_id` is `0`
when the command was rejected before acceptance. Dialectic reports
`status: "failed"` with `<command> failed because <reason>.` when the bridge is
not registered, the speaker ref is missing, the actor left the scene, no script
handler received the event, the queue is full, or 30 seconds pass without
completion. Requests are dropped without a server result when Dialectic halts
AI actions, a save is loaded, a new game starts or the runtime generation
changes; later completion calls return `0`.

### Plugin events

`DialecticSendPluginEvent` sends event type `pluginevent` with this payload:

```json
{"schema":"dialectic.plugin_event.v1","bridge":"ParityProbe","name":"ping","data":"text","actor":"Veronica","actor_refid":"0x000E32A9"}
```

`actor` and `actor_refid` are present only when the command is called on an
actor. In an object or effect script, a call without an explicit reference uses
that script's own reference. The server decides how to store or use the event;
sending it does not request dialogue.

### CHIM equivalents

This table covers every native in CHIM's `AIAgentFunctions.psc` and the
`ExtCmd`/`IntCmd`/`WebCmd` paths in `Commands.cpp`. "Not provided" means
Dialectic has no addon-facing equivalent. Commands described as internal are
registered for Dialectic's own scripts and MCM; they are not a supported addon
API and may change.

| CHIM | Dialectic | Parity |
| --- | --- | --- |
| `ExtCmd<Bridge>_<Action>` to `<Bridge>.DispatchExternalCommand(npc, command, parameter)` | `DialecticExternalCommand` on the exact actor, after `DialecticRegisterExternalBridge` | Equivalent; routed by actor ref instead of NPC name |
| `ExtCmd` fallback to `AIAgentAIMind.SendExternalEvent` | Not provided; unregistered bridges report failure | Gap |
| `IntCmd` (`AIAgentAIMind.SendInternalEvent`) | Not applicable | Papyrus-internal dispatch |
| `WebCmd` (immediate `funcret` echo) | Not provided | Server plugins handle it without a game round trip |
| `commandEnded` / `commandEndedForActor` | `actor.DialecticCompleteExternalCommand` | Equivalent; adds request ID and success flag. Dialectic reports `timed_out` after 30 seconds |
| `sendMessageToActor` | `DialecticAsk` event | Partial: player-input path only; no message type |
| `sendMessage`, `requestMessage`, `sendRequest` | Not provided | Deliberate: every request is bound to an actor |
| `requestMessageForActor` / `requestMessageForEligibleActor` | `DialecticReact` and `DialecticComment` events | Partial: always gated; no message type |
| `logMessage` / `logMessageForActor` | `DialecticSendPluginEvent` | Partial: namespaced by bridge, no arbitrary event type, 20 events per 10 seconds |
| `PostGameData` | `DialecticSendPluginEvent` | Partial: one bounded string (1000 bytes) per event, not arbitrary JSON |
| `isActorTalking` | `actor.DialecticIsActorTalking` | Equivalent for Dialectic speech only |
| `getChimInteractionState` | `DialecticGetInteractionState` | Equivalent |
| `setChimInteractionEnabled` | `DialecticToggleInteraction` (internal) | Partial: toggles instead of setting a value, and does nothing while the state is `2` (updating). There is no deterministic setter |
| `stopAllDialogue` | `DialecticStopAllDialogue` | Equivalent |
| `setLocked`, `setAnimationBusy` | Not provided | Gap: no per-actor talk lock or busy flag. `actor.DialecticIsActorAvailable` only reads the request gate |
| `setDrivenByAI`, `setDrivenByAIA`, `addBasicProfile`, `setAIKeyWord` | `DialecticRecruit` event | Partial: Dialectic registers AI agents itself on activation and exposes no command to add one. Recruiting makes the actor a teammate; it does not add a profile |
| `removeAgentByName` | `DialecticDismiss` event | Partial: dismisses a teammate by ref; it does not remove the actor from Dialectic's agent registry |
| `getClosestAgent`, `getAgentByName`, `findAllNearbyAgents`, `findAllAgents`, `findAllNearbyNonAgents`, `findAllNearbyActors`, `findAllAgentsFormId`, `getHerikaFormId` | Not provided | Gap: the agent registry is internal and has no read command. Use the game's own actor scans and `DialecticIsActorAvailable` |
| `isGameFocused` | Not provided | Gap |
| `scanActorsAroundOffline`, `updateRemoteCombatSnapshot` | Not provided | Dialectic sends nearby-actor context itself; addons cannot request a scan |
| `updateRemoteInventory` | `DialecticMarkPlayerInventoryDirty` (internal) | Partial: marks the player inventory for upload. Dialectic uploads agent inventory and equipment itself; addons cannot request it for an actor |
| `sendLocationFast`, `sendFactionFast`, `sendNPCFast` | `DialecticSyncWorldData` (internal) | Partial: full faction and location sync, not one record |
| `sendAllVoices` | `DialecticSendAllVoiceSamples` (internal) | Equivalent for configured voice samples |
| `setConf`, `get_conf_i` | `DialecticSendSetConf` (internal); `DialecticGetConfigInt` and related commands read local INI settings | Partial; not an addon API |
| `SayTo` | Not provided | FNV scripts can call the game's own `Say`/`SayTo` |
| `recordSoundEx`, `stopRecording`, open-mic natives, `getCurrentRecordingDeviceName` | Dialectic hotkeys and `DialecticGetCurrentRecordingDevice` (internal) | Not an addon API |
| `setNewActionMode`, `getPlayerBountyForGuard`, `requestMoveInventoryItemConfirmation`, `requestArrestConfirmation`, `isUsingFurniture`, `isInContainer`, `loadReference`, location-marker and door helpers, `jsonGet*` helpers, music scenes, `removeFromRenamedNPCList`, `hardResetExpression` | Not provided | CHIM-internal Papyrus helpers. FNV scripts use xNVSE and JIP LN commands for similar game queries |
| MCM snapshot and agent publishing, Prisma UI panels, `shotAndUpload`, `startSoulgazeCapture`, `isGameVR`, `startPlayerMenuDialogueTTS`, test natives | Not applicable | Skyrim or CHIM-internal UI, capture and debugging |

Engine differences: Fallout: New Vegas has no Papyrus, so addons use xNVSE
script commands and events instead of global native functions. Actors are
passed as references, never NPC names. Every runtime-compiled script shares
load-order index `0xFF` for bridge ownership. Dialectic runs on the 32-bit
game, has no VR build, and has no Prisma UI.

The API does not accept URLs, server endpoints or arbitrary event types, and
it does not replace Dialectic's bundled scripts. See
[`docs/examples/ParityProbe`](examples/ParityProbe/README.md) for a small
addon.
