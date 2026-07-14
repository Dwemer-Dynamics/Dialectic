# Dialectic Native Runtime Validation

Use this matrix before deleting any comparison bridge. Restart Fallout after
each DLL deployment; hot reload cannot validate native lifecycle ownership.

Before deployment, run:

```powershell
powershell -ExecutionPolicy Bypass -File Plugin\tests\verify-native-runtime.ps1
```

This guards the SDK/runtime ownership boundary and reports how many comparison
scripts remain. It does not replace the in-game matrix below.

## Startup Gate

1. Start TTW through the configured MO2 profile and load a normal save.
2. Confirm the log contains:
   - `[NATIVE_RUNTIME] xNVSE main-game-loop pump authoritative`
   - periodic `[NATIVE_RUNTIME]` health lines
   - no legacy update-thread or response-poll-thread startup entries
   - periodic `[NATIVE_COMPARE] summary` lines with per-domain `m` (match),
     `x` (mismatch), and `u` (bridge unavailable) counters
3. Remain in the same scene for five minutes and confirm dispatcher/task queue
   counts stay bounded.

## Lifecycle And Menus

1. Play a multi-line AI response.
2. Open and close Escape, Pip-Boy, container, barter, and dialogue menus.
3. Confirm current audio, subtitles, lipsync, and queued lines pause and resume.
4. Load another save and confirm no prior dialogue, rechat, action, or player TTS
   survives the generation change.
5. Repeat with a TTW train worldspace transition and a death reload.

## Actors And Targeting

1. Open the chatbox while looking at an NPC, then while looking away.
2. Confirm crosshair and nearest fallback select the expected loaded actor.
3. Move cells and confirm actors from the previous cell cannot speak or rechat.
4. Confirm dead, disabled, unloaded, and disallowed creature actors are absent.
5. Confirm profile identity, race, sex, voice, inventory, and equipment populate.

## Dialogue And Presentation

1. Trigger radiant dialogue, an NPC dialogue-menu line, and two player choices.
2. Confirm each reaches the event log once.
3. Confirm `[NATIVE_COMPARE] dialogue transports matched` appears while the
   comparison bridge remains enabled.
4. Play normal and rechat AI lines from actors not under the crosshair.
5. Confirm subtitles, facing, vanilla-dialogue suppression/restoration, and
   lipsync apply to the actual speaker.
6. Confirm `[NATIVE_LIPSYNC] FaceGen applied` and no persistent muted voice.

## Actions And Trade

Validate follow, stop following, move, travel, attack, pickup, give item, open
inventory, barter, and end conversation. Interrupt each with a cell change or
save load. Packages, factions, restraint, combat state, and teammate state must
restore. For barter and companion trade, confirm one post-close item/caps delta
event with condition-preserving item identity.

## Spatial And Performance

1. Test interior doors, exterior actors, blocked LOS, and path-obstructed actors.
   Confirm `[NATIVE_NAV] captured` and `[NATIVE_NAV] graph ready` report the current
   cell with plausible mesh/node/edge counts before evaluating path behavior.
2. Confirm only current-scene audible actors can rechat.
3. Confirm map-marker capture logs incremental progress/completion without a
   visible frame hitch.
4. Run at least 30 minutes with repeated rechat and cell changes. Frame rate,
   response queues, dispatcher pending count, and worker pending count must not
   trend upward.

## Removal Rule

For each passing domain, remove its writer script, temporary files, DLL reader,
startup cleanup entries, and deployment references in one change. Do not remove
the narrow spatial path-query adapter unless native FNV navmesh traversal has
separately passed the same matrix.
