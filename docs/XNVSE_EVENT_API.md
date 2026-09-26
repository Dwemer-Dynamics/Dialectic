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

The API is intentionally a fixed allowlist. It does not expose arbitrary server
endpoints, action names, JSON payloads, or remote actor selection.
