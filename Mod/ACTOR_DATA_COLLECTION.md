# Fallout Actor Data Collection

Dialectic uses a hybrid actor data path for Fallout: New Vegas:

1. JIP/xNVSE script-runner UDFs collect actor facts that are safer to access through script functions.
2. `dialectic.dll` owns runtime state, stale snapshot rejection, `addnpc` formatting, and HTTP delivery.
3. DialecticServer stores primary identity fields on `core_npc_master` and extended Fallout fields in `extended_data`.

## Runtime Files

The load-time script-runner bootstrap registers the configured MCM hotkeys and
actor snapshot adapter:

```text
Data/NVSE/Plugins/scripts/ln_DialecticBootstrap.txt
```

The capture UDF writes:

```text
Data/NVSE/Plugins/dialectic_actor_snapshot.tmp
```

The DLL reads that file when it registers a targeted or nearby NPC. If the
snapshot `refid` does not match the requested actor, the DLL rejects it as stale
and uses its native snapshot instead.

## Current Fields

The first pass captures:

- `name`, `refid`, `baseid`
- `gender`, `race`
- SPECIAL: `strength`, `perception`, `endurance`, `charisma`, `intelligence`, `agility`, `luck`
- Skills: `barter`, `energy_weapons`, `explosives`, `guns`, `lockpick`, `medicine`, `melee_weapons`, `repair`, `science`, `sneak`, `speech`, `survival`, `unarmed`
- Runtime stats: `level`, `health`, `health_max`, `action_points`, `action_points_max`, `scale`, `xp`, `karma`
- Equipment slots: `head`, `hair`, `upper_body`, `left_hand`, `right_hand`, `weapon`, `upper_body_addon`, `lower_body_addon`
- Inventory snapshot rows are exported separately from the fixed `addnpc` payload and posted to `DialecticServer/gamedata.php` as JSON. Current item fields are `name`, `baseid`, `count`, `equipped`, `condition`, `type`, `ammo`, and `mods`; unavailable per-instance data is sent as `null` or an empty list.

The native actor registry also supplies voice type, factions, location,
cell/worldspace, activity, and loaded-state information. Script snapshots remain
a narrow fallback for fields that are safer through JIP functions.
