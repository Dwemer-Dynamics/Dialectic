# Dialectic xNVSE Plugin

This is the native Fallout: New Vegas plugin for Dialectic. It is bootstrapped from the old AIAgentFNV prototype and is being renamed and stabilized for Dialectic.

## Layout

```text
Plugin/
  CMakeLists.txt
  CMakePresets.json
  src/
```

## Build

Fallout: New Vegas is a 32-bit process, so build Win32/x86.

```powershell
cmake --preset x86-release
cmake --build --preset x86-release
```

The expected output is:

```text
out/build/x86-release/bin/Release/dialectic.dll
```

Install it to:

```text
Fallout New Vegas/Data/NVSE/Plugins/dialectic.dll
```

## Current Status

- CMake project/output is renamed to `Dialectic` / `dialectic.dll`.
- Shipped defaults are read from `Data/NVSE/Plugins/dialectic.ini`.
- User overrides are read from and saved to `Data/NVSE/Plugins/dialectic_custom.ini`.
- Runtime log path is `dialectic.log`.
- Canonical exports use `Dialectic_*`.
- The runtime exports only canonical `Dialectic_*` entry points.

## Next Plugin Tasks

1. Build Win32/x86 and fix any compile errors from the deprecated prototype.
2. Verify `dialectic.dll` loads through xNVSE.
3. Confirm the Obscript temp-file bridge reaches `GameLoop::SendPlayerMessage`.
