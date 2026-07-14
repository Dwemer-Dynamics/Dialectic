# Contributing to Dialectic

## Branch Flow

Dialectic uses the following promotion chain:

```text
feature/* -> unstable -> dev -> dialectic
```

- Create work on a focused `feature/*` or fix branch.
- Open normal pull requests against `unstable`.
- Promote tested batches with a pull request from `unstable` to `dev`.
- Promote release candidates with a pull request from `dev` to `dialectic`.
- `dialectic` is the default and release branch. Do not target it directly with
  feature work.

Do not force-push or delete the `unstable`, `dev`, or `dialectic` branches.

## Validation

Build the x86 plugin and verify the release tree before opening a pull request:

```powershell
cd Plugin
cmake --preset x86-release
cmake --build --preset x86-release
cd ..
powershell -ExecutionPolicy Bypass -File Plugin\tests\verify-release-tree.ps1
```

Do not commit build output, local configuration, logs, cached responses, or
audio extracted from Fallout game files.
