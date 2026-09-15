---
title: "ADR 0016: SWUpdate OTA"
---

- **Status**: **proposed** — confirmed (or superseded) when M8 starts
- **Date**: 2026-09-15

## Context

Fleet OTA (docs/06 `fjarr.ota`) requires A/B partitioning, atomic apply,
automatic rollback, delta updates, and pre/post hooks — opinionated and
working out of the box. The in-house prior art (fleet-daemon `deploy.py`: git +
docker + two firmware flashes, no A/B, no rollback, abort-unsafe) is the
documented anti-pattern. This is a solved problem in embedded Linux; we
should adopt, not invent.

## Options considered

- **SWUpdate** (<https://sbabic.github.io/swupdate/swupdate.html>): mature,
  GPL-2.0 *tool* (runs beside, not linked into, our AGPL/commercial code —
  license-compatible as a separate process), `.swu` signed artifacts, delta
  support, handlers/hooks, suricatta backend mode we can implement
  Fjarr-side; maximum flexibility for custom partition layouts.
- RAUC: clean A/B design, similar capabilities, smaller ecosystem;
  strong contender — re-evaluate at M8.
- Mender: full platform incl. server (overlaps what Fjarr Cloud will be —
  integrating a competitor's server makes no sense; client alone possible).
- Build our own atop the file capability: exactly the wheel this ADR
  refuses to reinvent.

## Decision (proposed)

Design `fjarr.ota` around SWUpdate: Fjarr transports/orchestrates
(campaigns, accept→feedback→result, delta streaming over `fjarr.files`
backend mode), SWUpdate applies (A/B, signatures, rollback), with a
reference Ubuntu 24.04 partition layout + image recipe shipped in docs.
Final confirmation with a fresh SWUpdate-vs-RAUC review at M8 start (open
question #6).

## Consequences

We inherit a battle-tested updater and its constraints (image-based robots —
the opinionated part); the reference partition/image recipe becomes a
deliverable; hook scripts get the safety treatment (timeout-bounded,
refusable, logged) that fleet-daemon lacked.
