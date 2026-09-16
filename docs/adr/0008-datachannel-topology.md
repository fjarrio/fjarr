---
title: "ADR 0008: DataChannel topology"
---

- **Status**: accepted
- **Date**: 2026-09-15

## Context

The teleop car put 150 msg/s of control on one default (reliable-ordered) channel —
head-of-line blocking by construction; the camera streamer used a single channel per peer
too. Mixing bulk, control, and realtime traffic in one queue is the classic
failure.

## Options considered

One channel (simple, wrong) · per-message flags on one channel (SCTP can't
do that) · **fixed named classes** (`control` reliable-ordered, `realtime`
unordered/no-retransmit, `bulk:<cap>` reliable-ordered per capability) ·
fully dynamic per-capability channels (negotiation complexity without need).

## Decision

The three-class topology specified in docs/08#datachannel-topology, created
by the agent at session setup; capabilities declare which classes they use.

## Consequences

Reliability is visible in the spec and the wire; adding a class later is a
protocol minor. Bulk isolation beyond SCTP (separate PeerConnection) stays a
measured M4 option (docs/16).
