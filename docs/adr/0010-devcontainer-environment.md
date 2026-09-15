---
title: "ADR 0010: Devcontainer environment"
---

- **Status**: accepted
- **Date**: 2026-09-15

## Context

Host machines vary (the founder's host runs GStreamer 1.20 — unusable);
remote-desktop development needs a desktop to capture; CI needs the same
environment headless.

## Options considered

Native host setup (unreproducible) · devcontainer only (nothing to capture)
· **devcontainer + compose**, with an Xvfb `robot-sim` service as the
default capture target (reproducible, CI-able, watchable via noVNC) and
opt-in overrides for host-X11 capture and uinput (real-hardware feel without
baking privileges into the default path).

## Decision

The compose topology in docker-compose.yml with the self-verifying
`doctor.sh` as the environment authority (docs/12).

## Consequences

`/dev/dri` + RENDER_GID plumbing is explicit per machine (.env); everything
else is push-button. Verified working M0: HW encode + sim capture pass in
the container on the reference host.
