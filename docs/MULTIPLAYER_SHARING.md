# Multiplayer dialogue sharing (beta)

Public sharing uses a standalone relay. Both players connect outbound; nobody forwards
ports or exposes their local DialecticServer. The maintainer must deploy the relay and
set the shipped `[Multiplayer] PublicRelayURL` to its HTTPS endpoint first. Until then,
Host/Join explains that the public relay is unavailable. No endpoint is invented here.

## Players

Open **MCM > Tools > (Beta) Multiplayer Dialogue Sharing**:

1. Host selects **Host session**, then closes MCM to connect.
2. Host selects **Copy join code**, or **Save invite to Desktop**, then closes MCM.
   Saving creates `Dialectic Invite.txt` on the Windows Desktop, including redirected/
   OneDrive Desktops. Existing files are kept; repeat saves use numbered filenames.
   The file contains only the listener join code and instructions, never host credentials.
   Open the file to copy the code or send the file to friends. Saved files remain after
   disconnect, but their codes expire with the session.
3. Friends copy that 12-character code, select **Join session (copied code)**, and close MCM.
4. **Sharing status** reports Off, connecting, hosting, listening, or disconnected/waiting.
5. **Disconnect / Off** ends hosting or leaves listening and restores normal DIALECTIC.

The host keeps their ordinary AI server configuration. Listeners need no AI-provider
credentials or local server. Public room credentials stay in memory; restarting the game
returns public sharing to Off. Create a new session after restarting. A code grants
listener access to the session: share it only with intended players. Listening suspends
local AI conversations. Narrator/head voices and Player TTS are never shared.

Host session expiry is six hours maximum or three minutes without successful host
traffic. Gameplay disconnects are detected by a 15-second lease. Save/load can reset
playback without changing the code if the host resumes within three minutes. Joining
starts at live speech; previous lines are not replayed. Audio follows the host with
network delay and is centered, not positional. NVMP players, cell proximity, NPC
ownership, follower commands and lip sync are not synchronized.

Public upload limits: 4 MiB per line, 8 MiB waiting on the host, 16 MiB/128 audio files
per room. Lines exceeding limits are skipped. Off makes no relay requests; explicitly
choosing Host/Join performs setup, and Disconnect may send one best-effort end request.

## Operators and legacy setup

[Public relay deployment](https://github.com/Dwemer-Dynamics/DialecticServer/blob/codex/nvmp-dialogue-sharing/relay/README.md)

The original self-hosted INI mode remains supported for existing users. It uses
`Mode=1` (Host) or `Mode=2` (Listen), `URL`, `Session` and `Key`, with
`Mode=0` to disable. Disconnect it before starting a public session. See the
[server guide](https://github.com/Dwemer-Dynamics/DialecticServer/blob/codex/nvmp-dialogue-sharing/docs/MULTIPLAYER_SHARING.md).

The network/state machine is validated in isolated native probes. Actual MCM rendering,
xNVSE callback execution, XAudio2 playback and two-PC NVMP sessions still require in-game
verification. No public service has been deployed by these PRs.

## Debugging a shared session

Collect both players' debugging bundles immediately after a problem, labelled Host and
Listener, plus the approximate time. The launcher collector includes `dialectic.log`
when found; attach the full file if the bundle's recent-log excerpt misses the incident.
The public service's PHP log must be collected separately by its operator.

Search for `Sharing:` and match `session=` and `line=` between the two logs. These are
truncated SHA-256 diagnostic references, not access tokens. A normal delivery records
`publish_queued`, host `op=publish accepted=1`, listener `download_result` and
`download_queued`, then `playback_started` and `playback_finished`. Rejected WAVs,
audio load/start failures, queue limits, stale audio and interruptions have distinct
stage names. Transport failures include HTTP status, Windows error code and request
duration; invalid relay replies are distinguished from network failures.

Normal successful idle polling/heartbeats are silent. Repeated main-request failures
are sampled every eighth failure (about once a minute once backoff reaches eight
seconds); recovery and session transitions are logged. Off adds no recurring log work.
These diagnostics never record keys, join codes, URLs, dialogue text or raw session/
utterance IDs. Existing ordinary Dialectic logs may still contain conversation content.
