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
24.04 or 26.04 (re-checked in slice 2.9; we'd build/vendor it).

**Spike finding (2026-09-17, [agent/spikes/webrtcbin-probe](../../agent/spikes/webrtcbin-probe/README.md)):**
`webrtcbin` 1.24.2 ignores the `ice-restart` offer option and wedges the
answerer on `direction=inactive`; `max-bundle` is required for
DataChannels. The protocol gained a `session-close{retry:true}` fallback
so rung 2 of the reconnection ladder works without in-place ICE restart.
Re-run on 1.28.2 (2026-09-18, slice 2.9): ICE restart still absent and
`bundle-policy=none` still fails; the `inactive` stall is the answerer's
EOS and is gone once the answerer sets `reuse-source-pads`. Whether
`webrtcsink` (newer gst-plugins-rs) restarts ICE is a question for the
spike this ADR waits on.

**Chromium-answerer spike (2026-09-19, slice 3a, [report](../../agent/spikes/webrtcbin-probe/README.md#chromium-answerer-slice-3a)):**
the same `webrtcbin` offerer run against the browser lab's real Chromium
(153, [docs/25](../25-browser-lab.md)) for Q1/Q3/Q6, three configurations,
host candidates across the compose network. Every FAIL that had a
webrtcbin on the answering side disappears with a browser there: the
pre-offer DataChannel opens and carries strings and a 2 MiB burst; a
track added by renegotiation decodes without the untouched track missing
a frame; removing it with `direction=inactive` + re-offer makes Chromium
mute that receiver and drop its late RTP while the other track keeps
30 fps and the DataChannel works both ways — with the offerer still
pushing the removed track's packets for 2.5 s before the valve closed,
i.e. a harsher order than [docs/23](../23-agent-core-architecture.md)'s;
and `bundle-policy=none` connects (three ICE transports) where the
loopback never did. The docs/23 track-removal decision (valve closed
first, transceiver `inactive`, re-offer) **holds against a real browser**;
`reuse-source-pads=TRUE` remains a requirement only for the webrtcbin
answerers Fjarr ships. `max-bundle` stays the policy (one transport is
what the reconnection ladder assumes, and browsers bundle anyway). ICE
restart is unchanged, and the `webrtcsink` question above is still open.

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
