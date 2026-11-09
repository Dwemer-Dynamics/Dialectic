# Dialectic changelog

## 1.2.0 — 2026-11-08 release candidate

Changes from 1.1.3, paired with DialecticServer 1.2.0:

- Added actor-bound addon command bridges, command status, agent queries, context refresh, and interaction controls.
- Added request-scoped addon messages, reactions, and context without changing global dialogue settings.
- Restricted automatic AI participation to registered agents while retaining microphone input independently of addon controls.
- Routed active quest updates through eligible nearby speakers and unified NPC and narrator quest commentary.
- Added one-shot NPC hypnosis mode.
- Expanded the public xNVSE integration guide and included a parity-probe example for addon authors.
- Added server plugin catalog channels, package upload, retained-data removal, extension hooks, and bounded opt-in action follow-ups.
- Added DwemerDistro LLM Studio setup and improved PocketTTS provider detection.

### Upgrade notes

- Update the client and server together. Retain the separate custom mod and its `dialectic_custom.ini`.
- Production licensing and existing Ploofy support are retained.
- This candidate does not include server shared-key or relay-security changes that remain on `unstable`, or the open relationship fixes.
- Build and static checks do not establish in-game acceptance. Gameplay and provider checks remain release gates.
