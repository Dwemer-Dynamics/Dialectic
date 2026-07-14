# Dialectic

Dialectic is the Fallout: New Vegas and Tale of Two Wastelands client for AI
NPC interaction.

- `Plugin/`: native 32-bit xNVSE plugin source for `dialectic.dll`.
- `Mod/Data/`: deployable ESP, MCM, JIP script-runner, UI, and voice assets.
- `docs/`: native-runtime migration and validation records.

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
