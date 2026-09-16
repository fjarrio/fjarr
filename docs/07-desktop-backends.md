---
title: Desktop Backends
description: The four-way X11/Wayland × XTest-libei/uinput evaluation that closes ADR-0006.
---

The remote desktop capability needs a **capture** path and an **injection**
path on Ubuntu 24.04. We do not pre-commit: four combinations are spiked and
measured in M2, closing [ADR-0006](adr/0006-desktop-backend-selection.md)
with data. All four hide behind the same `DesktopBackend` interface
([docs/09](09-interfaces.md)) so the choice is swappable per deployment.

## The candidates

| # | Capture | Injection | Sketch |
|---|---|---|---|
| A | X11 `ximagesrc` (+XDamage/XFixes cursor) | XTest | The classic; smallest code |
| B | X11 `ximagesrc` | uinput virtual devices | Injection below the display server |
| C | Wayland: ScreenCast portal → PipeWire (`pipewiresrc`, DMA-BUF) | libei via RemoteDesktop portal | The blessed modern path |
| D | Wayland: ScreenCast portal → PipeWire | uinput | Portal capture, kernel-level input |

Notes:

- Multi-monitor: X11 via XRandR geometry + per-region capture; Wayland via
  `SelectSources(multiple=true)` streams, with `mapping_id` linking capture
  streams to libei input regions — coordinates come linked for free in C.
- uinput requires a privileged helper regardless of display server
  ([ADR-0009](adr/0009-privilege-separation.md)) and `modprobe uinput` + udev
  policy at provisioning.
- Wayland's hard problem is **consent**: `RemoteDesktop.Start()` presents a
  user dialog; persistent sessions/restore tokens exist but are designed
  around an interactive user. Unattended access after reboot is the make-or-
  break question for C/D.

## Evaluation criteria (measured, per combo)

| Criterion | How measured |
|---|---|
| Unattended access after reboot | robot-sim (and one real NUC) rebooted, no local interaction; can a session start? |
| Login screen (GDM) reachability | can we see/control before any login? |
| Glass-to-glass latency | frame-stamp harness ([docs/15](15-testing-strategy.md)), p50/p95 |
| Input-to-photon latency | click → pixel change, p50/p95 |
| Multi-monitor correctness | absolute pointer lands on the right monitor at the right pixel, mixed-DPI |
| CPU/GPU cost at 1080p30 | agent process + system, vs [budgets](16-performance-budgets.md) |
| Privilege surface | what runs as root / with which caps; lines of privileged code |
| Failure modes | capture source dies, portal revoked, X restart — recovery behavior |
| Code size/complexity | LoC of the backend implementation |
| Future-proofing | upstream direction (Ubuntu is Wayland-default; Xorg maintenance reality) |
| Desktop audio capture path | PipeWire capture (Wayland) vs PulseAudio monitor source (X11): availability, latency, whether it works unattended |

## Spike protocol (M2)

Each spike is **throwaway code** in `spikes/desktop-<combo>/`, written only
after this doc is `stable`, and produces:

1. a filled-in criteria table (numbers, not adjectives);
2. a 1-page findings note appended to ADR-0006 (what surprised us);
3. a go/no-go on the unattended question with the exact mechanism
   (e.g. "portal restore token survives reboot when X/Y/Z" or "requires
   dedicated auto-login session user").

Decision rule: **unattended access is a hard gate** — a combo that cannot
reach a rebooted, nobody-logged-in robot is out for the appliance case
regardless of other scores. Among survivors, lowest operational complexity
wins; latency differences under 20 ms p50 are noise.

## Working hypotheses (to be falsified, not trusted)

- A (X11+XTest) will win the MVP on simplicity and unattended behavior; an
  appliance can legitimately pin Xorg + auto-login ([platforms](04-supported-platforms.md)).
- C is the long-term destination; its unattended story on stock GNOME is the
  research question. If portals block, D (portal capture + uinput injection)
  may be the pragmatic Wayland bridge.
- The `DesktopBackend` interface must not leak X11 assumptions (e.g. global
  coordinates); Wayland's region/mapping model is the more general shape —
  design the interface Wayland-first, implement X11 into it.
