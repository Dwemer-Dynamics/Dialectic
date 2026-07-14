# Dialectic Runtime Bridge Inventory

This inventory is the deletion checklist for the native runtime migration. A row
may be removed only when its native producer is authoritative, comparison logs
have passed the in-game matrix, and the writer, reader, startup cleanup, and
deployed script are removed in the same change.

Generate the exact per-file ownership inventory at any time with:

```powershell
powershell -ExecutionPolicy Bypass -File Plugin\tests\report-runtime-bridges.ps1
```

The report enumerates every referenced `.tmp`/`.txt` bridge, its DLL owners,
script owners, and whether `main.cpp` still has a startup-cleanup reference. The
domain table below owns disposition; the generated report owns the exact file
list so newly introduced bridges cannot remain hidden behind wildcard wording.

| Domain | Bridge files | Script owner / writer | DLL reader / writer | Disposition |
| --- | --- | --- | --- | --- |
| Game state | `dialectic_game_state.tmp` | `GameStateTick.txt` | `GameLoop`, `TradeManager` | Native-authoritative. The legacy polling UDF is no longer started by the bootstrap. |
| World context | `dialectic_world_context.tmp` | `WorldContextTick.txt` | `WorldContextFNV` | Keep narrow adapter for weather/calendar until native equivalents are proven. Cell/worldspace/player position are native. |
| Actor positions | `dialectic_actor_positions.tmp` | `SpatialScanTick.txt` | `ActorPositionResolverFNV` | Native current-cell registry is authoritative. Direct `ActorProcessManager` traversal was removed after a stale actor entry caused a main-thread crash; adjacent exterior-cell coverage requires a lifecycle-fed safe registry. |
| Actor activity | `dialectic_activity_status.tmp` | activity scripts | `ActivityStatusFNV` | Replace with native actor/process state where documented; retain unknown state as adapter. |
| Actor metadata request | `dialectic_actor_snapshot_request.tmp`, `dialectic_actor_snapshot.tmp`, `dialectic_actor_snapshot_status.txt` | `ActorSnapshotRequestTick.txt`, `CollectActorSnapshot.txt`, `GameStateTick.txt` | `AgentManager` | Native-first profile/equipment/inventory capture; fallback only when actor is absent from native registry. Remove after profile validation. |
| Action request | `dialectic_action_request.tmp`, `dialectic_action_status.txt`, `dialectic_action_bridge_debug.txt`, `dialectic_action_diagnostics.txt` | `ActionCommandTick.txt` | `ActionManager` | Typed dispatcher plus cached in-memory NVSE callbacks are authoritative for the complete action set; request/status files remain failure fallback pending action matrix validation. |
| Attack state | `dialectic_attack_state.tmp`, `dialectic_attack_cleanup.tmp`, attack status files | `ActionCommandTick.txt`, `AttackStateTick.txt`, `AttackCleanupTick.txt`, `HaltActionsTick.txt` | `ActionManager` | Native generation-bound attack state records and restores both combatants. Files remain comparison/failure fallback pending combat/load validation. |
| Follow state | `dialectic_follow_state.tmp`, `dialectic_follow_state_status.txt` | `ActionCommandTick.txt`, `FollowStateTick.txt`, `HaltActionsTick.txt` | `ActionManager` | Native generation-bound state plus one cached package callback is authoritative for follow/follower/wait; bridge remains failure fallback pending matrix validation. |
| Move state | `dialectic_move_state.tmp`, `dialectic_move_state_status.txt` | `ActionCommandTick.txt`, `MoveStateTick.txt`, `HaltActionsTick.txt` | `ActionManager` | Native generation-bound state plus one cached package callback is authoritative for move/come-closer/travel/seat; bridge remains failure fallback pending matrix validation. |
| Pickup state | `dialectic_pickup_state.tmp`, `dialectic_pickup_move_marker.tmp`, `dialectic_pickup_state_status.txt` | `ActionCommandTick.txt`, pickup polling | `ActionManager` | Native generation-bound move-then-transfer state is authoritative and verifies the inventory delta. Files remain fallback pending pickup validation. |
| Halt actions | `dialectic_halt_actions.tmp`, `dialectic_halt_actions_status.txt` | `HaltActionsTick.txt` | `ActionManager`, `GameLoop` | Replace request transport with direct runtime cancellation; keep MCM hotkey command. |
| Nearby items | `dialectic_nearby_items.tmp` | `NearbyItemsTick.txt` | `NearbyItemsFNV`, `ActionManager` | Replace with native loaded-reference snapshot. |
| Nearby furniture | `dialectic_nearby_furniture.tmp` | `NearbyFurnitureTick.txt` | `ActionManager` | Replace with native loaded-reference snapshot. |
| Points of interest | `dialectic_nearby_pois.tmp` | `NearbyPoiTick.txt` | `NearbyPoiFNV`, `ActionManager` | Replace doors/markers with native loaded-reference snapshot; retain narrow nav/path adapter if needed. |
| Spatial path observations | path query/status files | spatial/path scripts | `SpatialPathProviderFNV`, `SpatialAwarenessFNV` | Native current-cell navmesh graph is authoritative when ready. The script result cache is a lenient fallback only while native capture/build is unavailable or an exterior graph is incomplete. |
| Quest journal | `dialectic_quests.tmp`, `dialectic_active_quest_direct.tmp`, `dialectic_quest_state_status.txt` | `ActiveQuestDirectTick.txt` | `QuestJournalFNV`, `ActionManager` | Replace selected quest/objective reads with native player quest data after validation. |
| Trade session | `dialectic_trade_session.tmp`, `dialectic_trade_pre_player.tmp`, `dialectic_trade_pre_counterparty.tmp`, `dialectic_trade_post_player.tmp`, `dialectic_trade_post_counterparty.tmp`, `dialectic_trade_done.tmp`, `dialectic_trade_status.txt` | `ActionCommandTick.txt`, `TradeSessionTick.txt`, `GameStateTick.txt` | `TradeManager` | Native menu lifecycle and native condition-aware deltas are authoritative; bridge is fallback pending barter and companion-trade tests. |
| Dialogue capture | `dialectic_dialogue_capture.tmp`, `dialectic_dialogue_capture_itr.tmp`, `dialectic_dialogue_player_choice.tmp`, `dialectic_dialogue_speaker.tmp`, dialogue debug files | dialogue event handlers and menu/topic scripts | `GameLoop`, `main` | Event scripts now submit through `DialecticCaptureDialogue`; canonical files are comparison-only until menu choice and radiant attribution pass. Redundant AppData/Documents copies are removed. |
| Dialogue suppression | dialogue guard ref/mod/local, saved voice, interrupt, sound, recovery, and status files | `DialogueGuardTick.txt`, `DialogueGuardRecoverVoiceTick.txt`, dialogue handlers | `SpeakManager` | Native deterministic voice guard is authoritative. The permanent guard poller is no longer started; recovery runs once after load to repair a save made during guarded speech. |
| Passive subtitles | `dialectic_subtitle.txt`, `dialectic_subtitle_status.txt` | `SubtitleTick.txt`, dialogue handlers | `SpeakManager`, `GameLoop` | Native HUD tile is authoritative. The legacy subtitle polling UDF is no longer started by the bootstrap. |
| Facing | `dialectic_facing_speaker_ref.txt`, `dialectic_facing_target_ref.txt`, `dialectic_facing_yaw.txt`, facing status/last files | `FaceTargetTick.txt` | `SpeakManager` | Native one-shot game-thread facing is authoritative. The legacy 20 Hz console poller is no longer started. |
| Lipsync | `dialectic_lipsync_ref.txt`, `dialectic_lipsync_command.txt`, `dialectic_lipsync_status.txt` | `LipSyncCommandTick.txt` | `SpeakManager` | Direct FaceGen is adapter-owned and generation-bound. The legacy command-file poller is no longer started. |
| Notifications | `dialectic_notification.tmp` and status files | `NotificationTick.txt` | `IngameNotifier` | Native `QueueUIMessage` is authoritative; bridge is automatic failure fallback pending icon/format validation. |
| Text input | `dialectic_open_text_input.tmp`, `dialectic_textinput.tmp`, target/status files | `OpenTextInputMenu.txt`, `TextInputMenuTick.txt` | `GameLoop` | Retain MCM/UI adapter until native TextEdit menu ownership is stable; eliminate duplicate target state via native registry. |
| Mode/model/profile menus | open/select/status files for mode, LLM model, and dynamic profile | menu tick/select scripts | `GameLoop` | Native in-memory opening is authoritative and the permanent menu pollers are no longer started. Script selection callbacks remain. Server owns selected values. |
| RPG events | `dialectic_rpg_events.tmp` and event status files | aid/lockpick/RPG handlers | `GameLoop` | Keep narrow event callbacks where xNVSE event manager is authoritative; remove polling aggregation file. |

## Startup Cleanup Ownership

`main.cpp` currently clears many transient bridge files during startup. Each
entry must be deleted from that cleanup list in the same commit that removes its
last producer and consumer. Status and diagnostics files may remain only when
they describe the new native path rather than acting as synchronization state.

## Validation Evidence

Runtime evidence is recorded by `[NATIVE_RUNTIME]`, `[NATIVE_DIALOGUE_GUARD]`,
native subtitle, native trade, and bridge comparison logs. No bridge in this
document is considered removable based on compilation alone.
