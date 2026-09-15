---
title: "ADR 0001: WebRTC + GStreamer"
---

- **Status**: accepted
- **Date**: 2026-09-15

## Context

Fjarr needs browser-reachable, low-latency media (camera + desktop) and data
channels to NAT'd robots, with hardware encoding on Intel NUCs.

## Options considered

- **WebRTC via GStreamer (webrtcbin/webrtcsink)** — browser-native, NAT
  traversal + DTLS-SRTP built in, GStreamer gives hardware pipelines and
  capture sources; team has years of production experience (camera-streamer).
- Custom protocol + WebSocket/MSE video — no P2P, higher latency, reinvents
  congestion control and encryption.
- Embed RustDesk — evaluated at length (origin discussion archived in the
  gitignored `inspiration/` folder): AGPL fork risk, no embeddable web API, generic product not a
  framework.
- aiortc/Python on the robot — proven too limited in teleop-car (performance,
  packaging).

## Decision

WebRTC end-to-end, implemented with GStreamer on the agent. Browser peers
use native WebRTC APIs.

## Consequences

Browser reach and P2P for free; we own GStreamer pipeline complexity;
relay-only fallback must be capacity-planned (docs/16). Element choice
inside GStreamer is ADR-0007.
