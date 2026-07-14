# Build Dialectic

## Requirements

- Visual Studio 2022 with C++ tools
- CMake 3.21 or newer
- Windows SDK
- xNVSE runtime installed for the game

## Configure And Build

```powershell
cmake --preset x86-release
cmake --build --preset x86-release
```

The plugin must be built as Win32/x86 because Fallout: New Vegas is a 32-bit game.

Expected output:

```text
out/build/x86-release/bin/Release/dialectic.dll
```

## Install

Copy the DLL and INI to:

```text
Fallout New Vegas/Data/NVSE/Plugins/
```

Required files:

- `dialectic.dll`
- `dialectic.ini`

The plugin creates `dialectic_custom.ini` on first run. MCM and runtime changes are
saved there so replacing `dialectic.ini` during an update does not reset user settings.
