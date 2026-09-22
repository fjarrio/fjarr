---
title: "ADR 0007: webrtcbin vs webrtcsink"
---

- **Status**: **accepted** (2026-09-22, slice 6 planning; the M1 spikes below are its evidence)
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

**Environment re-check (2026-09-22, slice 6 planning):** neither
`webrtcsink` nor gst-plugins-rs's `rtpgccbwe` estimator is packaged on
Ubuntu 26.04, so both routes to congestion control would mean vendoring a
Rust plugin build into the dev image and the agent image. `rtpsession`
does expose `twcc-stats` (transport-wide congestion control feedback, per
packet), and the retransmission and FEC elements are present.

## Decision

**webrtcbin + FrameHub, with a congestion estimator of our own.** The
core built in slices 3–5 (FrameHub fan-out, per-consumer pipelines, the
docs/08 session and track model, the reconnection ladder, introspection)
stays; adaptive bitrate is a small loss-and-delay estimator in `libfjarr`
fed by webrtcbin's per-peer TWCC feedback, driving the shared tier
encoders and per-viewer tier switching as
[docs/23](../23-agent-core-architecture.md#rate-control-and-tier-switching)
specifies. Loss repair is NACK/RTX plus keyframe requests, no FEC
([docs/08](../08-protocol.md#rtp-feedback)).

Rejected: **webrtcsink** — it would replace the tested core for a model
that does not carry docs/08's session and track semantics, and it is not
on the baseline; **vendoring `rtpgccbwe`** — a Rust toolchain in the C++
build and the image for one element, when the estimator it implements is
a few hundred lines against the same `twcc-stats`; **REMB** — deprecated
receiver-side estimation, coarse and unavailable on some browsers.

## Consequences

The estimator is ours to measure and maintain: docs/16's reaction (~2 s)
and recovery (~10 s) times are gated in the lab under the netem profiles
every commit (docs/23 slice 6a). ICE restart stays as recorded (open
question #21). The doctor's `webrtcsink` row is dropped.
