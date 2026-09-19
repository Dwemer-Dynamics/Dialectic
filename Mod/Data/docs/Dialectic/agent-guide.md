# Dialectic agent guide

## Runtime flow

1. xNVSE loads `NVSE/Plugins/dialectic.dll`. JIP LN runs
   `NVSE/Plugins/scripts/ln_DialecticBootstrap.txt` on a new game or loaded save.
   This is the single owner of runtime bridge registration.
2. MCM supplies hotkey bindings. The native client gathers player, target,
   nearby-actor and world context. Microphone input uses speech-to-text (STT).
3. Requests reach DialecticServer's `main.php`. Its PHP pipeline combines game
   context, profiles and memory, then calls AI and text-to-speech (TTS) services.
4. Client response routing resolves the actor, queues speech and actions, and
   returns game work to the game thread. An HTTP response alone does not prove
   that speech played or an action ran.

## Source map

Paths are relative to a client checkout, not this installed documentation folder.
All native filenames below are in `Plugin/src/`.

| Task | Start here |
| --- | --- |
| Startup and compatibility | `main.cpp`, `XNVSEAdapter.cpp`, `FNVRuntime.cpp` |
| Input, microphone, settings | `InputManager.cpp`, `VoiceRecorder.cpp`, `Config.cpp` |
| Requests and discovery | `HTTPManager.cpp`, `VersionCheck.cpp` |
| Actor routing and sound | `ResponseRouter.cpp`, `ResponseQueueFNV.cpp`, `SpeakManager.cpp`, `AudioManager.cpp` |
| Actions and game-thread work | `ActionManager.cpp`, `GameThreadDispatcher.cpp`, `TaskManager.cpp` |
| World, quests and NPC context | `WorldContextFNV.cpp`, `QuestJournalFNV.cpp`, `NearbyActorsFNV.cpp` |
| Script bridge | `Mod/Data/NVSE/Plugins/scripts/`, `Mod/Data/NVSE/user_defined_functions/Dialectic/` |
| MCM and menus | `Mod/Data/MCM/`, `Mod/Data/menus/` |
| Profiles, prompts and providers | Companion server: `main_dialectic_pipeline.php`, `lib/core/`, `connector/`, `tts/`, `stt/` |

## Installation and troubleshooting

Install the complete payload and enable `Dialectic.esp`. TTW is supported but
not required; the ESP's master is `FalloutNV.esm`. Use the matching release's
README requirements, including xNVSE, JIP LN, JohnnyGuitar, ITR, MCM and its
dependencies. Optional PipVision also needs SUP NVSE. Hotkeys start unbound.

`NVSE/Plugins/dialectic.ini` contains shipped defaults. The separate custom
mod's `NVSE/Plugins/dialectic_custom.ini` overrides them. Confirm which mod wins
before diagnosing an ignored setting. Profiles and AI credentials belong to
the server.

- Read the current `dialectic.log` in the Windows Documents known folder under
  `My Games/FalloutNV/NVSE/`. If unavailable, `Logger.cpp` tries the game
  directory, `Data/NVSE/Plugins/`, then the working directory. Check timestamps
  and mod-manager overwrite folders before using old logs.
- For a missing plugin or bridge, correlate xNVSE/JIP logs, runtime versions
  and the effective bootstrap script. Preserve the single bootstrap owner.
- For request failures, inspect discovery/custom host settings, server `log/`
  and PHP/web-server logs for the same request and time. Redact secrets.
- For wrong speakers or absent audio, trace actor identity through request,
  reply, routing and playback. A playback error does not establish STT failure.
- Record FNV versus TTW, client/server versions and an exact reproduction.
  Successful builds do not establish in-game behavior.

## Custom mods and server plugins

For another game mod, use the documented actor-bound
[public xNVSE events](https://github.com/Dwemer-Dynamics/Dialectic/blob/unstable/docs/XNVSE_EVENT_API.md)
instead of calling internal scripts. The source checkout includes that same
guide at `docs/XNVSE_EVENT_API.md` and registration in `ExternalEventAPI.cpp`.

For example, `VeronicaREF.DispatchEventAlt "DialecticSpeakExact", "Hello."`
requests exact speech on that actor. `DialecticAsk` enters normal conversation;
`DialecticComment` and `DialecticReact` request contextual speech. Recruitment,
dismissal, waiting and resuming have separate events. Read the full contract:
the actor must be loaded, alive and eligible; requests are asynchronous and
their acceptance/rejection is logged. Test with the intended actor, busy states,
save/load and both FNV/TTW environments you claim to support.

For server packages, read the companion server's
[agent guide](https://github.com/Dwemer-Dynamics/DialecticServer/blob/unstable/docs/agent-guide.md)
and `lib/plugin_package_manager.php`. `ServerPluginSync.cpp` owns game-side
package discovery and upload. A server package is not a native xNVSE DLL;
shipping PHP files does not automatically register new game actions or hooks.
Keep the extension in its own repository and include its own instructions,
supported versions, installation steps and validation evidence.
