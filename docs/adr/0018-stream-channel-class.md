---
title: "ADR 0018: Stream channel class"
---

- **Status**: accepted
- **Date**: 2026-09-16
- **Supersedes**: extends [ADR-0008](0008-datachannel-topology.md) (which stays accepted; this adds a fourth class rather than changing the three)

## Context

ADR-0008 fixed three DataChannel classes: `control` (reliable, ordered,
≤ 16 KiB JSON), `realtime` (unordered, no retransmit, ≤ 16 KiB JSON) and
`bulk` (reliable, ordered, binary). Planning point clouds, depth maps and
other high-rate binary sensor data ([docs/06](../06-capabilities.md),
[docs/21](../21-web-client-architecture.md#publishing-sending-toward-the-robot))
exposed a gap: such data is binary and often larger than one SCTP message
(needs chunking), yet must be **lossy and newest-wins** — a stale point
cloud is worthless, and queueing it behind newer frames (bulk semantics)
destroys interactivity. Neither `realtime` (JSON, single message) nor
`bulk` (reliable, ordered) fits.

## Options considered

- Encode such data as video (depth packed into luma/chroma): works for
  dense 2-D data, not for sparse point clouds or arbitrary sensor frames;
  still valid as a per-capability choice.
- Send on `bulk` and drop client-side: the reliable-ordered queue still
  delays newer frames behind older ones under loss — exactly the failure.
- **A fourth class, `stream`**: unordered, no retransmit, binary, with a
  small frame header enabling reassembly of chunked frames and dropping
  of incomplete/stale ones.

## Decision

Add `fjarr:stream:<cap>` per docs/08#datachannel-topology, with the 12-byte
frame header and frame-level newest-wins semantics. Capabilities declare it
like any other class; the web client exposes `session.stream(cap)`.

## Consequences

Four classes to document and test instead of three; `@fjarr/core` gains a
chunker/reassembler (unit-tested with loss injection). Depth-as-video
remains available where it fits better — the class exists so capabilities
have a correct default for lossy binary data.
