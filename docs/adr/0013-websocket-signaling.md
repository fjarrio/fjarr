---
title: "ADR 0013: WebSocket signaling"
---

- **Status**: accepted
- **Date**: 2026-09-15

## Context

Browsers speak WSS natively; enterprise networks pass 443. The camera streamer proved
MQTT signaling works for agents but adds a broker nobody else in the stack
needs, and its transport sat behind a tiny publish/callback seam — the
pattern worth keeping.

## Options considered

WSS to `fjarr-server` (one hop, one auth model, server-side presence =
`peer-gone` last-will equivalent) · MQTT broker (extra infra; browser MQTT
is second-class) · both now (unneeded complexity).

## Decision

WSS is the only shipped transport; the agent's `SignalingTransport` seam
(docs/09) keeps an MQTT adapter buildable if fleet demand appears (open
question #5).

## Consequences

`fjarr-server` owns presence/liveness semantics (docs/08 `peer-gone`);
no broker to operate; the seam costs one small interface.
