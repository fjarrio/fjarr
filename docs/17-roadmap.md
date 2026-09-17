---
title: Roadmap
description: Milestones M0–M8 with status, scope, and entry/exit gates.
---

Milestones gate on **demonstrable behavior**, not effort. Specs referenced
must be `review`+ before implementation starts
([workflow](13-development-workflow.md)). Milestone numbers are stable
anchors used across docs, ADRs and open questions; scope moves between them
by revision (this document's history), never by renumbering.

## Status

| Milestone | Status | Notes |
|---|---|---|
| M0 Docs & environment | **done** 2026-09-15 | doctor 0 failures, 3 tiers + 3 demos build, site builds |
| M0.5 Public foundations | **done** 2026-09-16 | CI green, fjarr.io + fjarr.dev live, registrations, prior-art anonymization policy |
| M1 Core + extension API (camera video) | **in progress** | slices 0–1 done (protocol + API-fit review; Rust signaling, reviewed); slices 2–7 pending |
| M2 – M8 | planned | revised 2026-09-16 after the slice-2 design ([docs/21](21-web-client-architecture.md), [docs/22](22-remote-desktop-client.md)); slice-3 planning 2026-09-17: [docs/23](23-agent-core-architecture.md) agent core, [docs/24](24-pipeline-introspection.md) introspection, [docs/25](25-browser-lab.md) browser lab, [ADR-0019](adr/0019-agent-process-model.md), the [webrtcbin spike](../agent/spikes/webrtcbin-probe/README.md) |

## M0 — Documentation & environment *(done)*

Docs 00–19 at `review`; ADRs 0001–0016 filed; devcontainer + compose +
robot-sim + doctor working; buildable skeletons for all three libraries,
three demos, website.

## M0.5 — Public foundations *(done)*

CI (lint, build, docs gates), website deployed via Cloudflare Workers
Builds, registrations (fjarr.io + fjarr.dev, npm org `@fjarr`, GitHub org
`fjarrio` connected to crates.io). Prior-art sources anonymized across the
tree, history and site ([docs/11](11-prior-art.md)).

## M1 — Core + extension API, proven by `fjarr.camera` *(in progress)*

**Scope.** Agent core (session lifecycle, FrameHub, DC router, reconnect
ladder; libsoup-3 signaling per ADR-0017); Rust signaling with grants,
webhooks, TURN minting (done, slice 1); the web core per
[docs/21](21-web-client-architecture.md): client-owned multi-session
handles, the three subscription modes, the publish side (send / request /
newest-wins publishers with deadman / ordered channels / bulk), demand-driven
track delivery with visibility, the stats & health sampler, and the
capability-specific core hooks [docs/22](22-remote-desktop-client.md#core-requirements-for-slice-2-so-m3-needs-no-core-change)
requires (page-level focus registry, `preference`/`latencyMode` acquire
options, `fjarr.core/time-sync`). Camera capability as the reference
implementation; the ADR-0007 webrtcbin-vs-webrtcsink spike; baseline
adaptive bitrate. Extension API paper-validated — **done**:
[M1 API-fit review](reviews/m1-api-fit-review.md).

**Slices** (each lands on `main`, reviewed retrospectively — docs/20):
0 protocol + review ✔ · 1 Rust signaling ✔ · 2 web core + React ✔
([review](reviews/slice-2-review.md)) ·
3 agent core (C++, per [docs/23](23-agent-core-architecture.md) — ships
with a built-in `fjarr.test` capability and the `fjarr-opsim` operator
simulator so the core is proven end-to-end before any real capability,
plus the pipeline-introspection walker, local endpoint and viewer of
[docs/24](24-pipeline-introspection.md), and — as its first task, on the
web side — the [browser lab](25-browser-lab.md) (CDP Chromium in compose,
Playwright/CDP harness, loopback agent, `fjarr-lab`); gate in
[docs/23](23-agent-core-architecture.md#slice-3-gate)) ·
4 `fjarr.camera` · 5 demo wiring (incl. `fjarr.introspect` +
`<PipelineGraph>`) · 6 ADR-0007 spike + adaptive bitrate ·
7 fault injection + latency harness.

**Gate:** the three-demo stack end-to-end — demo-robot (embedding libfjarr)
streams 2 tracks to 3 browsers through the `fjarr-server` sidecar, the TS
demo-backend minting grants and receiving webhooks per the ADR-0015
contract; reconnect + ICE restart demonstrated under fault injection;
latency harness reporting against [budgets](16-performance-budgets.md);
TS types conform to the golden fixtures (closes open question #15);
written API-fit review for the remaining capabilities.

## M2 — Desktop backend spikes + `fjarr.terminal`

The four spikes per [docs/07](07-desktop-backends.md) close ADR-0006 with
measurements — unattended access is the hard gate; cursor metadata and
desktop-audio capture are measured per backend for M3. In parallel, the
terminal capability — the deliberately media-free second consumer of the
extension API (two unrelated capabilities = minimum bar for "generic").
**Gate:** ADR-0006 accepted with data; terminal accepted per
[docs/06](06-capabilities.md); no extension-API changes needed for it (or
the API amended + re-reviewed).

## M3 — See, control and hear the robot

`fjarr.desktop` MVP per [docs/22](22-remote-desktop-client.md): video +
pointer + keyboard with the full input pipeline (focus model,
browser-reserved shortcuts + Keyboard Lock, no-auto-repeat, composed text,
client-side release-all), local-cursor mode where the backend allows,
`sharpness` preference and `latencyMode: interactive`, clipboard text;
`release_all_input` safety; unattended-access test green on a real NUC;
**presentation mode** (multi-monitor fullscreen, one window per monitor)
with the **portal-vs-route spike** that fixes the default per browser
(open question #19).
**Plus `fjarr.audio`** (robot microphone downlink, push-to-talk uplink via
the pre-allocated transceiver, audited) and **desktop audio** on
`fjarr.desktop`.
**Plus packaging** per [docs/26](26-robot-install-and-drivers.md): the apt
repository, `fjarr-agent setup` / `drivers`, the driver catalog, the
desktop backend packages (ADR-0021) and the first vendor camera packages
(ADR-0020) chosen by the design partner's hardware.
**Gate:** capability acceptance criteria ([docs/06](06-capabilities.md))
for desktop and audio; input-to-photon within budgets; the design partner
installs from the repository with `setup` and no hand-written config; the
first **design-partner demo** (docs/03 GTM).

## M4 — Files, telemetry, logs, sensors

`fjarr.files` (chunked/backpressure/resume — greenfield); `fjarr.telemetry`
with the ROS 2 `TelemetrySource` adapter and store-and-forward;
**`fjarr.logs` peer mode** (live tailing with server-side filters and
backpressure — the support tool design partners will ask for); the
**sensor-transport spike** on a simulated depth/lidar source that settles
depth-as-video vs stream class per the [docs/06 matrix](06-capabilities.md#sensor-transport)
(the ADR-0018 chunker/reassembler is implemented only when a capability
actually picks the stream class — docs/13 KISS); the cheap conventions:
`camera/snapshot`, `telemetry/alert`, `files/list`, haptic events.
**Gate:** 1 GB resume-after-kill with verified hash; bulk/interactive
isolation within budget; telemetry replay after outage; logs tail from
robot-sim with a filter; spike report attached to open question #16.

## M5 — Hardening + Fjarr Cloud alpha

Device enrollment/identity, multi-tenancy, ephemeral TURN at scale plus
TURNS/TCP-443 fallback, usage metering, the first OpenAPI-generated SDK
(TS), **multi-operator presence** (`session-peers` from the ownership
leases), Cloud alpha implementing the same ADR-0015 contract as the
sidecar.
**Gate:** a design partner integrates against Cloud using only public docs;
security review of docs/10 items; metering visible in webhooks; two
operators on one robot see each other and hand over control.

## M6 — Extension API stabilization

Third-party plugin authoring docs + template repo; ABI/compat promises
([docs/05](05-extension-model.md#compatibility-rules)); clipboard
images/files; multi-monitor polish.
**Gate:** an external developer (not us) builds a toy capability from docs
alone.

## M7 — Fleet observability *(flagship paid tier)*

Versions, host + user metrics, error rates; ingest + aggregation; fleet
views in `@fjarr/react`; alerting in Cloud; **`fjarr.logs` backend mode**
feeding the same store.
**Gate:** [docs/06 observability criteria](06-capabilities.md); 10-robot sim
fleet demo.

## M8 — Fleet OTA

SWUpdate integration per [ADR-0016](adr/0016-swupdate-ota.md): A/B atomic +
rollback, delta streaming over the file capability, pre/post hooks,
campaigns with staged rollout + auto-halt; reference partition layout +
image recipe.
**Gate:** [docs/06 OTA criteria](06-capabilities.md) including the
power-cut-mid-write and failing-post-hook rollback demos.

## Explicitly later

Session recording/replay, mobile operator apps — [open questions](18-open-questions.md).
