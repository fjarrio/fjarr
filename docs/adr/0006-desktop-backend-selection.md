---
title: "ADR 0006: Desktop backend selection"
---

- **Status**: **proposed** — closed by the M2 spikes
- **Date**: 2026-09-15

## Context

Remote desktop on Ubuntu 26.04 has two capture paths (X11 `ximagesrc`,
Wayland portals/PipeWire) and three injection paths (XTest, libei, uinput).
Unattended access after reboot is the make-or-break industrial requirement,
and the honest answer for Wayland portals is unknown until tested.

**Revised 2026-09-27**, on the spike machine: stock Ubuntu 26.04 ships GNOME 50,
which has **no X11 session**. X11 exists only in a kiosk or non-GNOME desktop a
robot chooses to run, and under GDM the login screen is always Wayland. GNOME's
own remote desktop reaches sessions through mutter's D-Bus interfaces without the
portal, which adds a fifth option.

## Options considered

Five combos (A: X11+XTest, B: X11+uinput, C: portals+libei, D:
portals+uinput, E: mutter's own ScreenCast/RemoteDesktop interfaces) — criteria,
spike protocol, and the decision rule
(unattended access is a hard gate; then lowest operational complexity;
<20 ms p50 latency differences are noise) are specified in docs/07.

## Decision

Deferred to evidence. The `DesktopBackend` interface (docs/09) is designed
Wayland-first so whichever combo wins is swappable per deployment.

## Consequences

M2 carries four small throwaway spikes; this ADR gains a findings appendix
per spike and flips to accepted with the data attached.
