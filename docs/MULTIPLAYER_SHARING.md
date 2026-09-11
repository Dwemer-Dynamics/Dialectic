# Optional multiplayer dialogue sharing (beta)

Requires the matching DialecticServer relay feature. Setup and server access details:
[DialecticServer multiplayer sharing guide](https://github.com/Dwemer-Dynamics/DialecticServer/blob/codex/nvmp-dialogue-sharing/docs/MULTIPLAYER_SHARING.md).

In MCM > Tools, **(Beta) Multiplayer Dialogue Sharing** uses 0 Off (default), 1 Host, or 2 Listen.
Set `URL`, `Session` and `Key` under `[Multiplayer]` in
`Data/NVSE/Plugins/dialectic_custom.ini`. The host uses the host key; listeners use
the separate listener key. Close MCM to reload the settings, or restart the game.

The dialogue host runs normal AI conversations. Listeners play the same ordinary
NPC speech and passive subtitles, centered at their own voice volume, without
actor actions or AI requests. Narrator/head voices and Player TTS are excluded.
Listening continues to be passive if the host or server disconnects.

This is audio/subtitle sharing for co-op testing, not follower or game-state
synchronization. It does not map NVMP actors, provide positional audio or lip sync
on listeners, or accept guest AI input. New listeners start with new lines, and
network delay means playback is not exactly simultaneous. Missing or stale audio
is skipped instead of replaying a long backlog.

Host pause/resume and cancellation are relayed. Mode changes clear pending speech.
Internet relay URLs require HTTPS; private IPv4/loopback URLs may use HTTP. Only
the relay endpoint needs to be reachable from a listener. Keep keys out of logs,
screenshots and bug reports. Use one audio-sharing method to avoid Discord echo.

Two-PC NVMP, JIP follower behavior, MCM rendering and actual in-game listening must
be verified separately from source builds and protocol probes.
