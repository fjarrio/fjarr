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
| M1 Core + extension API (camera video) | **in progress** | slices 0–2 done and reviewed, 2.9 (baseline bump, [ADR-0022](adr/0022-baseline-ubuntu-2604-gstreamer-128.md)) done 2026-09-18, 3a (browser lab) 2026-09-19, 3b (agent core) 2026-09-20, 3c (introspection + memory ladder) 2026-09-20, **4 (`fjarr.camera` on real sources) 2026-09-20**; 5–7 pending |
| M4.5 Network tunnel | planned | added 2026-09-23 after the [ROS 2 tunnel spike](../agent/spikes/ros2-tunnel/README.md) ([docs/27](27-network-tunnel.md), ADR-0023, ADR-0024); depends only on M2.5, so it can be pulled earlier if a design partner asks |
| M2 – M8 | planned | revised 2026-09-17 after the slice-3 planning ([docs/23](23-agent-core-architecture.md)–[26](26-robot-install-and-drivers.md), ADR-0019–0022, the [webrtcbin spike](../agent/spikes/webrtcbin-probe/README.md), the [planning review](reviews/slice-3-planning-review.md)): new **M2.5 packaging** milestone; M3 lightened |

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
**2.9 baseline bump ✔** — dev container, CI and images on Ubuntu 26.04 /
GStreamer 1.28 ([ADR-0022](adr/0022-baseline-ubuntu-2604-gstreamer-128.md)),
doctor re-verified, the webrtcbin spike re-run on 1.28 (track removal
settled on `inactive`; answerers set `reuse-source-pads`) ·
3 in three increments ([docs/23](23-agent-core-architecture.md#slices-3a-3b-3c-and-their-gates)):
**3a ✔** the [browser lab](25-browser-lab.md) (2026-09-19: `browser`
service, `@fjarr/e2e`, wire tap, `LoopbackAgent`, frame stamp,
`fjarr-lab`, CI job; the Chromium-answerer spike report on ADR-0007) ·
**3b ✔** the agent core
(2026-09-20: `libfjarr` core loop, signaling client, session state machine,
DataChannel router, FrameHub fan-out, producers/consumers on `webrtcbin`,
the built-in `fjarr.test`, `fjarr-opsim`, the introspection walker and
minimal endpoint ([docs/24](24-pipeline-introspection.md)), real HS256
grants in the demo backend, `demo-robot` streaming the test pattern to the
demo dashboard, the ASan and RAII gates) · **3c ✔** introspection
completeness and the memory-safety ladder (2026-09-20: `/events`,
`/stats`, `/memory` + checkpoints, `/log`, the diagnostics bundle,
`fjarr-lab introspect`; TSan, the leaks-tracer bracketing and the soak as
gates, valgrind/heaptrack/netem nightly on a runner prepared for a GPU
machine; it found an upstream webrtcbin stats leak on day one —
[review](reviews/slice-3c-review.md)) ·
**4 ✔** `fjarr.camera` (2026-09-20: tracks from config through the
`SourceFactory`, the `gst`/`test`/`v4l2`/`rtsp` types, hot-plug by
renegotiation, per-track failure that never rebuilds the plane,
`--probe-source` for every type, the demo robot on a pattern, the lab's
RTSP simulator and the host webcam —
[review](reviews/slice-4-review.md)) · **5a ✔** demo wiring, protocol
half (2026-09-21, [docs/23](23-agent-core-architecture.md#slices-3a-3b-3c-and-their-gates):
docs/08 blob frames on both tiers, `fjarr.introspect`, the pipeline
feeds, `<PipelineGraph>` + the dashboard Diagnostics tab, the demo
backend's operator/developer roles —
[review](reviews/slice-5a-review.md)) · **5b ✔** demo wiring, delivery half
(2026-09-21: the viewer served from `introspect.viewer_dir`, `make
introspect` opening it with the demo's token, the CI-built `fjarr-agent`
image running the demo robot under the lab smoke — unpublished until M2.5;
[review](reviews/slice-5b-review.md)) ·
**6a ✔** repair and rate control (2026-09-22, [docs/23](23-agent-core-architecture.md#slices-3a-3b-3c-and-their-gates):
ADR-0007 closed on webrtcbin with the agent's own TWCC estimator;
NACK/RTX and keyframe feedback in the offer; banded shared-encoder
targets with per-viewer tier switching; the lab gates under the netem
profiles — [review](reviews/slice-6a-review.md)) · **6b ✔** passthrough (2026-09-22:
a camera's own H.264 untouched — no encoder for the track — with its
substream as the thumbnail tier and `adaptive: false` without one;
[review](reviews/slice-6b-review.md)) ·
7 in two increments ([docs/23](23-agent-core-architecture.md#slices-3a-3b-3c-and-their-gates)):
**7a ✔** (2026-09-23) the four robot-lifecycle fault rows nothing exercised
— a killed agent, a wedged core loop against a fake notify-socket
supervisor, a pipeline error through the escalation ladder, and an expired
or skewed grant — plus a signaling-server restart under the real agent; it
found and fixed a tier-framerate bug that stopped **any camera below 30 fps
from streaming at all**;
**7b** the glass-to-glass harness on the existing frame stamp, with
`coturn` in the `lab` profile for the relay column, a loose ceiling in CI
and the docs/16 budgets gated nightly on the prepared runner.
Input-to-photon needs a robot-side input path and lands with M3.

**Gate:** the three-demo stack end-to-end — demo-robot (embedding libfjarr)
streams 2 tracks to 3 browsers through the `fjarr-server` sidecar, the TS
demo-backend minting grants and receiving webhooks per the ADR-0015
contract; reconnect + ICE restart demonstrated under fault injection;
latency harness reporting against [budgets](16-performance-budgets.md);
TS types conform to the golden fixtures (closes open question #15);
written API-fit review for the remaining capabilities.

## M2 — Desktop backend spikes + `fjarr.terminal`

The four spikes per [docs/07](07-desktop-backends.md) close ADR-0006 with
measurements — unattended access is the hard gate; cursor metadata,
desktop-audio capture and monitor hot-plug events are measured per
backend for M3. The winning backends are built as the runtime modules of
[ADR-0021](adr/0021-desktop-backends-as-runtime-modules.md) (the module
loader lands here). In parallel, the terminal capability — the
deliberately media-free second consumer of the extension API (two
unrelated capabilities = minimum bar for "generic") and the first user of
the bulk byte channel.
**Gate:** ADR-0006 accepted with data; terminal accepted per
[docs/06](06-capabilities.md); no extension-API changes needed for it (or
the API amended + re-reviewed); a desktop backend loads as a module from
a separate package on the dev stack.

## M2.5 — Packaging & install

Everything a robot needs to *receive* the product, so M3 can ship
features into it rather than build the delivery: the apt repository and
signing, the `fjarr-agent` package with its systemd unit
(ADR-0019 addendum), `fjarr-desktop-x11` / `fjarr-desktop-wayland` /
`fjarr-inputd` / `fjarr-tools`, the container image split (base +
per-vendor layers, arm64 — the core image itself is built and tested in
CI from slice 5b and first *published* here, with the release process),
the install script, `fjarr-agent setup` / `--check`
/ `drivers`, and the driver catalog format with the built-in entries
(`test`, `v4l2`, `rtsp`, desktop backends) — [docs/26](26-robot-install-and-drivers.md).
Vendor camera packages themselves are M3 (chosen by the design partner's
hardware). Also here: the [browser lab](25-browser-lab.md) profiling
scenarios and docs/16 web budgets promoted to CI gates, and release
process + docs versioning (open question #14).
**Gate:** a fresh Ubuntu 26.04 machine goes from `curl … | sh` to a test
pattern in the dashboard with no hand-written config; `apt install
fjarr-desktop-wayland` adds remote desktop to it; the package set builds
for amd64 and arm64 in CI.

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
**Plus** the first vendor camera packages ([ADR-0020](adr/0020-vendor-sources-as-gstreamer-plugins.md))
chosen by the design partner's hardware, delivered through the M2.5
repository and catalog.
**Gate:** capability acceptance criteria ([docs/06](06-capabilities.md))
for desktop and audio; input-to-photon within budgets; the design partner
installs from the repository with `setup`, adds their camera with
`drivers install`, and no config is hand-written; the first
**design-partner demo** (docs/03 GTM).

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

## M4.5 — Direct access: the network tunnel

`fjarr.net` per [docs/27](27-network-tunnel.md): the persistent tunnel
interface created by `fjarr-agent setup`, the packet pump on
`fjarr:stream:fjarr.net`, derived addressing, the two policy rules that make
isolation structural, and the audit trail. Plus **`fjarr-connect`**
([ADR-0024](adr/0024-native-operator-client.md)), Fjarr's first non-browser
operator and its first macOS artifact, shipped in `fjarr-tools`. Plus the
documented Cyclone DDS configuration and `setup`'s offer to write it. Plus
the discovery half, without which the CLI is a debugging tool rather than a
product: the optional [operator API](09-interfaces.md#operator-api) on the
customer's backend, the `FjarrCliLogin` handoff component in `@fjarr/react`,
and both implemented in `demo-backend` and `demo-dashboard` as the
reference.

Its only hard dependency is the M2.5 packaging and installer, because the
interface must be created before the robot's software starts — **it may be
pulled ahead of M3 or M4 if a design partner needs field debugging sooner**,
and the [spike](../agent/spikes/ros2-tunnel/README.md) already de-risked the
unknowns.

**Gate:** [docs/06 `fjarr.net` criteria](06-capabilities.md) —
`ssh` and a hash-verified 1 GB `scp` to a robot behind carrier NAT; `ros2
topic list` against it with Fast DDS unconfigured and with the documented
Cyclone file; `fjarr-connect login` through the demo dashboard followed by a
list, a pick and a connect without anyone typing a robot id, and the same on
a host with no browser; two robots attached at once provably unable to reach
each other; the agent upgraded without restarting the robot's ROS stack; every
open and close in the audit log.

## M5 — Hardening + Fjarr Cloud alpha

Device enrollment/identity, multi-tenancy, ephemeral TURN at scale plus
TURNS/TCP-443 fallback, usage metering, the first OpenAPI-generated SDK
(TS), **multi-operator presence** (`session-peers` from the ownership
leases), **access governance for the tunnel** (time-boxed and
approval-gated `net` grants, per-user policy, audit export — the paid half
of [docs/03](03-product-strategy.md#tunnel-positioning)), Cloud alpha
implementing the same ADR-0015 contract as the sidecar.
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
