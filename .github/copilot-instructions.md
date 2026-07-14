# Dialectic Repository Instructions

- `Plugin/` is a native 32-bit xNVSE plugin. Preserve x86 compatibility.
- `Mod/Data/` is the deployable Fallout: New Vegas/TTW mod package.
- Use the branch flow `feature/* -> unstable -> dev -> dialectic`.
- `dialectic` is the default release branch; normal pull requests target
  `unstable`.
- Do not commit compiled DLLs, build output, local INI overrides, logs, caches,
  or extracted Fallout game audio.
- Keep `fallout_builtin_voices.csv`; runtime code uses it to locate voice data
  in the user's installed game.
- Prefer JSON for plugin/server contracts and preserve explicit schema names.
- Validate changes with the x86 CMake preset and
  `Plugin/tests/verify-release-tree.ps1`.
