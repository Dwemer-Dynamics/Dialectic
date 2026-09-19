# Dialectic agent instructions

This repository owns the Fallout: New Vegas / Tale of Two Wastelands (TTW)
32-bit xNVSE client. Its independent backend is
[DialecticServer](https://github.com/Dwemer-Dynamics/DialecticServer).

Read the [bundled entry point](Mod/Data/docs/Dialectic/AGENTS.md),
[runtime guide](Mod/Data/docs/Dialectic/agent-guide.md), and
[build guide](Mod/Data/docs/Dialectic/building.md). These are the canonical
guides shipped with the mod; edit them in place instead of duplicating them.

- Confirm remote, branch, working-tree changes and overlapping PRs. Preserve
  unrelated work; use an isolated feature branch when the checkout is active.
- Follow [CONTRIBUTING.md](CONTRIBUTING.md); normal draft PRs target `unstable`.
  A PR does not authorize deployment, promotion or release.
- Keep native builds Win32/x86, the default/custom INI split, and the single
  load-time script-runner owner. Review both sides of protocol changes.
- Use existing focused checks. Report build, package, deployment and in-game
  evidence separately. Keep generated binaries and local data out of PRs.
