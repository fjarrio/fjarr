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
| M1 Core + extension API (camera video) | **done** 2026-09-25 | slices 0–2 done and reviewed, 2.9 (baseline bump, [ADR-0022](adr/0022-baseline-ubuntu-2604-gstreamer-128.md)) done 2026-09-18, 3a (browser lab) 2026-09-19, 3b (agent core) 2026-09-20, 3c (introspection + memory ladder) 2026-09-20, **4 (`fjarr.camera` on real sources) 2026-09-20**, 5a/5b (blob frames, `fjarr.introspect`, the served viewer) 2026-09-21, 6a/6b (repair + rate control, passthrough) 2026-09-22, **7a/7b (the remaining fault rows, the latency harness) 2026-09-23/24 — every slice landed**; the [gate review](reviews/m1-gate-review.md) found the ADR-0015 webhook half had never run and that standard-form TURN URLs were silently ignored by the agent, and fixed both. **Gate met 2026-09-25**: CI green and the first fully green nightly on the self-hosted runner, which also recorded the first trustworthy latency numbers — clean 56/79 ms, lossy 82/100, relay 69/102 against budgets of 120/200, 200/350 and 250/450, at 46–52 decoded fps |
| M2.6 Encoder families | planned | added 2026-09-24 ([ADR-0025](adr/0025-encoder-families.md)): `nvcodec` and `nvv4l2` behind the existing adapter; closes open question #2 |
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
**7b ✔** (2026-09-24) the glass-to-glass harness on the existing frame
stamp: `coturn` in the `lab` profile so the relay column exists and is
asserted to be genuinely relayed, p50/p95 and time-to-first-frame under
clean/lossy/relay, a loose ceiling in CI with the docs/16 budgets behind
`E2E_LATENCY_STRICT` for the prepared runner, and `make latency` recording
a labelled row that carries its own decoded `fps` so a CPU-limited machine
cannot be mistaken for a slow network. Input-to-photon needs a robot-side
input path and lands with M3.

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
**Slices** (planned 2026-09-25; the three deliverables are unusually
independent, so they land in this order for the reason each gives):

