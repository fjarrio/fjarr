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
mock's best idea), scripted in integration tests. Minimum fault menu:

| Fault | Expected behavior |
|---|---|
| Signaling socket killed | reconnect with the spec backoff; ICE restart; session resumes or closes cleanly |
| **Server goes silent without disconnecting** | heartbeat detects within budget; teardown + reconnect (the nastiest real-world case) |
| Packet loss 5–20 % on media path (netem) | video degrades, never stalls > budget; input stays responsive |
| Relay-only forced (`iceTransportPolicy: relay`) | everything works through coturn; budgets per docs/16 relay column |
| Agent SIGKILL mid-session | operator sees `peer-gone` ≤ heartbeat budget; supervised restart; robot reachable again < 30 s |
| Media plane hang (SIGSTOP) | control plane survives; ownership lease expires (fail-open); watchdog restarts media plane |
| Mid-transfer network kill | file resume from received ranges; hash verifies |
| Grant expired / clock skew | clean `grant-expired`, no retry storm (fatal vs retryable taxonomy) |

## Latency harness (glass-to-glass) {#latency-harness}

From M1, a measurement rig — not vibes:

- **Glass-to-glass**: robot-sim renders a frame counter + timestamp;
  headless-browser side OCRs/reads it from the decoded frame; Δ = g2g.
  Report p50/p95 under clean, lossy, and relay conditions.
- **Input-to-photon**: synthetic click → screen change at a known pixel →
  time to that change appearing in the received stream.
- Results append to a tracked CSV; regressions against
  [docs/16](16-performance-budgets.md) fail CI (M1+) and gate the ADR-0006/0007
  decisions.

## Safety behaviors {#safety-behaviors}

Learned the hard way (teleop-car's silently-regressed deadman): every safety
behavior is **spec'd, implemented, and covered by a test that fails when it
regresses**:

- input silence deadman on actuation-bearing channels;
- `release_all_input()` on any session end (no stuck modifiers — test:
  disconnect mid-keydown, assert keyup injected);
- ownership lease expiry (fail-open) under media-plane hang;
- heartbeat teardown timings.

## Unattended-access test (the industrial gate)

Scripted per desktop-backend spike and kept forever after: reboot the
robot-sim (later: a real NUC), wait, assert a session can start and see the
display **with zero local interaction**. This test decides ADR-0006.

## What CI runs (from M0.5)

Lint (clang-tidy, clippy, eslint, markdownlint, lychee) → unit + component →
integration on the compose stack (software encoders; VA-API asserted only on
GPU runners when available) → docs build. Nightly: soak + latency trends.
