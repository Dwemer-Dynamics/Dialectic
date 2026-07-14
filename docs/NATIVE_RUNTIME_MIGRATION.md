# Dialectic Native Runtime Migration

Baseline: `b63064e` (`native-runtime-baseline-2026-07-11`)

Pinned xNVSE SDK revision: `03684a6bdf7ca583ba851e8efdb762178654d828`

## Objective

Make the DLL authoritative for live Fallout state. Background workers consume immutable snapshots and submit typed commands to a game-thread dispatcher. NVSE scripts remain only where xNVSE/JIP exposes no safe native equivalent.

## Invariants

- Fallout objects are read or mutated only on the game thread.
- Every request, response, action, and playback item carries a runtime generation.
- Loading a save, returning to the main menu, or changing playthrough invalidates stale work.
- Cell changes invalidate actor, path, targeting, rechat, and action state.
- Native and bridge results run in comparison mode before a bridge is removed.
- Bridge writer, reader, startup cleanup, and deployed script are removed together.
- No permanent user-facing native/legacy toggle is introduced.

## Migration Gates

1. Native implementation compiles and loads without changing existing behavior.
2. Comparison logs show equivalent state through menu, Pip-Boy, combat, cell, save-load, and interruption tests.
3. Native state becomes authoritative while the bridge remains comparison-only.
4. The bridge is removed after an in-game soak test.
5. Queue, cancellation, and frame-time counters remain bounded.

## Bridge Inventory

| Domain | Current bridges/scripts | Disposition |
| --- | --- | --- |
| Runtime lifecycle | `GameStateTick.txt`, `dialectic_game_state.tmp` | Replace with native menu, load, combat, cell, and worldspace snapshot. |
| Actor registry | `SpatialScanTick.txt`, `dialectic_actor_positions.tmp`, `dialectic_activity_status.tmp` | Replace with native loaded-actor registry and immutable actor snapshots. |
| Targeting | `dialectic_textinput_target.tmp`, crosshair/script caches | Replace with native crosshair and nearest eligible actor resolution. |
| Actions | `ActionCommandTick.txt`, `dialectic_action_request.tmp`, `dialectic_action_status.txt` | Replace transport with typed game-thread commands. Keep GECK package forms where required. |
| Attack | `AttackStateTick.txt`, `AttackCleanupTick.txt`, `dialectic_attack_state.tmp`, `dialectic_attack_cleanup.tmp` | Replace state transport and cleanup ownership with native action state. |
| Follow/move/pickup | `FollowStateTick.txt`, `MoveStateTick.txt`, pickup marker/state files | Replace with native action state; retain a narrow script adapter only for unsupported JIP calls. |
| Menu commands | mode, model, dynamic-profile, halt, and text-input open/select files | Replace transport with registered commands or native UI events; keep MCM configuration. |
| Dialogue capture | dialogue topic, prompt, menu-choice, speaker, and capture files | Replace with native dialogue hooks where stable; otherwise consolidate behind one typed callback adapter. |
| Dialogue suppression | dialogue guard ref/mod/local/status files and guard scripts | Replace with scoped native voice/dialogue guard with deterministic restoration. |
| Subtitles | `dialectic_subtitle.txt` and subtitle script | Replace with native HUD subtitle submission if stable. |
| Facing | facing speaker/target/yaw files and `FaceTargetTick.txt` | Replace with native actor target/yaw command on the game thread. |
| Lipsync | lipsync ref/command/status files and `LipSyncCommandTick.txt` | Keep direct FaceGen pump; remove file command transport after native runtime validation. |
| Spatial paths | script path observations consumed by `SpatialPathProviderFNV` | Replace with native loaded-cell navmesh graph; allow one narrow query adapter if native traversal is unsafe. |
| Nearby items/POIs | nearby items, furniture, and POI snapshot files | Migrate to native loaded-reference scans after actor registry. |
| Active quests | active quest direct and quest snapshot files | Migrate selected quest and objective reads after lifecycle hooks. |
| Trade | trade session, pre/post player, pre/post counterparty, done, and status files | Replace with native menu lifecycle plus inventory/caps snapshots. |
| RPG events | RPG event file plus lockpick/aid handlers | Prefer xNVSE events; retain narrow handlers where the event manager is the authoritative source. |
| Notifications | notification file and tick script | Replace with native HUD notification call if stable. |

