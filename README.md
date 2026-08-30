# Dialectic

Dialectic is the Fallout: New Vegas and Tale of Two Wastelands client for AI
NPC interaction.

- `Plugin/`: native 32-bit xNVSE plugin source for `dialectic.dll`.
- `Mod/Data/`: deployable ESP, MCM, JIP script-runner, UI, and voice assets.
- `docs/`: native-runtime migration and validation records.

## Requirements

### Game and Runtime

- [Fallout: New Vegas](https://store.steampowered.com/app/22380/Fallout_New_Vegas/).
  [Tale of Two Wastelands](https://taleoftwowastelands.com/) is supported but
  is not required; `Dialectic.esp` only has `FalloutNV.esm` as a master.
- [xNVSE 6.3.3 or newer](https://github.com/xNVSE/NVSE/releases).
- [JIP LN NVSE Plugin 57 or newer](https://www.nexusmods.com/newvegas/mods/58277).
- [JohnnyGuitar NVSE 5.17 or newer](https://www.nexusmods.com/newvegas/mods/66927).
- [SUP NVSE 8.55 or newer](https://www.nexusmods.com/newvegas/mods/73160) is
  required for the optional PipVision screenshot-context hotkey.
- [ITR NVSE 1.0.0 or newer](https://www.nexusmods.com/newvegas/mods/96922).
  Dialectic uses its dialogue and sound events to capture and suppress vanilla
  dialogue correctly while AI speech is playing.
- [Microsoft Visual C++ 2015-2022 Redistributable (x86)](https://aka.ms/vs/17/release/vc_redist.x86.exe).
  The 32-bit runtime is required even on 64-bit Windows.

### MCM

The in-game configuration menu and hotkey binding require the current versions
of:

- [The Mod Configuration Menu](https://www.nexusmods.com/newvegas/mods/42507).
- [MCM Extender](https://www.nexusmods.com/newvegas/mods/93642).
- [ShowOff xNVSE](https://www.nexusmods.com/newvegas/mods/72541) and
  [UIO](https://www.nexusmods.com/newvegas/mods/57174), which are requirements
  of MCM Extender.

Install these framework mods before Dialectic. Hotkeys are unbound by default,
so MCM is part of the supported installation rather than an optional convenience.

### Server

Dialectic requires a running, matching-version
[DialecticServer](https://github.com/Dwemer-Dynamics/DialecticServer), normally
installed and managed through DwemerDistro. The client can use launcher
autodiscovery or a manually configured server address in
`dialectic_custom.ini`.

## Build

```powershell
cd Plugin
cmake --preset x86-release
cmake --build --preset x86-release
```

Fallout: New Vegas is 32-bit, so release builds must use the Win32/x86 preset.

Validate the tracked package before release:

```powershell
powershell -ExecutionPolicy Bypass -File Plugin\tests\verify-release-tree.ps1
```

## Development Flow

Changes move through `feature/* -> unstable -> dev -> dialectic`. Open normal
pull requests against `unstable`; promotion pull requests move the accumulated
changes from `unstable` to `dev`, then from `dev` to the release branch
`dialectic`. See [CONTRIBUTING.md](CONTRIBUTING.md) for the full policy.

## Voice Samples

Dialectic does not distribute voice audio extracted from Fallout game files.
`Mod/Data/Dialectic/fallout_builtin_voices.csv` maps voice types to samples in
the installed game's `Data/Sound/Voice` tree. Dialectic imports those samples
when an NPC is encountered.

## Runtime

JIP LN runs `Mod/Data/NVSE/Plugins/scripts/ln_DialecticBootstrap.txt` once for
each new game or loaded save. That script is the single owner of runtime bridge
registration. Hotkeys are unbound by default and configured through MCM.

Other xNVSE mods can use Dialectic's actor-bound public events for exact TTS,
contextual speech, reactions, questions, prompt opening, and deterministic
follower control. See [Public xNVSE Event API](docs/XNVSE_EVENT_API.md).
