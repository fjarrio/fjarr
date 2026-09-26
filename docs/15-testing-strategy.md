---
title: Testing Strategy
description: Test pyramid, fault injection, the latency harness, and safety behaviors.
---

Connectivity software fails in the field through *network weather, process
death, and time* — so those are first-class test inputs here, not
afterthoughts.

## Pyramid

| Layer | Tools | What it proves |
|---|---|---|
| Unit | GoogleTest (C++), `cargo test`, Vitest | pure logic: envelope codecs, backoff math, range/resume bookkeeping, state machines. The translation-boundary style ([prior art](11-prior-art.md#fleet-daemon)) keeps this layer big and cheap |
| Component | same + fakes | capability against a fake core; DesktopBackend against Xvfb; signaling router against an in-process client |
| Integration | docker compose, headless Chromium (Playwright) | agent ↔ fjarr-server ↔ browser, real WebRTC, against robot-sim |
| End-to-end / demo | the `demo` profile | the three-demo stack **is** the e2e suite — demos consuming only public APIs means passing demos prove the customer path |
| Soak | nightly (M1+) | 24 h session churn + transfer loops; memory/fd leak watch |

Protocol conformance: golden envelope fixtures generated from
`protocol/schemas/` are replayed against all three implementations — drift
fails the build ([docs/08](08-protocol.md#versioning)).

## Fault injection {#fault-injection}

A **fault-injecting mock backend/peer is a first-class artifact** (the fleet-daemon
mock's best idea), scripted in integration tests. The web side ships it as
`@fjarr/core/testing` (`MockAgent`: fake signaling socket + fake peer
connection, every fault below as a method) so host dashboards test their
own integration without a browser or a robot; the C++ side ships
`fjarr-opsim` (docs/23) and the [browser lab](25-browser-lab.md) applies
the network rows below as named profiles on both the browser and the media
path. Minimum fault menu:

Rows are implemented as the capability that makes them meaningful arrives;
the four robot-lifecycle rows land in **slice 7a**, the mid-transfer kill
with `fjarr.files` in M4, and monitor hot-plug with `fjarr.desktop` in M3.

| Fault | Expected behavior |
|---|---|
| Signaling socket killed | reconnect with the spec backoff; ICE restart; session resumes or closes cleanly |
| **Server goes silent without disconnecting** | heartbeat detects within budget; teardown + reconnect (the nastiest real-world case) |
| Packet loss 5–20 % on media path (netem) | video degrades, never stalls > budget; input stays responsive |
| Relay-only forced (`iceTransportPolicy: relay`) | everything works through coturn; budgets per docs/16 relay column |
| Agent SIGKILL mid-session | operator sees `peer-gone` ≤ heartbeat budget; supervised restart; robot reachable again < 30 s |
| Pipeline error (encoder reset, capture death — injected via the `fjarr.test` hooks or `GST_DEBUG` fault points) | producer restart with backoff, then a media-plane rebuild: sessions close with `media-restart` (`retry:true`), the signaling socket stays up, the robot stays online ([ADR-0019](adr/0019-agent-process-model.md)) |
| Whole-process hang (SIGSTOP) | the systemd watchdog kills and restarts the process (no in-process defense against a stopped process); operators see `peer-gone`; ownership lease expires (fail-open); robot back < 30 s |
| Mid-transfer network kill | file resume from received ranges; hash verifies |
| Monitor hot-plug during a session (`xrandr --setmonitor`/`--delmonitor` on robot-sim) | renegotiation adds/removes the track; other monitors' frame counters never stall; re-plug restores the same `track_id`; zero monitors then one recovers without reconnect |
| Grant expired / clock skew | clean `grant-expired`, no retry storm (fatal vs retryable taxonomy) |

## Latency harness (glass-to-glass) {#latency-harness}

From M1, a measurement rig — not vibes:

- **Glass-to-glass**: the agent draws a **machine-readable frame stamp**
  (frame counter + sender timestamp as a block pattern — no OCR) on the
  test pattern / a stamped camera track; the [browser lab](25-browser-lab.md)
  reads it per decoded frame via `requestVideoFrameCallback`; Δ = g2g.
  Report p50/p95 under clean, lossy, and relay conditions (the lab's
  network profiles).
- **Input-to-photon**: synthetic click → screen change at a known pixel →
  time to that change appearing in the received stream.
- **Two gates, because the budgets are NUC-class and CI is not.** Shared
  runners with a software encoder cannot hold a p95 honestly, and a gate
  people learn to re-run is worse than no gate. So the CI job fails only
  above a **loose ceiling** — gross regression, not budget — while the
  **nightly job on the prepared runner** gates the
  [docs/16](16-performance-budgets.md) numbers themselves.
- Results append to `web/e2e/latency.csv` — but **only a labelled run
  records** (`make latency LABEL=<hardware>`, or the nightly job with the
  label variable set). The label is mandatory because rows from different
  machines must never be compared. Nothing commits a row automatically: CI
  and the nightly upload theirs as a run artifact, since a job that pushes to
  the repository every night is noise, not history.
- **Every row carries its own trust marker.** A machine that cannot encode
  the source rate is measuring its own CPU, not the path, so the harness
  records the decoded `fps` beside the percentiles and flags a run below 60 %
  of the source rate as CPU-limited. Strict mode *refuses* such a run rather
  than holding it to a budget it was never measuring.
- Input-to-photon needs a robot-side input path and therefore arrives with
  `fjarr.desktop` in M3; glass-to-glass lands in slice 7b
  ([docs/23](23-agent-core-architecture.md#slices-3a-3b-3c-and-their-gates)).
  Together they gate the ADR-0006 decision.

## Safety behaviors {#safety-behaviors}

Learned the hard way (the teleop car's silently-regressed deadman): every safety
behavior is **spec'd, implemented, and covered by a test that fails when it
regresses**:

- input silence deadman on actuation-bearing channels;
- `release_all_input()` on any session end (no stuck modifiers — test:
  disconnect mid-keydown, assert keyup injected);
- ownership lease expiry (fail-open) under media-plane hang;
- heartbeat teardown timings;
- **tunnel isolation** ([docs/27](27-network-tunnel.md#testing)): with two
  robots attached at once, no packet crosses from one link to the other in
  either direction, and each end drops anything not addressed to its own
  tunnel address. Isolation is the property customers will ask about, so it
  is a test that fails loudly, not a configuration note. The single-ended
  half of it ships with the agent (slice 4.5a): `test_net.cpp` pins both
  rules in both directions, and the `tunnel` opsim scenario proves the robot
  refuses a packet aimed into its LAN over a real data channel. The
  two-robot half needs two operator ends and lands with `fjarr-connect`.
- **tunnel interface ordering**: a DDS participant created while the agent is
  detached must not advertise the tunnel address; created while attached it
  must; and it must keep advertising across an agent restart. These pin the
  three measured facts the tunnel lifecycle rests on — the ordering requirement
  in [docs/27](27-network-tunnel.md#lifecycle), and therefore the installer's
  job and the systemd unit's ordering, is built on nothing else. Implemented in
  slice 4.5d as `make tunnel-ros-ordering`, against real ROS 2 participants on
  both ends of a real link. The agent runs under a supervisor in the lab so it
  can be restarted the way systemd restarts it, leaving the interface in place;
  restarting the container instead would take the device with it and the third
  fact would be untestable.

## The robot's log is a gate {#log-gate}

`make agent-log-gate` fails a run whose robot logged a GLib or GStreamer
**CRITICAL**, or a failed assertion. Those mean a refcount or an invariant was
already violated, and a suite can pass straight through one: a double-released
fd watch printed three criticals per lab job for weeks while every test stayed
green, and was found by reading a log for an unrelated failure. Plain warnings
are deliberately not fatal — GStreamer emits benign ones — so the gate stays
worth obeying.

`make agent-log-gate-selftest` feeds it that same historical line and fails if
the gate stays quiet, because a gate that cannot fire is no gate. The same slice
found `opsim-all`'s crash retry had never once fired for the same reason
([docs/17](17-roadmap.md)), so every gate that tolerates something now has to
show it can still refuse.

## Memory safety (C++) {#memory-safety-c}

The agent wraps a C object system, so lifetime bugs get their own ladder
([docs/23](23-agent-core-architecture.md#memory-and-lifetime-discipline-and-the-tooling-that-enforces-it)):

| Layer | Tool | When | Gate? |
|---|---|---|---|
| RAII kit only touches refcounts | grep gate in `/verify`, clang-tidy `-Werror` | every commit | yes |
| Address/Undefined/Leak | `asan` preset (`-fsanitize=address,undefined`, UB non-recoverable, LSan with `lsan.supp`) on unit + loop tests | every commit (CI) | yes |
| Data races in the threading model | `tsan` preset (+ `tsan.supp` for the uninstrumented GLib/GStreamer modules; the RAII kit's hand-offs carry acquire/release pairs so TSan sees them) on unit + loop tests | every commit (CI, in the runner-level e2e job: the container job cannot set the host sysctl, docs/12) | yes |
| Live GStreamer objects per test / scenario | `leaks` tracer checkpoints bracketing every test case and every `fjarr-opsim` scenario | every commit (CI) | 3c (docs/23 slices) |
| Uninitialised reads MSan would need every library rebuilt for | valgrind memcheck (below); MemorySanitizer is deliberately not used — GLib, GStreamer, libsoup and libstdc++ would all need instrumenting | nightly | — |
| Object census + RSS over a soak | `GET /memory` before/after N sessions (`fjarr-opsim soak --cycles N`) | 20 cycles every commit (the e2e job); 200 nightly and at the 3c gate | yes (docs/16 budget) |
| Uninitialised reads, invalid frees sanitizers miss | valgrind memcheck on loop tests | nightly | trend → gate at M3 |
| Allocations on the hot path | heaptrack on a streaming scenario | nightly | docs/16 budget (one buffer header per subscriber per frame, nothing else) |

Every tool prints a one-screen verdict and writes JSON, so an AI agent
running `make agent-test-asan` or `curl :7381/memory` gets an answer it
can act on, not a wall of output.

## Unattended-access test (the industrial gate)

Scripted per desktop-backend spike and kept forever after: reboot the
robot-sim (later: a real NUC), wait, assert a session can start and see the
display **with zero local interaction**. This test decides ADR-0006.

## What CI runs

Today (M0.5–slice 2): lint, Rust and web unit tests, builds, docs gates.
From slice 3a/3b: lint (clang-tidy, clippy, eslint, markdownlint, lychee)
→ unit + component (C++ under ASan and TSan) → browser-lab e2e on the
compose stack ([docs/25](25-browser-lab.md): real Chromium over CDP,
software encoders; the real agent, `fjarr-opsim` incl. a 20-cycle soak;
VA-API asserted only on GPU runners when available) → docs build.
**Nightly** (`nightly.yml`, from slice 3c; also on demand): the 200-cycle
soak, valgrind memcheck on the loop tests, heaptrack on a streaming
scenario, the `netem-*` scenarios and latency trends, with lab artifacts
(traces, profiles, captures) attached to the run. It runs on the hosted
runner by default and on a **self-hosted GPU runner** when one is
registered (docs/12): the same job, with `media.encoder = auto` engaging
VA-API where `/dev/dri` exists, so the nightly measures the product path
as soon as such a machine is available.
