---
title: Performance Budgets
description: Latency, bitrate, and resource targets — and the degradation policy when they can't be met.
---

Budgets are **requirements with numbers**. The [latency
harness](15-testing-strategy.md#latency-harness) measures them continuously;
a regression is a failing test. Numbers marked ~ are initial engineering
targets to be confirmed against M1 measurements (then this doc goes
`stable`).

## Latency (p50 / p95, 1080p30 primary track)

| Path | Glass-to-glass | Input-to-photon |
|---|---|---|
| LAN direct | ~120 / 200 ms | ~150 / 250 ms |
| Internet P2P (STUN) | ~200 / 350 ms | ~250 / 400 ms |
| TURN relay | ~250 / 450 ms | ~300 / 500 ms |

Terminal echo round-trip: < 150 ms LAN. Pointer motion send rate ≤ 60 Hz
coalesced; stale motion dropped, never queued.

## Bitrate tiers (per video track)

| Tier | Resolution/fps | Target bitrate |
|---|---|---|
| Active (operator focused) | up to 1920×1080@30 | 2.5–5 Mbps adaptive |
| Inactive thumbnail | 960×540@5 | ≤ 300 kbps |
| Disabled | — | ~0 (valve dropped; no renegotiation) |

Audio (Opus): 32–64 kbps per track downlink, 24–48 kbps uplink; mouth-to-ear
< 250 ms P2P. Stream-class sensor frames (ADR-0018): capability-declared
ceiling (e.g. a decimated point cloud ≤ 2 Mbps at 5–10 Hz); frames older
than one interval are dropped, never queued.

**Adaptive bitrate is a hard requirement** (the camera-streamer gap): the encoder
target follows congestion feedback (per-peer TWCC into the agent's own
estimator — [ADR-0007](adr/0007-webrtcbin-vs-webrtcsink.md)) between a
floor of 250 kbps and the tier target, reacting within ~2 s to loss and
recovering within ~10 s. Fixed-CBR-only operation is a spec violation.
Because one encoder serves every viewer of a tier, the encoder only follows
its viewers within a band (half the tier target and up); a viewer whose
link is below the band is moved to the lower tier on its own, so one bad
receiver never costs the others more than that band
([docs/23](23-agent-core-architecture.md#rate-control-and-tier-switching)).
A passthrough track (the camera's own encoded stream) cannot adapt at the
agent: it adapts by tier only when the source offers a lower stream,
otherwise not at all, and says so (`adaptive: false`). In exchange it
costs no encoder at all — the robot's GPU and CPU budgets above are per
*transcoded* track, so a fleet of passthrough cameras is bounded by the
network, not by the encoder count.

## Robot resource budget (Intel NUC class, one active session + one thumbnail)

| Resource | Budget |
|---|---|
| CPU (agent total) | < 25 % of one core with VA-API encode; capture-copy path < 60 % |
| GPU | encode within iGPU capacity for 2 concurrent 1080p30 encodes |
| RAM | agent RSS < 300 MB steady state |
| Store-and-forward disk | bounded ≤ 200 MB (oldest-first eviction + drop counter) |
| Heap allocations per frame on the streaming hot path (FrameHub → appsrc), steady state | one metadata-only `GstBuffer` header per subscriber per frame (the fan-out's per-subscriber PTS rebase, [docs/23](23-agent-core-architecture.md#fan-out)) and GStreamer's own refs — nothing else: no pixel copy, no per-frame `std::function`, string or container allocation (heaptrack) |
| Object census after a 200-session soak | identical to baseline; RSS growth < 5 MB |

Multi-viewer scales via FrameHub: +1 viewer ≈ +RTP fan-out cost only (no new
encode) — verify ≤ 5 % CPU per additional viewer.

## Fleet/relay planning

Browser viewers are **relay-realistic**: capacity-plan TURN at ~1 stream =
0.5–5 Mbps depending on activity. Ten concurrent operator views ≈ tens of
Mbps through the relay — metered per session ([docs/10](10-security.md#turn)),
priced through ([docs/03](03-product-strategy.md#usage-meters)).

## Web client budgets {#web-client-budgets}

Measured by the [browser lab](25-browser-lab.md) profiling scenarios
(initial targets, confirmed at slice 5):

| Metric | Budget |
|---|---|
| Main-thread busy while streaming 2 active tracks (60 s) | ≤ 15 % |
| Long tasks (> 50 ms) while streaming | 0 per minute after first frame |
| INP (dashboard interactions while streaming) | ≤ 200 ms |
| JS heap growth per connect/disconnect cycle (100-cycle soak, after GC) | ≤ 50 KB/cycle, no monotonic listener/node growth |
| Time to first frame after `select-tracks` enable (LAN, keyframe requested) | ≤ 300 ms |
| Frames dropped on unchanged tracks during a renegotiation | 0 (frame-stamp counter) |

## Connection health thresholds {#connection-health-thresholds}

The web client's health score ([docs/21](21-web-client-architecture.md#stats-and-connection-health))
derives from the selected candidate pair's RTT and the windowed packet
loss across received tracks:

| Level | Condition (any) |
|---|---|
| good | RTT ≤ 300 ms and loss ≤ 5 % and no freeze in the window |
| degraded | RTT > 300 ms, or loss > 5 %, or a freeze ≥ 500 ms in the window |
| poor | RTT > 600 ms, or loss > 15 %, or no frames decoded for a full window while a track is enabled |

A level changes only after three consecutive 1 s samples agree
(hysteresis). The numbers are initial targets in the same sense as the
latency table above.

## Bulk vs interactive isolation

During a saturating file transfer: interactive video g2g p95 may degrade by
at most +50 ms and input-to-photon by +30 ms vs baseline. If a shared SCTP
association can't hold that, bulk moves to a separate PeerConnection
(measured decision, M4).

## Degradation policy (in order)

1. Reduce inactive-tier tracks (fps, then resolution).
2. Reduce active-tier bitrate toward the floor.
3. Reduce active fps (30→15) before resolution.
4. Drop inactive tracks entirely (UI shows "paused — bandwidth").
5. Never: silently stall media, queue stale input, or let heartbeats starve
   (control DC has priority).

Startup: first frame visible < 2 s after `session-accept` on P2P, < 3 s on
relay (keyframe-on-connect required). **Hot-plug** (docs/08 renegotiation):
a newly connected monitor's first frame < 2 s after the `monitors` event;
**zero** dropped frames and no re-attach on unchanged tracks during the
renegotiation.
