---
title: "ADR 0034: X11 Sound Through the Session Program"
description: An X11 kiosk's sound reaches the agent the way GNOME's does — the kiosk's session program connects to the agent and hands over a PipeWire connection narrowed to the speakers — rather than the agent opening the kiosk user's sound server.
---

- **Status**: accepted
- **Date**: 2026-10-12
- **Supersedes**: — (amends [ADR-0028](0028-desktop-session-helper.md)'s
  "X11 kiosks (A) need no long-running helper")

## Context

M3.5 slice 3.5.2 puts the robot desktop's sound on `fjarr.desktop`
([docs/23](../23-agent-core-architecture.md#desktop-audio)). On GNOME the session helper
hands the agent a PipeWire connection narrowed to the default output, as it hands the
screen; measured in the fixture, that connection hears the speakers and never a
microphone.

An X11 kiosk (backend A) has no such helper. ADR-0028 settled that the X11 module opens the
display from the agent's own process, after the kiosk session grants the agent's account
with `xhost +si:localuser:…`. X has that per-user grant; the sound server has none. The
kiosk user's PipeWire (and its PulseAudio socket, `pipewire-pulse`) lives in that user's
`XDG_RUNTIME_DIR`, mode 0700, so the agent cannot open it, and should not open all of it:
a live microphone is as sensitive as a terminal (docs/10).

The kiosk session already runs `fjarr-x11-session` for the whole session, as that user: it
grants the agent and re-lays the outputs on every RandR change.

## Options considered

1. **The session program hands over the sound, as the GNOME helper does.**
   `fjarr-x11-session` also connects to the agent's helper socket and serves the sound part
   of the helper protocol (`speakers`, `start-audio` → `audio-started` with a connection
   narrowed to the default sink). Pros: the same account boundary as GNOME, proven the same
   way (the microphone is unreachable through the handed connection); the same code on both
   ends (the speaker watch and the narrowing in the session program, the audio reader in the
   module). Cons: the X11 module gains a socket listener and a peer check; the kiosk account
   joins `fjarr-desktop`; `fjarr-desktop-x11` gains libpipewire.
2. **A second PulseAudio socket for the agent.** `setup` configures the kiosk user's
   `pipewire-pulse` to listen also on a socket in `/run/fjarr` that the agent's group may
   open. Pros: little code; the agent reads the monitor source as any Pulse client. Cons: a
   Pulse client may open every source, so the agent's account would reach the microphones —
   the boundary the GNOME path keeps would not hold on X11; a configuration file in the
   kiosk user's PipeWire setup to own and undo.
3. **No desktop sound on X11.** Pros: nothing to build. Cons: a kiosk that plays alarms or
   media (the mini-PC's YouTube test) cannot be heard; the two backends differ for no
   reason the operator can see.

## Decision

**Option 1.** The kiosk's session program is a helper for the sound only: it connects to
the agent's helper socket (`/run/fjarr/desktop.sock`, the same one module E uses; only one
backend runs), says `hello` with `session.type: "x11"`, and from then on reports the
speakers and hands them over narrowed, exactly as the GNOME helper does. Everything else on
X11 stays as ADR-0028 has it: the display is opened by the agent, granted by X.

The deciding reason is the account boundary: the agent must reach what the desktop plays
and nothing else of the desktop user's, on every backend. Option 2 cannot give that.

## Consequences

- ADR-0028's line "X11 kiosks (A) need no long-running helper" no longer holds for the
  sound. The session program was already long-running for the layout; it now also holds a
  connection to the agent and reconnects when the agent restarts. Without the agent's socket
  it still grants and lays out, so an X11 kiosk without sound is no worse than before.
- `setup desktop --x11` must know the kiosk account (`--account`, or LightDM's
  `autologin-user`), add it to `fjarr-desktop`, and record it as `helper.user`, as the
  GNOME path does. Without an account the agent has no sound on X11 and says so.
- The X11 fixture runs PipeWire and WirePlumber with a null sink, so the X11 path is tested
  in CI as GNOME's is.
- Revisit if a kiosk runs without PipeWire (a bare PulseAudio or ALSA-only session): then the
  session program has nothing to narrow, and option 2's account question comes back.
