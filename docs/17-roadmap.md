---
title: Roadmap
description: Milestones M0–M8 with entry/exit gates.
---

Milestones gate on **demonstrable behavior**, not effort. Specs referenced
must be `review`+ before implementation starts
([workflow](13-development-workflow.md)).

## M0 — Documentation & environment *(current)*

Docs 00–19 at `review`; ADRs 0001–0016 filed; devcontainer + compose +
robot-sim + doctor working; buildable skeletons for all three libraries,
three demos, website.
**Gate:** `make doctor` 0 failures; all skeletons build; demos consume only
public APIs; website previews locally rendering `docs/`.

## M0.5 — Public foundations *(as soon as a git remote exists)*

CI (lint, build, docs gates) and website deploy. Registrations done
2026-09-15: fjarr.io + fjarr.dev (Cloudflare), npm org `@fjarr`, GitHub org
`fjarrio` (`fjarr` was taken) connected to crates.io; repo at
`github.com/fjarrio/fjarr`.
**Gate:** landing + docs live at fjarr.io; CI green on main.

## M1 — Core + extension API, proven by `fjarr.camera`

Agent core (session lifecycle, FrameHub, DC router, reconnect ladder), Rust
signaling with grants/webhooks/TURN minting, `@fjarr/core` state machine +
`@fjarr/react` basics; camera capability as the reference implementation;
**ADR-0007 spike** (webrtcbin+FrameHub vs webrtcsink, measured); baseline
adaptive bitrate; extension API paper-validated against all seven
capabilities + the [arm-teach stress test](06-capabilities.md#stress-test).
**Gate:** three-demo stack end-to-end — demo-robot streams 2 tracks to 3
browsers through the sidecar, TS demo-backend minting grants and receiving
webhooks; reconnect + ICE restart demonstrated under fault injection;
latency harness reporting against [budgets](16-performance-budgets.md);
written API-fit review for the remaining capabilities.

## M2 — Desktop backend spikes + `fjarr.terminal`

The four spikes per [docs/07](07-desktop-backends.md) close ADR-0006 with
measurements (unattended access is the hard gate). In parallel, the terminal
capability — the deliberately media-free second consumer of the extension
API (two unrelated capabilities = minimum bar for "generic").
**Gate:** ADR-0006 accepted with data; terminal capability accepted per
[docs/06](06-capabilities.md); no extension-API changes needed for it (or
the API amended + re-reviewed).

## M3 — `fjarr.desktop` MVP

Video + pointer + keyboard on the chosen backend; clipboard text;
`release_all_input` safety; unattended-access test green on a real NUC.
**Gate:** capability acceptance criteria ([docs/06](06-capabilities.md));
input-to-photon within budgets; design-partner demo.

## M4 — `fjarr.files` + `fjarr.telemetry`

Chunked/backpressured/resumable transfer; telemetry with ROS 2
`TelemetrySource` adapter and store-and-forward.
**Gate:** 1 GB resume-after-kill with verified hash; bulk/interactive
isolation within budget; telemetry replay after outage.

## M5 — Hardening + Fjarr Cloud alpha

Device enrollment/identity, multi-tenancy, ephemeral TURN at scale, usage
metering, first OpenAPI-generated SDK (TS), Cloud alpha implementing the
same ADR-0015 contract; TURNS/TCP-443 fallback.
**Gate:** a design partner integrates against Cloud using only public docs;
security review of docs/10 items; metering visible in webhooks.

## M6 — Extension API stabilization

Third-party plugin authoring docs + template repo; ABI/compat promises
([docs/05](05-extension-model.md#compatibility-rules)); clipboard
images/files; multi-monitor polish.
**Gate:** an external developer (not us) builds a toy capability from docs
alone.

## M7 — Fleet observability *(flagship paid tier)*

Versions, host + user metrics, error rates; ingest + aggregation; fleet
views in `@fjarr/react`; alerting in Cloud.
**Gate:** [docs/06 observability criteria](06-capabilities.md); 10-robot sim
fleet demo.

## M8 — Fleet OTA

SWUpdate integration per [ADR-0016](adr/0016-swupdate-ota.md): A/B atomic +
rollback, delta streaming over the file capability, pre/post hooks,
campaigns with staged rollout + auto-halt; reference partition layout +
image recipe.
**Gate:** [docs/06 OTA criteria](06-capabilities.md) including the
power-cut-mid-write and failing-post-hook rollback demos.
