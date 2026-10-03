# ParityProbe example addon

ParityProbe is the smallest xNVSE addon for Dialectic's
[plugin extension API](../../XNVSE_EVENT_API.md#plugin-extension-api). It owns
the bridge `ParityProbe` and handles one server action,
`ExtCmdParityProbe_Ping`. It reads actor state only and changes no game data.

On each new game or loaded save it:

1. registers the `ParityProbe` bridge;
2. compiles `OnExternalCommand.txt` and attaches it to
   `DialecticExternalCommand`, filtered to bridge `ParityProbe`;
3. sends plugin event `state` with data `loaded=1`.

When the server sends `ExtCmdParityProbe_Ping` for an actor, the handler sends
plugin event `ping` on that actor with the server's parameter. It then completes
the request with `Pong from <actor> to [<parameter>] talking=<0|1>`. Other
`ParityProbe` actions complete with a failure result.

## Files

Copy the contents of `Data/` into a separate mod; do not add them to
Dialectic's own package.

```text
Data/NVSE/Plugins/scripts/ln_ParityProbe.txt
Data/NVSE/user_defined_functions/ParityProbe/Register.txt
Data/NVSE/user_defined_functions/ParityProbe/OnExternalCommand.txt
```

The scripts are compiled at runtime by xNVSE `CompileScript`. No ESP or GECK
compilation is needed. Requirements:

- xNVSE 6.3.3 or newer, which Dialectic already requires. The scripts use
  only xNVSE commands (`CompileScript`, `SetEventHandlerAlt`,
  `GetCommandOpcode`, `GetPluginVersion`, `Print`) and Dialectic commands.
- JIP LN NVSE, only for its script runner, which runs `ln_ParityProbe.txt` on
  each new game or loaded save. Dialectic already requires JIP LN 57 or newer.
- A Dialectic DLL with the plugin extension commands.

The plugin version (`10103`) is not raised for the extension API, so it cannot
identify a capable DLL. `ln_ParityProbe.txt` therefore also asks xNVSE for the
opcode of every Dialectic command the compiled scripts use. `GetCommandOpcode`
returns `0` for an unregistered name, so with an older DLL the runner prints
one console line and never compiles `Register.txt`. Runtime-compiled scripts
share load-order index `0xFF`, so bridge ownership cannot tell this addon apart
from another runtime-compiled addon that registers `ParityProbe`.

## Paired server package

The game side only executes commands that DialecticServer emits. The companion
DialecticServer example is the server plugin `parity_probe`
(`examples/plugin-parity/ext/parity_probe/` in the server source). It registers
the `ExtCmdParityProbe_Ping` action. The client bridge name stays `ParityProbe`
because bridge names cannot contain `_`.

Build its schema-4 archive (manifest `name` `parity_probe`) from the
DialecticServer source root with PHP's `zip` extension, and do not commit it:

```sh
php examples/plugin-parity/build_package.php parity_probe-1.0.0.dwpkg 1.0.0
```

Ship both sides as one mod by placing it at:

```text
Data/Dialectic/server-plugins/parity_probe/1.0.0.dwpkg
```

`ServerPluginSync` sends the folder name as the package name, and the server
rejects an archive whose manifest `name` differs (case-insensitively), so the
folder must match the manifest. On launch, Dialectic probes the configured
server and uploads the package when its installed version differs. Check
`[SERVER_PLUGIN_SYNC]` in `dialectic.log` and the server's Server Plugins page.
An installed package does not prove that the game addon ran.

### What the client sends

For the server side, both outbound events are JSON (the full contract is in
[the API reference](../../XNVSE_EVENT_API.md#server-actions-extcmdbridge_action)):

```json
{"schema":"dialectic.action_result.v1","action":"ExtCmdParityProbe_Ping","speaker":"Veronica","speaker_refid":"0x000E32A9","target":"<parameter>","result":"Pong from Veronica to [<parameter>] talking=0","status":"completed","bridge":"ParityProbe","request_id":3}
{"schema":"dialectic.plugin_event.v1","bridge":"ParityProbe","name":"ping","data":"<parameter>","actor":"Veronica","actor_refid":"0x000E32A9"}
```

The first is event type `funcret`; the second is `pluginevent`. A failure
before acceptance (for example `bridge_not_registered`) has `status` `failed`,
`request_id` `0` and `bridge` as parsed from the command. `speaker_refid` is
omitted when the server sent no speaker ref. When the parameter is empty,
`target` is the speaker ref (or `result` without one). In `action`, `target`
and `result`, `@`, `|`, tabs and newlines become spaces.

The server example's `prerequest.php` observer reads both JSON schemas. A
DialecticServer build with the plugin runtime accepts `pluginevent` passively,
without a model call. Bridge names must start with a letter on both sides. The
server only registers codes of at most 64 characters whose action part is a
letter followed by letters or digits. The client also accepts a leading digit
or `_` in the action part, but the server never emits such a code.

## Validation

Status: these scripts have not been compiled or run in game. The client
regression test (`DialecticExternalCommandRegistryTests`) covers bridge parsing,
ownership, pending bounds, timeouts and envelopes; it does not run xNVSE.

To validate in game with a disposable save:

1. Confirm `dialectic.log` contains
   `[EXTERNAL_COMMAND] register bridge=ParityProbe ... result=registered` (or
   `already_owned` after a reload) and no compile errors from xNVSE.
2. Trigger the server action for a nearby eligible NPC. The log should show
   `accepted external command`, `dispatched ... handlers=1` and `completed`, and
   the server should receive one `funcret` with `status` `completed` and the
   same `speaker_refid`.
3. Start the game without these files and trigger the action again. Expect a
   `funcret` with `status` `failed` and reason `bridge_not_registered`.
