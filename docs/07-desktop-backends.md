---
title: Desktop Backends
description: The four-way X11/Wayland × XTest-libei/uinput evaluation that closes ADR-0006.
---

The remote desktop capability needs a **capture** path and an **injection**
path on Ubuntu 26.04. We do not pre-commit: four combinations are spiked and
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

Whichever combinations win ship as **runtime modules in separate
packages** (`fjarr-desktop-x11`, `fjarr-desktop-wayland`, plus the
privileged `fjarr-inputd`), never linked into the core
([ADR-0021](adr/0021-desktop-backends-as-runtime-modules.md)); the
`fjarr.desktop` capability reports `unavailable` with the package to
install when none matches the running display server.

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
| Cursor metadata | can the backend capture *without* the cursor and report cursor shape changes (XFixes cursor image events / PipeWire cursor metadata)? Required for local-cursor mode ([docs/22](22-remote-desktop-client.md#cursor-strategy)) |
| **Monitor hot-plug** | change notification (X11: RandR `RRScreenChangeNotify`/output events; Wayland: does the portal session expose new outputs, or must `SelectSources` re-run — and can that happen unattended with a restore token?), stable connector ids, adding/removing one capture without disturbing the others, zero-monitor and re-plug behavior ([docs/06](06-capabilities.md#fjarrdesktop--remote-desktop-m3-backend-spikes-m2)) |

## Spike protocol (M2)

**Two phases, because one criterion eliminates.** Unattended access is a
hard gate, so answering it for all four combinations is cheaper than
measuring any of them in full. Phase 1 asks only that question, with the
smallest thing that can answer it: can a rebooted machine with nobody logged
in be captured and driven at all? Phase 2 fills the criteria table, and only
for what survived. If all four survive, phase 1 cost one round trip and we
are no worse off.

**Where.** The `gpu-desktop` self-hosted runner (decided 2026-09-25): a real
machine running Xorg and GNOME that reboots under its owner's control, which
is what the gate needs and what no container can give. Xvfb in `robot-sim`
still carries everything that does not depend on a real session — latency,
multi-monitor geometry, hot-plug via RandR virtual monitors. The Wayland
combinations need a Wayland session on that machine alongside the Xorg one,
which is itself part of phase 1's answer.

> **Blocked on hardware, 2026-09-25.** Access to the `gpu-desktop` machine
> was lost, and phase 1 is the one part of this that a container cannot
> stand in for — its whole question is what a real machine does after a real
> reboot. The protocol below is complete and startable; it is waiting on a
> host, not on a decision. **Do not soften it to fit a container**: an
> unattended answer measured without an unattended machine would be worth
> less than no answer, because it would be believed.

### Phase 1 in detail (decided 2026-09-25)

The question, precisely: **after a reboot with nobody logged in, can the
agent capture the screen and inject input?** Two sub-cases per combination,
because they answer different product questions and have different answers:

| Sub-case | Setup | What it decides |
|---|---|---|
| **Appliance** | a dedicated `fjarr-spike` account with auto-login enabled | whether a robot that ships with an auto-login session can be reached — the case docs/04 says an appliance may legitimately pin |
| **Login screen** | auto-login off, nobody logged in | whether a robot can be reached *before* anyone logs in, which is what "unattended" means when the appliance trick is not available |

Both are run per combination, on the `gpu-desktop` runner, with a dedicated
account and auto-login toggled between runs. The Wayland combinations use a
GNOME-on-Wayland session on the same machine — with the caveat written down
now rather than discovered later: that machine has an **NVIDIA** GPU, and a
Wayland session there can behave differently from the Intel hardware robots
actually run, so a Wayland finding is provisional until it is seen on Intel.

**The injection oracle.** Injection must be *verified*, not assumed, without
a human watching: the session autostarts a recorder that appends what it
receives to a file, the probe injects a known sequence, and the file is read
back over ssh. A capture that produces frames proves nothing about input,
and "the call returned success" proves nothing at all.

Each run records, per combination and sub-case: yes/no, **the exact
mechanism or blocker** (a portal restore token that survived, an
`XAUTHORITY` that had to be readable, a udev rule that was needed), and what
a robot would have to ship for it to work. A "no" with a precise blocker is
a useful result; a "yes" without the mechanism is not.

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

## Simulating hot-plug

robot-sim's Xvfb runs with the RandR extension; RandR 1.5 *virtual
monitors* (`xrandr --setmonitor VIRT-2 1280/300x720/200+1920+0 none`,
`xrandr --delmonitor VIRT-2`) add and remove monitor objects at runtime
without real hardware, which is what X11 backends see on a hot-plug. The
M2 spikes verify this works on Xvfb and it becomes the docs/15 hot-plug
test fixture; the Wayland equivalent (a headless compositor with
configurable outputs) is a spike question in its own right.

## Working hypotheses (to be falsified, not trusted)

- A (X11+XTest) will win the MVP on simplicity and unattended behavior; an
  appliance can legitimately pin Xorg + auto-login ([platforms](04-supported-platforms.md)).
- C is the long-term destination; its unattended story on stock GNOME is the
  research question. If portals block, D (portal capture + uinput injection)
  may be the pragmatic Wayland bridge.
- The `DesktopBackend` interface must not leak X11 assumptions (e.g. global
  coordinates); Wayland's region/mapping model is the more general shape —
  design the interface Wayland-first, implement X11 into it.
