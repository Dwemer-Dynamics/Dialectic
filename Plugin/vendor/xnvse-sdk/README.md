# Pinned xNVSE SDK Headers

Source: https://github.com/xNVSE/NVSE

Revision: `03684a6bdf7ca583ba851e8efdb762178654d828`

The `nvse` and `common` directories contain the header-only SDK surface used by the Dialectic plugin. They are vendored to keep local and CI builds reproducible. Runtime integration belongs in `src/XNVSEAdapter.*`; other Dialectic modules should depend on the adapter rather than including SDK headers directly.

The upstream common-component license is preserved in `common/common_license.txt`.
