---
title: "ADR 0009: Privilege separation"
---

- **Status**: proposed (design accepted; refined when implemented alongside ADR-0006)
- **Date**: 2026-09-15

## Context

Input injection may require privileges (always for uinput; possibly for
login-screen scenarios). Running the network-facing agent as root is
unacceptable; fleet-daemon's arbitrary-shell-over-FIFO shows where convenience
escalation ends up.

## Options considered

Root agent (no) · capability-granting the agent binary (broad surface) ·
**separate `fjarr-inputd`**: a < 500-line target binary owning
`/dev/uinput`/XTest, speaking a 5-verb validated protocol (key, button,
motion, wheel, release_all) over a 0700 unix socket, rate-limited, refusing
without an active session claim.

## Decision

The separate helper, packaged with its own systemd unit and udev rule;
everything else runs as the unprivileged `fjarr` user.

## Consequences

One small auditable privileged surface; an IPC hop on the input path
(latency-measured in the M2 spikes); the helper ships only when the chosen
backend needs it.
