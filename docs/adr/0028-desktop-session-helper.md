---
title: "ADR 0028: The desktop is reached through a session helper"
---

- **Status**: accepted
- **Date**: 2026-09-27
- **Supersedes**: —

## Context

[ADR-0006](0006-desktop-backend-selection.md) chose mutter's own ScreenCast and
RemoteDesktop interfaces (candidate E) for stock Ubuntu, and the portal (C) as
second. Both are reached over the **desktop user's D-Bus session bus**. The
session bus accepts only processes running as that same user. The spikes ran
every probe as the auto-login user, so they never had to answer how anything
else gets in.

Several accepted decisions assume the agent is not that user:

- [docs/10](../10-security.md#robot-side-privilege-layout-adr-0009) runs
  `fjarr-agent` as its own unprivileged account, `fjarr`, which also owns the
  robot's device key (`0600`, docs/10 enrollment).
- [ADR-0021](0021-desktop-backends-as-runtime-modules.md) loads desktop
  backends into the agent's process as runtime modules.
- libfjarr is **library-first**: it embeds in the customer's robot software,
  which runs as whatever service account the customer chose, almost never the
  desktop user.

The spikes added two facts. The desktop session is not durable: GNOME Shell
crashed once in four unplugs of every monitor, and the session ended with it. And
GNOME hands the hardware encoder's render node to the seat's user by ACL.

So something has to cross from the agent's account into the desktop user's
session, and nothing specified how.

## Options considered

1. **Run the agent as the desktop user, as a user service inside the
   session.** It is the simplest process model, but the agent would die with the
   desktop. When GNOME crashed, the robot would lose its camera, terminal and
   tunnel along with the screen, until a watchdog restored the session. Rejected.
2. **Run the agent as the desktop user, from a system unit (`User=`).** It
   outlives the session and needs no IPC. But every app in the desktop session
   (a kiosk HMI, a browser) could then read the device key and impersonate the
   robot. A compromise of the agent would also land in the desktop user's files.
   And it does nothing for an embedded libfjarr, which still runs as the
   customer's account. The option would be a second path, not a replacement.
3. **A session helper that hands the agent file descriptors.** A small process
   runs *in* the desktop session as its user. It is the only thing that talks
   to mutter or the portal. It creates the sessions and passes the agent the
   two descriptors that carry the heavy traffic: a PipeWire connection for
   capture and an EIS socket (libei) for input. It also relays the low-rate
   events: monitors, hot-plug, capture loss, clipboard, cursor mode. This is
   the portal's own design, since `OpenPipeWireRemote` and `ConnectToEIS` hand
   out exactly these descriptors across a trust boundary. Cost: one more
   process, a small socket protocol, and a lifecycle to supervise.
4. **A root agent or root broker that enters the user's session**, via
   `setpriv`/`machinectl` onto the user's bus. That puts root inside a
   network-facing process, or next to it. Rejected (docs/10).
5. **A helper that captures and encodes itself** and streams encoded frames to
   the agent. It would move the media plane (FrameHub, rate control, tiers)
   into a second process or duplicate it. Rejected: descriptor handover gives
   the agent the frames without that.

## Decision

**Option 3.** `fjarr-desktop-session` runs as a systemd **user** service bound
to `graphical-session.target` of the configured desktop account. It ships in
the desktop package, `fjarr-desktop-wayland`.

- **Direction and trust.** The agent listens on `/run/fjarr/desktop.sock`
  (owned by the agent's user, group `fjarr-desktop`, mode `0660`), and the
  desktop account is the group's only member. The helper connects out to it,
  and reconnects after either side restarts. The agent accepts a connection
  only from the configured desktop user's uid (`SO_PEERCRED`). The helper serves
  only that one peer.
- **What crosses.** The PipeWire connection and the EIS socket, as descriptors,
  so frames and input never pass through the helper. Also monitors and
  hot-plug, capture loss (including the silent end of a stream whose monitor
  went away), clipboard offers and data, and the cursor mode. The agent-side
  module (ADR-0021's runtime module) implements `DesktopBackend` (docs/09) on
  top of this. The interface is unchanged by where the work happens.
- **Lifetime.** The agent outlives the session. When the helper disconnects,
  every capture ends with `CaptureLost::SessionEnded`. When a new session's
  helper connects, the capability rebuilds its tracks. The GDM watchdog
  ([ADR-0006](0006-desktop-backend-selection.md)) is a separate root-owned unit,
  not the agent's job.
- **X11 kiosks (A)** need no long-running helper. The kiosk session grants the
  agent's account access with
  `xhost +si:localuser:<agent user>` at session start. That is X's own
  per-user grant; no authority file changes hands. The agent then opens the
  display itself.
- **The encoder.** The installer adds the agent's user to `render`, so VA-API
  does not depend on the seat's ACL.

Why this and not option 2, which is simpler: it is the only option that also
serves **embedded libfjarr**, which is the product's primary shape and runs
under the customer's account. It keeps the **device key** away from desktop
apps. And it keeps the agent alive when the desktop is not. Option 2 would have
been a second path for the reference daemon alone.

## Consequences

- **M3 proves the handover first.** An agent running as another account
  captures from a PipeWire descriptor and injects through an EIS descriptor that
  the helper handed it. This has not been measured here; the portals rely on the
  same mechanism, which is the reason to expect it to hold. If it fails, this
  ADR is revisited before any backend is built on it.
- The desktop account is the security boundary on GNOME
  ([docs/10](../10-security.md)). The helper lives there, and so does anything
  else that account runs. The device key and the network-facing code do not.
- One more package artefact: a user unit plus a small binary, and a socket
  protocol that is internal and versioned with the module seam (ADR-0021's
  rule: from the first release, a change bumps the name).
- `fjarr-inputd` ([ADR-0009](0009-privilege-separation.md)) stays unbuilt. The
  helper is not privileged; it holds the desktop user's own rights and no more.
- Revisit if GNOME or the portals stop handing out descriptors. Revisit if a
  deployment needs the desktop from more than one account at once (one helper
  per account would then need naming). Revisit if the descriptor handover
  costs measurable latency, which it should not, because nothing is copied.
