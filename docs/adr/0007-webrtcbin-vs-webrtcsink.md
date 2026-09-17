---
title: "ADR 0007: webrtcbin vs webrtcsink"
---

- **Status**: **proposed** — closed by an M1 spike
- **Date**: 2026-09-15

## Context

The camera streamer proved a hand-rolled `webrtcbin` + FrameHub fan-out in production —
but with zero congestion control, and adaptive bitrate is a hard Fjarr
requirement (docs/16). `webrtcsink` (gst-plugins-rs) natively ships GCC
congestion control, encoder management, and multi-consumer fan-out — i.e.
much of what the camera streamer hand-built — at the cost of less control over the exact
session/track model docs/08 specifies, and it is not packaged in Ubuntu
24.04 (we'd build/vendor it).

**Spike finding (2026-09-17, [agent/spikes/webrtcbin-probe](../../agent/spikes/webrtcbin-probe/README.md)):**
`webrtcbin` 1.24.2 ignores the `ice-restart` offer option and wedges the
answerer on `direction=inactive`; `max-bundle` is required for
DataChannels. The protocol gained a `session-close{retry:true}` fallback
so rung 2 of the reconnection ladder works without in-place ICE restart.
Whether `webrtcsink` (newer gst-plugins-rs) restarts ICE is a question for
the spike this ADR waits on.

## Options considered

- **webrtcbin + FrameHub (camera-streamer pattern)** + hand-wired TWCC/GCC bitrate
  loop: full control, proven skeleton, congestion control is the hard new
  part.
- **webrtcsink**: congestion control for free; must verify multi-track
  manifests, per-track valving, DC control, and our reconnect ladder fit its
  model; adds a vendored Rust plugin build to the agent image.

## Decision

Deferred: M1 builds the camera capability on **webrtcbin+FrameHub** (the
proven path) while a timeboxed spike measures webrtcsink against the same
harness (adaptation reaction time, CPU, integration friction with docs/08).
Whichever meets docs/16 with less ongoing complexity wins.

## Consequences

Possible M1 rework if webrtcsink wins (accepted: the capability API hides
the choice). The doctor WARNs on webrtcsink absence until the spike lands.
