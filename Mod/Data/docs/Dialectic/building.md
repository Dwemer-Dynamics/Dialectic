# Building and packaging Dialectic

Use a source checkout of [Dialectic](https://github.com/Dwemer-Dynamics/Dialectic)
matching the intended release. An installed mod lacks the build sources.
`Plugin/CMakeLists.txt` and `Plugin/CMakePresets.json` own build settings.

## Native build

Install Visual Studio 2022 with Desktop development with C++, x86 MSVC and the
Windows SDK, plus CMake 3.21 or newer. C++20 is required. The xNVSE SDK is tracked
in `Plugin/vendor/xnvse-sdk`; private monorepo scripts are not needed.

From the repository root in PowerShell, stop if any command fails:

```powershell
# Prevent the optional post-build hook from copying into a game installation.
Remove-Item Env:FNV_DATA_PATH -ErrorAction SilentlyContinue
Push-Location Plugin
cmake --preset x86-release
cmake --build --preset x86-release
ctest --test-dir out/build/x86-release -C Release --output-on-failure
Pop-Location
powershell -ExecutionPolicy Bypass -File Plugin/tests/verify-release-tree.ps1
```

The preset selects Win32/x86 Release. Output:
`Plugin/out/build/x86-release/bin/Release/dialectic.dll`. Never substitute x64.
CTest checks native units; the release-tree check covers tracked assets, JSON,
scripts and ESP structure. Neither proves xNVSE loading or in-game behavior.

## Package contract

The main payload is the complete `Mod/Data/` tree plus the verified x86 DLL at
`NVSE/Plugins/dialectic.dll`. Retain `docs/Dialectic/AGENTS.md`, `agent-guide.md`
and `building.md`. These are canonical source files; no generated documentation
copy needs refreshing. `README.txt` makes them discoverable. Do not install a
generic `AGENTS.md` into a shared game Data root.

The separate custom package uses
`Mod/Data/NVSE/Plugins/dialectic_custom.ini.example` as
`NVSE/Plugins/dialectic_custom.ini`. Never package a player's live override.
Keep DLLs, archives, logs and personal configuration out of source PRs.

Maintainer deployment and beta packaging automation lives outside this public
repository. It copies the complete Data payload. Before release, extract the
archive and verify the DLL architecture/version, required assets, all three
guides and relative links. Preserve custom settings. Deployment and release
require their own authorization; a staged archive is not a published release.
