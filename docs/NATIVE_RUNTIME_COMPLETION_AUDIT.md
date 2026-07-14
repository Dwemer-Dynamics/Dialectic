# Native Runtime Completion Audit

Audit date: 2026-07-12

This document tracks the requirements in the original migration plan. "Built"
means the current x86 DLL contains the implementation. It does not satisfy an
in-game acceptance gate.

| Phase | Requirement | Current evidence | Status |
| --- | --- | --- | --- |
| 0 | Reconcile `unstable` branches | `Dialectic/unstable` matches `origin/unstable` at baseline `b63064e`; migration changes are intentionally uncommitted. `DialecticServer/unstable` is no longer behind its upstream and contains two local commits. | Built/prepared |
| 0 | Working checkpoint | Tag `native-runtime-baseline-2026-07-11` points to `b63064e`. | Complete |
| 0 | Migration and bridge inventory | `NATIVE_RUNTIME_MIGRATION.md`, `NATIVE_RUNTIME_BRIDGE_INVENTORY.md`, and this audit exist. The exact ownership reporter currently finds 90 bridge files and the static verifier reports 59 deployed comparison scripts. | Complete for implementation; removal pending |
| 1 | Pinned full xNVSE SDK | `Plugin/vendor/xnvse-sdk` is pinned to `03684a6bdf7ca583ba851e8efdb762178654d828`; handwritten `NVSEInterfaces.h` is deleted. | Built; load test pending |
| 1 | Runtime ownership components | `FNVRuntime`, `XNVSEAdapter`, `RuntimeGeneration`, `RuntimeEventBus`, `RuntimeSnapshot`, and `GameThreadDispatcher` compile into the DLL. | Built; load test pending |
| 2 | Native lifecycle and menus | Native menu, Pip-Boy, dialogue, barter, container, loading, combat, cell, worldspace, save/load, new-game, and exit state are authoritative when fresh. | Built; matrix pending |
| 2 | Remove game-state bridge | `dialectic_game_state.tmp` remains comparison/fallback by migration rule. | Not yet eligible |
| 3 | Native actor registry | Current-cell actors feed immutable snapshots. Direct high/middle-high `ActorProcessManager` traversal was removed after a stale entry caused a main-thread access violation. Current interior cell/exterior worldspace is a shared hard boundary; adjacent exterior-cell parity awaits a lifecycle-fed safe registry. | Built with adjacent-cell limitation; matrix pending |
| 3 | Required actor fields | Identity, race, sex, voice, cell/worldspace, transform, dead/deleted/loaded, combat/hostility/teammate, package, health/AP, equipment, LOS refinement, generation, and timestamps are available. Restrained/scene classification remains a narrow adapter. | Built with documented adapter |
| 3 | Remove broad actor bridge | `dialectic_actor_positions.tmp` remains comparison/fallback. | Not yet eligible |
| 4 | Central task system | Eight bounded workers provide five workload lanes, priority with aging, lane/per-type concurrency caps, cancellation by ID/type/key/actor/turn/utterance/generation, pending replacement/rejection policies, queue-aware and active deadlines, interruptible WinHTTP, per-type/worker health, reserved game-thread completion capacity, and joined/restartable shutdown. Three native test executables cover mixed-lane stress, scoped cancellation, queue saturation, exact-once completion, deadlines, exceptions, generation drops, restart, and real dispatcher-thread delivery. | Built and automated tests passed; in-game soak pending |
| 4 | Remove detached work | Static verification rejects `.detach()` and subsystem-owned threads outside `TaskManager` and `VoiceRecorder`. HTTP, TTS preparation, STT upload, navmesh work, imports, and telemetry use the central pool. The two joined `VoiceRecorder` workers exclusively own blocking WinMM device capture and expose health counters. | Static gate passed |

Task-manager runtime acceptance is checked with `Plugin/tests/report-task-health.ps1 -RequireDialogueExercise` after an in-game dialogue/STT/rechat soak. The report requires final five-lane startup, periodic pool/type/device health, observed HTTP-stream and audio-preparation tasks, and no queue-full, deadline, or worker-exception markers.
| 5 | Native action dispatch | Typed generation-bound commands cover the maintained action set. Speakers and targets must remain in the current native scene; package/faction/combat state has cleanup ownership. | Built; action matrix pending |
| 5 | Remove action files | Action/status files remain automatic failure fallback. | Not yet eligible |
| 6 | Spatial runtime | Native actor/reference snapshots, hard scene boundaries, cached LOS, native doors, a bounded one-actor-per-tick spatial prewarm queue, incremental map-marker capture, and generation invalidation are present. | Built; spatial matrix pending |
| 6 | Navmesh path distance | The game thread copies the attached current cell into immutable POD data. A worker builds triangle-centroid adjacency, stitches matching geometry across meshes, and serves bounded Dijkstra queries. Interior disconnected paths reject; incomplete exterior paths fall back leniently. Script path results remain fallback pending the in-game matrix. | Built; runtime validation pending |
| 7 | Presentation/dialogue control | Native HUD subtitles, facing callbacks, voice suppression/restoration, speaker-FormID FaceGen lipsync, notifications, and in-memory dialogue capture are present. | Built; presentation matrix pending |
| 8 | Inventory/trade | Native barter/container lifecycle and condition-aware pre/post player/counterparty inventory and caps snapshots are present. | Built; trade matrix pending |
| 8 | Remove trade files | Trade files remain fallback until barter and companion-trade tests pass. | Not yet eligible |

## Current Artifact

- Deployment: `C:\Modlists\Fallout TTW\mods\Dialectic_dev`
- Architecture: PE `0x014C` (x86)
- SHA256: `6D3518E920A6F598CE88063329B6558186AA518A3503D3DE306010BFF59FD5F0`
- Static verifier: passed

## Remaining Proof

Run every section of `NATIVE_RUNTIME_VALIDATION.md` against this artifact and
retain the resulting current `dialectic.log`. Bridge families may then be
removed one domain at a time, including writer, reader, startup cleanup, and
deployed script in the same change. Until that evidence exists, the original
objective is not complete.
