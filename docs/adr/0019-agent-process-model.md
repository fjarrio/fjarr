---
title: "ADR 0019: Agent process model — one process, two restartable planes"
---

- **Status**: accepted (revisit if in-process media-plane rebuild proves
  insufficient against driver hangs — measured in M2/M3)
- **Date**: 2026-09-17

## Context

docs/02 separates a **control plane** (signaling, sessions, capability
registry) from a **media plane** (GStreamer pipelines) so that the part
most likely to hit driver trouble can be torn down without dropping the
robot's connection to the server (the fleet-daemon lesson, docs/11). The
question for slice 3 is whether that separation is a *process* boundary
from day one.

## Options considered

- **Two processes** (`fjarr-agent` + `fjarr-mediad`): strongest isolation
  (a wedged VA-API driver call cannot block signaling; SIGKILL of the media
  child is always possible), at the cost of an IPC boundary carrying
  session/track state, DataChannel messages and stats, a second supervised
  unit, and a `SessionContext` that must be proxied — a lot of code before
  the first frame flows, and a boundary that every capability API call
  would cross.
- **One process, two subsystems with a rebuild seam**: the media plane is
  an object graph the control plane can dispose and recreate on a bus
  error; sessions close with a named reason, the signaling socket stays up.
  A *wedged* streaming thread (as opposed to an erroring one) is caught by
  the systemd watchdog (`WatchdogSec`) fed from the core loop, which
  restarts the whole process — the docs/15 "media plane hang" row is
  covered by supervision rather than by isolation.
- **One process, no seam**: any pipeline error is a process restart; the
  robot flaps offline on every encoder hiccup. Rejected by docs/02.

## Decision

One process, two restartable subsystems, with the media-plane interface
designed so that a `fjarr-mediad` child can implement it later without
changing the capability API ([docs/23](../23-agent-core-architecture.md#process-model)).
Supervision contract: exit 0 clean, 1 configuration error (no restart
loop), 2 "restart me" after the recovery ladder is exhausted; `READY=1` /
`WATCHDOG=1` via `sd_notify` when available.

## Addendum (2026-09-17): systemd is required on shipped robots

The packaged `fjarr-agent` (docs/26) ships a `Type=notify` unit with
`WatchdogSec=30` and `Restart=on-failure`, and the daemon **requires**
`sd_notify` support (`libsystemd`) — the watchdog is the only defense
against a wedged core loop and is not optional on a robot. Embedders of
`libfjarr` get the same behaviour through `Agent::supervision()` (a
notify/watchdog seam they may leave unset); containers without systemd
(the demo, CI) run with the watchdog off and log that fact at startup.

## Addendum (2026-09-28): READY when the robot is ready, not when the server is

`READY=1` was sent after the first `hello-ack`, so that `systemctl status`
could not call an unreachable robot fine. Two costs surfaced when
`fjarr-agent net setup` was built ([docs/26](../26-robot-install-and-drivers.md#fjarr-agent-net-setup),
[docs/18 #32](../18-open-questions.md)):

- A robot with no WAN at boot — a basement, a field, a warehouse without
  its uplink yet — never sends `READY`, so systemd kills the agent at the
  start timeout (90 s by default) and `Restart=on-failure` relaunches it,
  forever, and the watchdog never engages. Offline is a robot's normal state,
  not a failed start.
- The tunnel's ordering rule ([docs/27](../27-network-tunnel.md#lifecycle))
  needs the customer's software to start after the agent has *attached* the
  device — a Fast DDS participant created before that never sees it — and
  `After=fjarr-agent.service` waits for `READY`. With `READY` on `hello-ack`
  that made the robot's bringup wait for the WAN.

Decided: **`READY=1` once the capabilities are configured and the core loop
runs**, before any server is reached; **`STATUS=`** carries the connection
(`connecting to <url>`, `online`, `offline: <reason>`), which is the line
`systemctl status` shows, so the truth that `READY` used to deliver is still
there. The watchdog is unchanged: the core loop was always what fed it. Exit
codes are unchanged. Embedders get the seam as `Supervision::status`.

The supervision rig ([docs/23](../23-agent-core-architecture.md#testing-docs15))
now asserts the opposite of what it did: `READY` arrives with no server at
all, and a `STATUS` line says so. `net setup`'s drop-in orders the customer's
units after both `fjarr-net.service` and `fjarr-agent.service`.

## Consequences

Slice 3 ships without IPC. The measurable trigger for revisiting is a
hang that survives an in-process rebuild during the M2 desktop spikes or
M3 soak runs; if it occurs, the `fjarr-mediad` split is an ADR superseding
this one, not a redesign. Every media-plane entry point is already
asynchronous (`post_to_owner` in, callbacks out), which is the shape an
IPC boundary needs.
