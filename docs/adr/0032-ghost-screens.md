---
title: "ADR 0032: Ghost screens are forced connectors; virtual monitors stay per session"
description: A headless robot's persistent screens come from forced DRM connectors with Fjarr-generated EDIDs, on free root connectors only, managed by fjarr-agent ghosts.
---

- **Status**: accepted
- **Date**: 2026-09-30
- **Supersedes**: —

## Context

Robots often run their desktop with no monitor attached, and some want two or
three screens their applications lay windows out on. The M2 spikes measured two
ways to give GNOME monitors that are not there
([ADR-0006](0006-desktop-backend-selection.md), docs/07; commits 1606931 and
a1c0985):

- **Forced connectors.** The kernel's `video=<connector>:<mode>e` enables a
  connector with nothing plugged in, and `drm.edid_firmware=<connector>:edid/<file>`
  gives it an EDID. Three at once worked on the mini-PC, one of them DisplayPort.
  Backends E and C capture them like real monitors.
- **mutter virtual monitors** (`RecordVirtual`). Two at once worked. They exist
  only while a screencast session records, and their ids were unstable.

The operator also wants real monitors plugged in beside the fakes, on the same
machine, without either hiding the other. And they want to add and remove fakes
later without re-running the whole setup.

## Options considered

1. **Forced connectors for persistent screens, virtual monitors per session.**
   Pros: forced connectors exist from boot, so applications see the same layout
   whether or not anyone is connected, and a Fjarr-generated EDID gives each a
   stable monitor id. Virtual monitors still serve "an extra screen for this
   session". Cons: a forced connector needs a free physical connector and a
   reboot, and the count is bounded by the GPU's connectors.
2. **Virtual monitors only.** Pros: no kernel parameters, no reboot, no
   connector limit. Cons: they vanish when the session ends, so windows jump
   between layouts whenever an operator connects or leaves. Their ids were
   unstable, and headless means no session is running most of the time. Wrong
   for a robot whose applications own their screens.
3. **A dummy HDMI/DP plug.** Pros: no software at all. Cons: hardware per robot,
   an EDID we do not control (identity collisions between identical dongles), and
   nothing to undo or inspect remotely.

## Decision

**Option 1.** A **ghost screen** is a forced connector with a Fjarr-generated
EDID ("Fjarr Ghost N", its own serial, the requested mode). Its rules:

- **Free root connectors only.** A ghost goes on a connector whose
  `/sys/class/drm/*/status` is `disconnected` at the time it is added, and never
  on one a real monitor uses. Forcing such a connector would hide the real
  monitor's EDID behind the ghost's. DisplayPort MST branch connectors
  (`DP-1-1`…) are never ghosts: their names are created at runtime, so a
  boot-time kernel parameter cannot name them reliably. Real monitors on an MST
  chain remain fully supported; only ghosts are restricted.
- **Distinct identity.** A ghost's EDID is unique, so its monitor id (the EDID
  slug of docs/09's `MonitorIdentity`) never collides with a real monitor's, and
  it survives reboots and connector renumbering like any other.
- **Managed by `fjarr-agent ghosts list|add|remove`**, which `setup desktop
  --ghost-screens N` also uses. Each change writes a GRUB drop-in and the EDID
  firmware file, is recorded for `setup --undo desktop`, and says that it takes
  effect after a reboot. `--check` reports a configured ghost that the running
  kernel did not boot with, and a real monitor plugged into a ghost connector.
- **Virtual monitors stay a per-session option** for "one more screen while I am
  connected", with no persistence promised.

Details and the commands: [docs/26](../26-robot-install-and-drivers.md#ghost-screens).

## Consequences

- A real monitor plugged later into a ghost connector appears as the ghost: the
  kernel forces the ghost's EDID and mode on that port. `ghosts list` and setup's
  summary name the ghost connectors so an engineer knows which ports are taken.
  `--check` flags the case.
- The number of ghosts is the number of free root connectors. On a machine with
  too few, setup and `ghosts add` say how many they can make rather than failing
  after a reboot.
- Kernel parameters are the one boot-level change `setup desktop` makes. It is
  reversible, recorded and visible in `--check`, like the rest.
- Revisit if mutter gains persistent virtual monitors with stable identities.
  That would remove the reboot and the connector limit. Revisit also if a vendor
  kernel (Jetson's L4T) does not honour `video=…e` or `drm.edid_firmware`.