## Planned Components

- `XNVSEAdapter`: official SDK interfaces and lifecycle messages.
- `FNVRuntime`: game-thread ownership and native state capture.
- `RuntimeGeneration`: stale-work invalidation.
- `RuntimeEventBus`: typed lifecycle, menu, actor, and dialogue events.
- `RuntimeSnapshot`: immutable player/world/menu/actor state.
- `GameThreadDispatcher`: bounded typed command queue.
- `TaskManager`: bounded worker pool, priorities, coalescing, cancellation, and health counters.
- `NativeActorRegistry`: loaded actor ownership, freshness, and eligibility.

## Implementation Order

1. SDK adapter and lifecycle messages.
2. Runtime generation, event bus, snapshots, and game-thread dispatcher.
3. Native menu/load/cell/combat state with bridge comparison.
4. Central task manager and detached-thread migration.
5. Native actor registry and targeting.
6. Action transport and cleanup.
7. Spatial snapshots and pathing.
8. Subtitles, facing, dialogue guard, and lipsync transport.
9. Inventory and trade.
10. Remove superseded scripts and bridge cleanup code.

## Current Implementation Status

Native foundation deployed for validation on 2026-07-11:

- `main.cpp` uses the pinned official xNVSE interfaces; the hand-written ABI and `NVSEInterfaces.h` are removed.
- Fallout object and form access is confined to `XNVSEAdapter`/`FNVRuntime`. `SpeakManager`, `Misc`, and the registered dialogue-prompt command consume copied state or typed adapter results rather than raw engine pointers.
- xNVSE lifecycle and main-loop messages drive `FNVRuntime`.
- `GameLoop::Update()` is driven directly by the xNVSE `MainGameLoop` message. The former independent 60 FPS update thread is removed, preventing game-facing work from running concurrently with the engine.
- Menu, Pip-Boy, dialogue, barter, container, loading, combat, cell, worldspace, player, and crosshair state are captured natively.
- Native state is authoritative in `GameLoop`; `dialectic_game_state.tmp` is comparison/fallback only until the in-game matrix passes.
- The loaded-cell actor registry supplies native positions and hard cell/generation boundaries to `ActorPositionResolverFNV`.
- The native actor registry enumerates the player current cell on the game thread. Direct `ActorProcessManager` high/middle-high list traversal was removed after a stale actor entry caused a main-thread access violation during load. Adjacent loaded exterior-cell coverage remains limited until a lifecycle-fed registry can provide it without retaining or dereferencing stale engine pointers.
- Actor snapshots now copy name, BaseID, race, sex, voice FormID, cell/worldspace, position, distance, health/AP, level, combat target, teammate state, current package, equipped weapon, weapon-out, and movement flags.
- Actor snapshots include condition-aware equipped-item summaries. Prompt-critical profile refreshes consume native actor and inventory snapshots without the legacy 600-900 ms script wait when the actor is loaded.
- Prompt-critical actor metadata uploads are queued through `TaskManager`; failed profile, equipment, or inventory HTTP requests never block the Fallout main-loop callback.
- A central loaded-reference snapshot captures non-actor references and native door destinations on the game thread. Nearby item and POI modules consume copied references instead of maintaining their own raw engine scans.
- The selected quest and displayed, incomplete objectives are captured natively from the player once per second. `QuestJournalFNV` uses this snapshot before the two five-second quest bridges.
- Crosshair and nearest-NPC fallback targeting use the same native registry; script-fed targeting is fallback-only when no fresh native snapshot exists.
- Interior cell and exterior worldspace identity form one shared native scene boundary used by targeting, autoactivation, rechat position lists, activity status, and action validation. This prevents persistent loaded actors from crossing TTW worldspace or interior boundaries.
- A fresh empty native actor/reference snapshot is authoritative. Cached bridge actors, items, doors, and POIs are not revived merely because the current scene has no results; the nearby-item bridge contributes only the unsupported held-physics-item signal or metadata for a matching native reference.
- World cell/worldspace identity and player position are native-authoritative; weather and calendar fields remain script-adapted.
- Player name, FormID, position, pitch, and yaw are captured in the same immutable native game-state snapshot; narrator sky targeting and player identity no longer maintain a separate raw singleton reader.
- Full map-marker collection is incremental and frame-budgeted (32 cells and 1,500 references per update slice by default). Plugin-file parsing and HTTP upload remain worker tasks, avoiding a complete DataHandler cell-array walk in one frame.
- Trade open/close lifecycle uses native barter/container menu state.
- Trade sessions now capture native player/counterparty inventories before opening and after closing, including BaseID, name, count, type, equipped state, caps, and condition; bridge snapshots are fallback-only when native capture fails.
- Vanilla-dialogue suppression is native-first: guarded actor bases retain their original resolved voice type, use the silent voice while AI playback is active, and restore deterministically on queue completion, load, or cell invalidation. The script guard remains failure fallback pending game validation.
- Passive subtitles update the injected HUD tile directly on the game thread. `dialectic_subtitle.txt` is used only when the HUD tile is unavailable during UI reconstruction.
- Facing uses one cached in-memory xNVSE function callback on the game thread; facing files are failure fallback only.
- Direct FaceGen remains the primary lipsync path, but all process/FaceGen access now lives in `XNVSEAdapter`. Playback identifies the current speaker by FormID rather than crosshair pointer. Its MFG fallback uses cached in-memory xNVSE callbacks before falling back to command files.
- Attack, halt, walk-speed, sheathe, stop-walk, end-conversation, follower, follow, move, wait, seat, travel, stop-following, caps transfer, item transfer, consume, pickup, inventory opening, and barter execute through typed game-thread commands and return results in memory. GECK/JIP-only operations are invoked through cached in-memory xNVSE function adapters; the file dispatcher is failure fallback pending runtime validation.
- Native package actions are generation-bound. Load and cell invalidation restores loaded actors immediately and records unloaded actors for deterministic cleanup when they next enter the native actor registry.
- Attack state records teammate/aggression/confidence/assistance values for both combatants and restores them after combat, cancellation, load, or cell invalidation. Pickup owns a generation-bound move-then-transfer state instead of marker/status files.
- Trade/container menus open natively. Pre/post snapshots use the actual merchant container when available and retain condition-aware item identity.
- Pairwise LOS refinement is queued on the game thread and cached; spatial evaluation no longer requires the broad actor-position file for fresh LOS.
- Attached current-cell navmesh geometry is copied on the game thread into immutable snapshots. Triangle adjacency and cross-mesh geometric portal stitching are built on a bounded worker task and published only for the matching runtime generation.
- Native path queries use nearest-triangle snapping and bounded Dijkstra. Door portal metadata is best-effort and combined with the native reference door state. Disconnected interiors reject; incomplete exterior graphs and unavailable snaps retain the lenient script/air-distance fallback.
- Spatial actor prewarming is capped at 32 nearby live actors and evaluates one actor per periodic game-loop tick. It warms the existing LOS/path caches rather than running a second all-scene spatial system.
- Pip-Boy notifications use the native `QueueUIMessage` call first and retain `NotificationTick.txt` only as delivery fallback.
- Validated actions enter the generation-aware game-thread dispatcher and are rejected when their actor participants are no longer in the current native scene.
- Load, new-game, exit, and cell events invalidate dialogue, rechat, actions, recording, dispatcher commands, and generation-bound tasks.
- Player input advances runtime generation and invalidates stale turn work without discarding the current scene snapshot.
- Mode, LLM-model, and dynamic-profile menus open through cached in-memory xNVSE function calls. Their open files remain failure/menu-blocked fallback pending validation.
- Radiant, dialogue-menu NPC, and player-choice handlers submit captured dialogue through the registered `DialecticCaptureDialogue` command into a bounded in-memory queue. Canonical bridge output remains comparison-only and logs transport matches under `[NATIVE_COMPARE]`.
- Periodic uploads and synchronization jobs use the bounded, coalescing `TaskManager`; uncontrolled detached worker threads are removed.
- HTTP streaming, telemetry, TTS preparation, STT upload, navmesh computation, and periodic synchronization use the shared eight-worker `TaskManager`; the former HTTP and TTS worker pools are removed. Interactive, audio, gameplay, compute, and background lanes enforce lane and per-type concurrency limits so long imports cannot starve a player turn.
- Task handles and scopes support cancellation by ID, type, key, actor FormID, turn, utterance, and runtime generation. Queue-aware deadlines expire pending interactive/audio work before it starts, active cancellation can interrupt WinHTTP immediately, and task completions use reserved bounded dispatcher capacity so ordinary game-thread commands cannot discard cleanup. World-data scans, actor-snapshot waits, imports, retries, and multipart uploads poll the shared cancellation token.
- Voice capture keeps two dedicated WinMM device threads because blocking microphone ownership is not general worker-pool work. They never access Fallout objects or perform HTTP, expose heartbeat/lifecycle health, dispatch callbacks to the game thread, and are synchronously joined during shutdown. STT network work runs in `TaskManager`.
- Streamed response envelopes are consumed from `ResponseQueueFNV` by the native game-frame pump. The former response poll thread is removed, so dialogue/action queue transitions occur on the same authoritative frame thread.
- Response streams retain both their HTTP response generation and the native runtime generation active when the stream began. Queue dispatch, delayed actions, TTS preparation/playback, and final action execution reject stale runtime generations independently of HTTP cancellation.
- Runtime health is logged every ten seconds under `[NATIVE_RUNTIME]` and `[TASK_HEALTH]`, including pool uptime, per-type queue/active counts, cancellation/timeout/error/rejection totals, duration maxima, active worker ages, and microphone-service heartbeat/lifecycle counters. Full-queue warnings are rate limited, and the file logger serializes worker and game-thread writes.
- Dual-read domains report rate-limited `[NATIVE_COMPARE]` details and cumulative ten-second summaries. Current coverage includes game state, actor registry/positions, dialogue capture transport, nearby items, POIs, and player-side trade snapshots; native counterparty trade integrity uses conservation checks because the old bridge has no post-counterparty writer.
- The Win32 deployment target is only `C:\Modlists\Fallout TTW\mods\Dialectic_dev`.