- **2a ✔** (2026-09-25) **`fjarr.terminal`.** The pty over the bulk byte
  channel in both directions ([docs/08](08-protocol.md#terminal)), the xterm
  component, the ownership lease, the audit, and the config that makes it
  exist at all ([docs/06](06-capabilities.md),
  [docs/10](10-security.md#terminal)). **It did what it was scheduled to
  do**: the extension API needed one amendment, `watch_readable`, because a
  pty is a file descriptor and nothing in the interface could serve one
  without breaking the single-loop rule ([docs/09](09-interfaces.md)).
  Measured: an interactive round trip of **11 ms** against a 150 ms budget.
  First because it needs no hardware and no display server, and because an
  extension API with one consumer is not proven generic — it is camera video
  with extra structure. If the API is wrong, this is the cheapest moment to
  find out. *Gate:* interactive round trip < 150 ms on LAN; a disconnect
  leaves no orphan shell; every open and close audited; **no extension-API
  change was needed**, or the API is amended and the fit review re-run.
- **2b — the desktop spikes and ADR-0006.** Two phases
  ([docs/07](07-desktop-backends.md#spike-protocol-m2)): unattended access
  for all four combinations first, since it eliminates; then the full
  criteria table for the survivors. On the `gpu-desktop` runner, because a
  rebooted machine with nobody logged in is the one thing a container cannot
  simulate. Phase 1 is specified to the point of being startable: a
  dedicated `fjarr-spike` account, auto-login toggled between runs, both the
  appliance and login-screen sub-cases, and an injection oracle that records
  what actually arrived rather than trusting a return value. Its one
  recorded caveat is that Wayland findings on an NVIDIA machine are
  provisional until seen on Intel. *Gate:* ADR-0006 accepted with numbers,
  not adjectives, and a go/no-go on unattended access naming the exact
  mechanism.
- **2b is blocked on hardware** (2026-09-25): the machine it needed is no
  longer available, and its question — what a real machine does after a real
  reboot — is precisely the one no container answers. The protocol is
  written and startable; it waits on a host, not on a decision. **2c runs
  first instead**, which changes nothing about it: the loader is
  backend-agnostic, so it does not depend on which combination wins.
- **2c ✔** (2026-09-25) **the module loader.** A backend loads at runtime
  from a separate package
  ([ADR-0021](adr/0021-desktop-backends-as-runtime-modules.md)) and the
  always-present `fjarr.desktop` capability reports `unavailable` **with the
  package to install** when none matches — a robot with no desktop still
  passes `--check`, because that is the normal case. The seam's version is
  in the exported symbol's name, so a module built against an older one is
  not found rather than loaded and misread. *Gate met:* the loader creates a
  backend from a separate `.so` on the dev stack, and the doctor prints a
  desktop row either way.

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

## M2.6 — The encoder beyond Intel

`EncoderAdapter`'s second and third implementations per
[ADR-0025](adr/0025-encoder-families.md): the `nvcodec` family for discrete
NVIDIA cards and `nvv4l2` for Jetson, with the `auto` preference order, the
memory-path rule, and the live-bitrate declaration that keeps rate control
honest on a family that cannot take one. Plus the doctor probe per family,
the platform matrix's runner column, and the container plumbing that gives
the dev image a GPU (`NVIDIA_DRIVER_CAPABILITIES`, which is why
`gst-inspect-1.0 nvcodec` reported zero features the first time it was
tried).

Here rather than post-M6 for two reasons. A seam with one hardware
implementation is not proven generic, and this is the cheapest moment to
find that out. And M2.5 already builds arm64 images and owns the driver
catalog a Jetson package belongs in, so doing this next avoids opening the
same boxes twice. `nvcodec` goes first because the hardware exists and
everything below Fjarr is verified on it; `nvv4l2` follows when a board does.

**Gate:** a camera streams through `nvcodec` on the `gpu-desktop` runner
with no CPU colour conversion in the pipeline graph, rate control moves its
bitrate while playing, `auto` picks VA-API on a machine that has both, the
doctor names the family it chose and why, and the nightly runs the media
suites on that runner. Jetson's row in the matrix stays **untested** until a
board is in CI, per ADR-0025.

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

**Pulled forward (planned 2026-09-25, re-split 2026-09-26).** The installer is
the only part that needs packaging; in the lab, compose creates the device the
same way the installer will, so everything else is buildable and testable
now. The plan was two slices; after 4.5a landed, what remained was too large
for one — six gate items, a third WebRTC implementation, and a lab that cannot
yet run `ssh`, `scp` or `ros2` at all. Split into five, each sized for one
session and ordered so every slice is provable when it lands:

- **4.5a — the agent side. Done 2026-09-25.** The core could not route
  inbound stream-class data at all: it assumed every binary channel was a
  bulk one, so a tunnel packet was cut at the wrong offset, matched no
  capability, and was dropped and counted. That came first — one parse of the
  label for both classes, a sender for the stream class, and registration
  refusing a `framed` stream channel so ADR-0018's chunker stays unbuilt
  until something needs it. Then `fjarr.net`: attaching to the persistent
  device with no `CAP_NET_ADMIN`, the packet pump, derived addressing,
  the two policy rules, tail-drop, and `link-stats` — which needed the
  extension API's **second amendment**, `SessionContext::every`, because the
  event is specified per second on an idle link too. `make tun-up` plays the
  installer in the lab, and `fjarr-opsim --scenario tunnel` carries an HTTP
  request to the robot's own introspection endpoint over the link. *Gate
  met:* `GET http://<robot tunnel address>:7381/stats -> 200` over a real
  data channel; packets addressed into the robot's LAN refused and counted;
  tail-drop unit-tested against a refusing channel, including that what was
  dropped is gone rather than queued.
- **4.5b — gates worth trusting.** Not tunnel work: two suites are red on the
  development machine while green in CI, and the next slices measure media.
  `opsim hotplug` fails deterministically when it runs *after* `smoke` in
  `opsim-all` and passes standalone — determinism that specific is usually
  findable. `ratecontrol.spec.ts` reports 1.1 Mbps on a clean link where it
  asserts 3, with VA-API active and no qdisc left behind. Both reproduce with
  4.5a's changes stashed, so neither is the tunnel's. This comes first because
  [question #23](18-open-questions.md) — whether a `scp` starves the camera —
  is answered by reading local media numbers, and numbers from a suite that
  fails for unknown reasons answer nothing. *Gate:* both green locally, or the
  cause understood and the assertion made honest about it — the precedent is
  `opsim netem wifi-ok`, which records loss rather than asserting it because
  the receiver over-reports under jitter.
- **4.5c — the rig, and the tools the gate names.** The lab can carry packets
  and nothing else: there is no `sshd` in the robot image, no ROS 2 anywhere
  in the stack, and one robot. All three are prerequisites for the M4.5 gate,
  and none of them needs `fjarr-connect` — `fjarr-opsim` already pumps a
  tunnel, so the robot side and the rig can be proven before the Rust client
  exists, exactly as 4.5a proved the capability before the client. `sshd` goes
  in the robot (forwarding is off, so the server must sit on the robot's own
  tunnel address); a second `demo-robot` with its own id; ROS 2 as sidecars
  with `network_mode: service:<end>`, which see `fjarr0` in the shared
  namespace without putting ROS 2 in the agent image. Plus the documented
  Cyclone DDS file and `fjarr-agent net setup`'s offer to write it. *Gate:*
  `ssh` login and a hash-verified 1 GB `scp` over the link; `ros2 topic list`
  with Fast DDS unconfigured and with the Cyclone file; the three ordering
  facts of [docs/27](27-network-tunnel.md#lifecycle) as a regression — a
  participant created while the agent is detached does not advertise the
  tunnel address, one created while attached does, and it keeps advertising
  across an agent restart; and **question #23 measured**, a camera streaming
  while the `scp` runs.
- **4.5d — `fjarr-protocol` and `fjarr-connect`**
  ([ADR-0024](adr/0024-native-operator-client.md)). The shared signaling types
  move into their own crate **here**, when a second consumer exists: the
  protocol module is 217 lines needing only serde, while the signaling crate
  carries axum, hyper and the HMAC stack that an operator CLI has no business
  linking. Then the client itself — signaling, one webrtc-rs peer connection,
  one stream channel, no media at all, the TUN/utun device with a /32 route
  per attached robot, `--grant <jwt>`, a link that lives in the terminal that
  started it, and `-- <command>` with `FJARR_ADDR` in its environment. Several
  robots at once is what the single-interface design exists for, so address
  collision detection and the **two-robot isolation regression** (docs/15
  safety class, never removed) land with it rather than before. *Gate:* the
  4.5c end-to-end repeated through `fjarr-connect` instead of `fjarr-opsim`;
  two robots attached at once provably unable to reach each other in either
  direction; a colliding pair refused by name with the `address` line that
  fixes it.
- **4.5e — discovery and login.** Without this the CLI is a debugging tool
  rather than a product: the optional
  [operator API](09-interfaces.md#operator-api) on the customer's backend, the
  `FjarrCliLogin` handoff component in `@fjarr/react`
  ([docs/21](21-web-client-architecture.md#cli-login)), both implemented in
  `demo-backend` and `demo-dashboard` as the reference, and the CLI's own
  `login`, `list`, picker, `--ssh-config` and the short-code path for a host
  with no browser. *Gate:* `fjarr-connect login` through the demo dashboard,
  then a list, a pick and a connect with nobody typing a robot id; the same on
  a host with no browser; every open and close in the audit log as
  `session.started`/`session.ended` with `fjarr.net` among the capabilities.

macOS is ADR-0024's committed second platform and there is no macOS runner, so
4.5d ships it cross-compiled and hand-checked, with the matrix honest about
that until M5's packaging work says otherwise.

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
