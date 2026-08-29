# Dialectic Stutter Diagnostics

Use this procedure when Fallout New Vegas or Tale of Two Wastelands develops a
regular hitch while Dialectic is enabled. The diagnostic build records callback
cadence and workload; it does not intentionally pause the game or collect a
full trace.

## Test Procedure

1. Start the game through the normal MO2 profile and load a save.
2. Wait 30 seconds after the loading screen closes.
3. Stand still with no menus open for 60 seconds.
4. Move around the same loaded area for 60 seconds.
5. Send one Dialectic chat message and wait until all NPC audio finishes.
6. Continue moving for another 60 seconds.
7. Quit the game normally before collecting the logs.

Do not run a world-data synchronization or voice batch upload during this test.
Note the approximate real-world time when the visible stutter becomes severe.

## Logs To Collect

- `Fallout New Vegas\dialectic.log`
- `Fallout New Vegas\NVSE\nvse.log`
- `Fallout New Vegas\NVSE\kNVSE.log`
- `Fallout New Vegas\falloutnv_error.log`, when present
- The MO2 diagnostic archive, when reproducing on a packaged installation

## Diagnostic Markers

- `[HITCH]`: A main-loop callback gap of at least 30 ms. It includes the
  estimated time outside Dialectic's previous callback, current and previous
  plugin work, queue/task state, game state, and script bridges active near the
  hitch.
- `[HITCH_SUMMARY]`: Ten-second cadence totals, gap buckets, plugin work, and
  invocation counts for each instrumented GECK bridge.
- `[BRIDGE_STORM]`: A permanent bridge is running substantially faster than its
  designed cadence, usually because more than one self-rescheduling chain is
  active.
- `[PROCESS_HEALTH]`: Thirty-second memory, CPU time, page-fault, process I/O,
  handle, logging, and queue high-water deltas.
- `[PERF]`: Five-second microsecond-resolution timing for individual Dialectic
  game-loop subsystems.
- `[RUNTIME_HEALTH]`: Existing queue, task, cache, spatial, and audio health
  state.

## Initial Interpretation

- High `outside_callback_ms` with low `plugin_work_ms` means the hitch occurred
  outside the native Dialectic callback. Compare `recent_bridges` and
  `bridge_ticks` to identify a periodic GECK bridge or another game plugin.
- High `plugin_work_ms` should have a corresponding slow `[PERF]` subsystem.
- Increasing working set, private memory, handles, pending tasks, or dispatcher
  high-water values indicates accumulating runtime state.
- High logger `write_ms`, `lock_wait_ms`, or `slow` counts means log I/O itself
  is contributing to frame stalls.
- Stable Dialectic timings with matching entries in `kNVSE.log` or
  `falloutnv_error.log` points to animation, asset, or another plugin path.
