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

| Fault | Expected behavior |
|---|---|
| Signaling socket killed | reconnect with the spec backoff; ICE restart; session resumes or closes cleanly |
| **Server goes silent without disconnecting** | heartbeat detects within budget; teardown + reconnect (the nastiest real-world case) |
| Packet loss 5–20 % on media path (netem) | video degrades, never stalls > budget; input stays responsive |
| Relay-only forced (`iceTransportPolicy: relay`) | everything works through coturn; budgets per docs/16 relay column |
| Agent SIGKILL mid-session | operator sees `peer-gone` ≤ heartbeat budget; supervised restart; robot reachable again < 30 s |
| Media plane hang (SIGSTOP) | control plane survives; ownership lease expires (fail-open); watchdog restarts media plane |
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
- Results append to a tracked CSV; regressions against
  [docs/16](16-performance-budgets.md) fail CI (M1+) and gate the ADR-0006/0007
  decisions.

## Safety behaviors {#safety-behaviors}

Learned the hard way (the teleop car's silently-regressed deadman): every safety
behavior is **spec'd, implemented, and covered by a test that fails when it
regresses**:

- input silence deadman on actuation-bearing channels;
- `release_all_input()` on any session end (no stuck modifiers — test:
  disconnect mid-keydown, assert keyup injected);
- ownership lease expiry (fail-open) under media-plane hang;
- heartbeat teardown timings.

## Memory safety (C++) {#memory-safety-c}

The agent wraps a C object system, so lifetime bugs get their own ladder
([docs/23](23-agent-core-architecture.md#memory-and-lifetime-discipline-and-the-tooling-that-enforces-it)):

| Layer | Tool | When | Gate? |
|---|---|---|---|
| RAII kit only touches refcounts | grep gate in `/verify`, clang-tidy `-Werror` | every commit | yes |
| Address/Undefined/Leak | `asan` preset (+ `lsan.supp`) on unit + loop tests | every commit (CI) | yes |
| Data races in the threading model | `tsan` preset on unit + loop tests | every commit (CI) | yes |
| Live GStreamer objects per test / scenario | `leaks` tracer checkpoints bracketing every test case and every `fjarr-opsim` scenario | every commit (CI) | yes |
| Object census + RSS over a soak | `GET /memory` before/after 200 sessions (`fjarr-opsim`) | nightly | yes (docs/16 budget) |
| Uninitialised reads, invalid frees sanitizers miss | valgrind memcheck on loop tests | nightly | trend → gate at M3 |
| Allocations on the hot path | heaptrack on a streaming scenario | nightly | docs/16 "0 per frame" budget |

Every tool prints a one-screen verdict and writes JSON, so an AI agent
running `make agent-test-asan` or `curl :7381/memory` gets an answer it
can act on, not a wall of output.

## Unattended-access test (the industrial gate)

Scripted per desktop-backend spike and kept forever after: reboot the
robot-sim (later: a real NUC), wait, assert a session can start and see the
display **with zero local interaction**. This test decides ADR-0006.

## What CI runs (from M0.5)

Lint (clang-tidy, clippy, eslint, markdownlint, lychee) → unit + component →
browser-lab e2e on the compose stack ([docs/25](25-browser-lab.md): real
Chromium over CDP, software encoders; VA-API asserted only on GPU runners
when available) → docs build. Nightly: soak, profiling scenarios and
latency trends, with lab artifacts (traces, profiles, captures) attached
to the run.
