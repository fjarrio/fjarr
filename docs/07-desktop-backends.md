---
title: Desktop Backends
description: The X11/Wayland × injection evaluation that closes ADR-0006 — including the finding that stock Ubuntu 26.04 has no X11 session at all.
---

The remote desktop capability needs a **capture** path and an **injection**
path on Ubuntu 26.04. We do not pre-commit: five combinations are spiked and
measured in M2, closing [ADR-0006](adr/0006-desktop-backend-selection.md)
with data. All five hide behind the same `DesktopBackend` interface
([docs/09](09-interfaces.md)) so the choice is swappable per deployment.

**Stock Ubuntu 26.04 has no X11 session.** Its desktop is GNOME 50, which is
Wayland-only: there is no GNOME-on-Xorg session to choose, and the package that
provided one does not exist in 26.04 (checked on the spike machine, 2026-09-27).
GDM itself can still launch an Xorg session for *another* desktop
(`gdm-x-session` ships), and Xorg, its drivers, Openbox, Xfce and LightDM are all
in the archive. So X11 is a real robot configuration — a kiosk (Xorg plus a
small window manager, common on industrial machines) or a non-GNOME desktop —
but it is never the default, and a robot on stock Ubuntu is on Wayland.

## The candidates

| # | Capture | Injection | Sketch |
|---|---|---|---|
| A | X11 `ximagesrc` (+XDamage/XFixes cursor) | XTest | The classic; smallest code |
| B | X11 `ximagesrc` | uinput virtual devices | Injection below the display server |
| C | Wayland: ScreenCast portal → PipeWire (`pipewiresrc`, DMA-BUF) | libei via RemoteDesktop portal | The blessed modern path |
| D | Wayland: ScreenCast portal → PipeWire | uinput | Portal capture, kernel-level input |
| E | Wayland: mutter's own `org.gnome.Mutter.ScreenCast` → PipeWire | mutter's `org.gnome.Mutter.RemoteDesktop` (libei) | GNOME's compositor interfaces, **without the portal** — what gnome-remote-desktop uses. GNOME-only, and not a promised-stable API |

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
- E exists because GNOME already solved unattended access for itself:
  gnome-remote-desktop (installed on stock 26.04, disabled by default) reaches
  a session through mutter's D-Bus interfaces directly, and in its system mode
  creates a headless session from the login screen via GDM's remote-display
  interface. That is prior art and a candidate mechanism at once. The price is
  a GNOME-only backend on an interface GNOME may change between releases.
- A and B run in an **X11 kiosk session**, not GNOME: Openbox under GDM, and
  Xfce under LightDM (below).

Whichever combinations win ship as **runtime modules in separate
packages** (`fjarr-desktop-x11`, `fjarr-desktop-wayland`, plus the
privileged `fjarr-inputd`), never linked into the core
([ADR-0021](adr/0021-desktop-backends-as-runtime-modules.md)); the
`fjarr.desktop` capability reports `unavailable` with the package to
install when none matches the running display server.

## Evaluation criteria (measured, per combo)

| Criterion | How measured |
|---|---|
| Unattended access after reboot | the spike machine rebooted, no local interaction; can a session start? |
| Login screen reachability | can we see/control before any login? Under GDM the login screen is always Wayland, so for X11 this is only answerable under LightDM |
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

**Where.** A dedicated spike machine (from 2026-09-27): an AMD Ryzen 7 5700U
mini-PC with Radeon graphics on a fresh Ubuntu 26.04.1, kernel 7.0, GNOME 50
on GDM — the project's baseline, reached over ssh, rebooted at will. Its GPU
runs Mesa, the same driver stack as Intel, so its Wayland findings stand
without the NVIDIA caveat the lost `gpu-desktop` runner carried. Xvfb in
`robot-sim` still carries everything that does not depend on a real session —
latency, multi-monitor geometry, hot-plug via RandR virtual monitors. **Do not
soften phase 1 to fit a container**: an unattended answer measured without an
unattended machine would be worth less than no answer, because it would be
believed.

### Phase 1 in detail (decided 2026-09-25)

The question, precisely: **after a reboot with nobody logged in, can the
agent capture the screen and inject input?** Two sub-cases per combination,
because they answer different product questions and have different answers:

| Sub-case | Setup | What it decides |
|---|---|---|
| **Appliance** | a dedicated `fjarr-spike` account with auto-login enabled | whether a robot that ships with an auto-login session can be reached — the case docs/04 says an appliance may legitimately pin |
| **Login screen** | auto-login off, nobody logged in | whether a robot can be reached *before* anyone logs in, which is what "unattended" means when the appliance trick is not available |

Both are run per combination, with the dedicated account and auto-login
toggled between runs, in three sessions. The GDM steps come first because
they share a login manager; LightDM replaces GDM, so it runs last and GDM is
restored after it:

| Step | Session | Candidates | Login-screen sub-case |
|---|---|---|---|
| 1 | GNOME on Wayland, GDM (stock) | C, D, E | measured — the greeter is Wayland |
| 2 | Openbox on Xorg, under GDM | A, B | **structurally no**: GDM's greeter is Wayland, so an X11 backend cannot reach it. Recorded as a result of the architecture, not skipped |
| 3 | Xfce on Xorg, under LightDM | A, B | measured — LightDM's greeter runs on X11; this is the one setup where X11 can reach "before anyone logs in" |

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

## Decision (2026-09-27)

[ADR-0006](adr/0006-desktop-backend-selection.md) is accepted on the spikes'
measurements. **E** is the backend for stock Ubuntu, **C** is second (for
non-GNOME compositors and as E's fallback), and **A** is for X11 kiosks. D is
dropped and B is not built. Headless robots get a forced connector. The
hypotheses below are kept as they were written.

## Working hypotheses (to be falsified, not trusted)

- ~~A (X11+XTest) will win the MVP on simplicity and unattended behavior.~~
  **Falsified as written, 2026-09-27**: it assumed stock Ubuntu offered an X11
  session, and 26.04 does not. A remains the likely answer for robots that
  ship an X11 kiosk; it is not an answer for a robot on stock Ubuntu.
- On stock Ubuntu the question is therefore Wayland's unattended story. C is
  the standards-based destination and its consent model is the risk; **E is the
  most likely unattended answer on GNOME** because GNOME's own remote desktop
  already depends on it; D (portal capture + uinput injection) is the bridge if
  the portal allows capture but not input.
- The `DesktopBackend` interface must not leak X11 assumptions (e.g. global
  coordinates); Wayland's region/mapping model is the more general shape —
  design the interface Wayland-first, implement X11 into it.