Not yet eligible for bridge removal:

- Actor identity, condition, equipment, and inventory are native-first; the metadata bridge remains only when an actor is absent from the current native registry pending validation of unloaded/persistent actors.
- Actor package FormID is captured natively, but restrained state and higher-level scene/activity classification still use the narrow activity-status adapter where the pinned SDK does not provide a proven stable native accessor.
- Pairwise LOS and door state execute in memory through cached xNVSE functions. Native current-cell navmesh path distance is preferred; the narrow script query remains only as a fallback until the native path matrix passes.
- Native action callbacks, trade menus, notifications, LOS, dialogue suppression, subtitles, and presentation paths require fresh runtime validation before their fallback files are removed.
- Native navmesh coverage is deliberately limited to the attached current cell. Adjacent exterior-cell portals are not traversed, and the fallback remains lenient when that limitation prevents a reliable route.
- Native dialogue suppression and native subtitle delivery require fresh runtime validation before their fallback files and polling scripts can be removed.
- Native trade snapshots require barter and companion-trade validation before the six legacy trade files and `TradeSessionTick.txt` can be removed.
- `GameStateTick.txt` and `SpatialScanTick.txt` must remain deployed until comparison logs pass multiple sessions.

## Required In-Game Matrix

- New game, normal load, older save load, TTW train worldspace transition, death reload, and exit to main menu.
- Pause menu, Pip-Boy, dialogue, barter, container, and loading screens during single and multi-line playback.
- Player interruption during NPC speech and during rechat.
- Interior/exterior cell transitions with companions and distant cached actors.
- Follow, stop following, move, attack, pickup, give item, open inventory, barter, and end conversation.
- Vanilla dialogue suppression and restoration for ambient speakers.
- Subtitles, facing, lipsync, and 3D audio while player and speaker move.
- Trade item/caps deltas and durability-preserving transfers.
