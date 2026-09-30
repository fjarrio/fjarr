---
title: "ADR 0033: The desktop is tested on headless mutter in CI and on shared lab machines at night"
description: Headless mutter in a container covers backend E on every push; hardware and reboot tests run on self-hosted lab machines inside a nightly CI window managed by fjarr-lab, which people use by day.
---

- **Status**: accepted
- **Date**: 2026-09-30
- **Supersedes**: —

## Context

M3 builds backend E (mutter on GNOME Wayland, [ADR-0006](0006-desktop-backend-selection.md)).
Today nothing in CI runs a Wayland compositor: robot-sim is Xvfb, which is
backend A's world. Every E measurement so far was taken by hand on the mini-PC.

Two kinds of test need two kinds of machine:

- **Compositor behaviour**: capture from a PipeWire stream, input through EIS,
  clipboard, virtual-monitor hot-plug, the helper's descriptor handover, and
  PipeWire narrowing. These need mutter but no hardware.
- **Hardware and boot behaviour**: forced connectors and ghost screens
  ([ADR-0032](0032-ghost-screens.md)), real and daisy-chained monitors, GDM
  auto-login, the unattended-access test after a real reboot (docs/15), and
  input-to-photon on real hardware. These need a real machine that CI may reboot.

The machines that can do the second kind are also people's machines. The
mini-PC is used for manual tests, and the GPU desktop (`gpu-desktop`, the M2.6
runner) is a desktop in the office by day. More will join: Raspberry Pi 4 and 5
with Pi cameras, and a Jetson with a ZED camera. CI must not take a machine from
someone using it, and a person must not break a nightly run halfway.

## Options considered

1. **Headless mutter in CI, plus the lab machines as shared self-hosted runners
   with a scheduled CI window.** Pros: backend E is exercised on every push, and
   hardware tests run every night. Machines are people's by day and CI's by night,
   and either side can override. Cons: two environments to maintain, and a
   runner-management tool to write.
2. **The mini-PC only, as a dedicated runner.** Pros: one real environment.
   Cons: backend E is tested only nightly, never on the push that broke it. The
   machine can no longer be used by hand, and the GPU desktop and later boards
   would have no path to CI.
3. **Manual testing on the lab machines.** Pros: no infrastructure. Cons:
   docs/15 requires the unattended-access test to be "scripted and kept
   forever", and a reboot test nobody runs regresses silently. That is how the
   `/run/fjarr` packaging bug reached a release in M2.5.

## Decision

**Option 1.**

- **The headless-mutter fixture** (slice 3.0): a container running
  `mutter --headless --virtual-monitor WxH` inside its own D-Bus session with
  PipeWire and WirePlumber, the session helper, and a test application to type
  into. It runs in `ci.yml` on hosted runners, on every push.
- **Lab machines** are self-hosted runners for `nightly.yml`-style workflows
  only. Those trigger on `schedule` and `workflow_dispatch` from the default
  branch, never on `push` or `pull_request`: the repository is public and a
  fork's code must never reach a machine in the office (docs/12). Workflows
  select machines by **label**, never by name.
- **`fjarr-lab`** runs on every lab machine. It is a shell script with systemd
  units, portable to arm64 boards. It gives each machine a **CI window** (for
  example 00:00–06:00), during which the runner is online, and a **manual
  reservation** that wins over the window until released. Closing the window
  never kills a job halfway: it waits a grace period, then goes offline. At boot
  it restores the right state for the time of day, so a job that reboots the
  machine gets its runner back.
- **Every lab job starts from a baseline.** It undoes the previous
  configuration, installs the build under test, and applies its own settings,
  so manual experiments and CI runs cannot poison each other.

Details: [docs/12](../12-development-environment.md#lab-machines-and-fjarr-lab)
and [docs/15](../15-testing-strategy.md#the-desktop-test-lab).

## Consequences

- **Slice 3.0 proves the fixture before anything is built on it.** mutter's
  headless mode providing RemoteDesktop and ScreenCast in a container, with no
  GPU, is expected (it renders in software) but not yet measured here. If it
  does not hold, the compositor tests move to the mini-PC's nightly run, and
  backend E is gated a day late rather than on every push.
- Backend E's compositor behaviour is gated on every push; its hardware and
  boot behaviour is gated nightly, a day late at worst.
- A job dispatched by hand during the day waits in GitHub's queue until the
  machine's window opens. `fjarr-lab status` shows it as queued.
- Nightly jobs on lab machines must fit inside the window, including reboots.
  The schedule starts shortly after the window opens.
- New hardware joins by installing `fjarr-lab`, registering the runner with its
  labels, and adding the labels to the nightly matrix. The Pi and Jetson rows
  are planned in docs/12's table now and wired when the boards arrive.
- Revisit if headless mutter stops providing RemoteDesktop/ScreenCast without a
  real GPU, or if the nightly window becomes too short for the fleet's jobs.

## Addendum (2026-09-30): the fixture's assumption, measured

The slice-3.0 spike (`spikes/desktop-e-headless/`) ran `mutter --headless
--virtual-monitor 1280x720` in a plain Ubuntu 26.04 container with no GPU. Both
RemoteDesktop and ScreenCast were on its session bus, a PipeWire frame came back
with the test window's content (100 % of the expected colour), and `ConnectToEIS`
gave a keyboard and an absolute pointer whose click, keys and position all
reached the window. The fallback is not needed. One rule for the fixture:
nothing has focus on a headless desktop until something clicks, so it focuses
its test window before keyboard tests.
