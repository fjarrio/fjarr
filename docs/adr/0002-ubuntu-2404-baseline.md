---
title: "ADR 0002: Ubuntu 24.04 baseline"
---

- **Status**: accepted
- **Date**: 2026-09-15

## Context

The agent needs `vah264enc` (VA-API encode), usable libei, PipeWire 1.0, and
current `webrtcbin`. Ubuntu 22.04 ships GStreamer 1.20 (none of the above in
usable form); 24.04 ships 1.24.2 with everything from distro packages
(verified in the M0 doctor).

## Options considered

22.04 (fleet inertia, obsolete stack) · 24.04 (everything needed, LTS until
2029) · 24.04-primary + 22.04-legacy matrix (double CI, degraded feature
story).

## Decision

Ubuntu 24.04 LTS x86-64 is the only supported agent baseline; robots
upgrade. No 22.04 compatibility work.

## Consequences

One test matrix; modern APIs assumed everywhere (libei, PipeWire). Jetson
(L4T) support later is an explicit adapter effort, not a baseline change.
