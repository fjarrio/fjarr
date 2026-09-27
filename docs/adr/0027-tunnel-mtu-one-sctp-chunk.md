---
title: "ADR 0027: the tunnel MTU is one SCTP chunk"
---

- **Status**: accepted
- **Date**: 2026-09-27

## Context

[docs/27](../27-network-tunnel.md#the-packet-path) fixes the tunnel MTU at
**1280** — the IPv6 minimum — reasoning that "UDP, DTLS and SCTP add roughly 80
bytes, so a 1280-byte inner packet sits well inside a 1500-byte path". That
sizes the packet against the **UDP** path. It never sized it against SCTP's own
path MTU, which is smaller: GStreamer pins usrsctp's to **1200** with PMTU
discovery off (gst-plugins-bad `ext/sctp/sctpassociation.c`, following
draft-ietf-rtcweb-data-channel). So every full-size tunnel packet has been a
**two-chunk** SCTP message, on a channel that, by design, never retransmits
(`max-retransmits=0`, [docs/08](../08-protocol.md#datachannel-topology)).

[Question #28](../18-open-questions.md) — a bulk transfer stalling in about half
of attempts since slice 4.5c — turns out to be that. Captures of both tunnel
devices during a stall show the **robot→operator direction of the association
dying permanently** (every packet offered after the stall lost: data, bare ACKs,
DDS, the control channel) while operator→robot delivers everything. That is the
shape of a receiver whose window never reopens. The mechanism consistent with it
is a lost fragment stranding its sibling in the receiver's reassembly queue on a
no-retransmit stream; this ADR does not depend on that detail, only on the edge
below.

## Evidence

The opsim scp gate (1 GiB, hash-verified, video beside it), ten runs per MTU, one
session, with a late re-check of the highest green point to rule out drift:

| Tunnel MTU | Green | Pass times |
|---|---|---|
| 1100 | 10 / 10 | 23–31 s |
| 1172 | 10 / 10 | 29–34 s |
| 1180 | 10 / 10, and 10 / 10 again late in the session | 34–39 s |
| **1184** | **10 / 10** | 37–41 s |
| **1185** | **4 / 10** | 48–55 s |
| 1188 · 1189 · 1192 · 1212 · 1230 · 1280 | 4–6 / 10 each | 38–62 s |

**The failure switches on at exactly one byte: 1184 → 1185.** 1184 is
1200 − 16, usrsctp's path MTU less the DATA chunk header: the largest message
carried in one chunk. Pass times step up at the same byte, which is the cost of
fragmentation visible even in the runs it does not kill. Before this edge was
found, a prediction of 1172 (counting the 12-byte common header too) was tested
and **failed** — 1180 stayed green — which is how the real edge was located
rather than assumed.

Two further facts bound it. `fjarr-connect` (webrtc-rs) moved 1 GiB 15 times for
15 at MTU 1280, relay and direct, with and without video: the wedge needs usrsctp
on the receiving end, and neither real operator is usrsctp. And independently of
any bug, a two-chunk message on a no-retransmit channel is lost if *either* chunk
is, which roughly doubles the effective loss of every full-size packet on a lossy
link — a cost paid by every operator.

## Decision

**The tunnel MTU is the largest IP packet that travels as one SCTP DATA chunk:
the SCTP path MTU less 16 — 1184 for the 1200 every WebRTC stack uses.**

- The agent reports it on `open` as today; `fjarr-connect` and the installer's
  device take it from there, so both ends agree without either hard-coding it.
- docs/27's "MTU 1280, the IPv6 minimum" is replaced by this rule and its
  reason. IPv6 inside the tunnel ([question #22](../18-open-questions.md)) would
  need 1280; if #22 is ever taken up, it has to raise the SCTP path MTU as well.
- A regression pins it: the scp gate at the configured MTU, and a unit test that
  the agent's default equals `1200 − 16`.

## Alternatives considered

- **Leave 1280 and fix usrsctp.** The bug is plausibly usrsctp's reassembly
  accounting, and worth reporting upstream with this data. But the agent ships
  the distribution's GStreamer (ADR-0022), a fix would take releases to reach
  robots, and the fragment-loss cost above remains even with the bug fixed.
- **A margin below the edge (1152, 1100).** Safer if a future GStreamer changed
  its overhead — but the rule states the dependency explicitly, so a change
  would be caught by the regression rather than by a margin. 1184 keeps 8 %
  more payload per packet than 1100. A reviewer may reasonably prefer 1152; the
  evidence supports anything at or below 1184.
- **A reliable stream class.** Retransmitted fragments cannot strand; measured
  in 4.5c at 2 of 8 failing, but that arm predates the agent-crash fix and is
  contaminated by it. It also reintroduces head-of-line retransmission under TCP,
  which docs/08's rationale for the class rejects.
- **Raise the SCTP path MTU above 1200.** Not ours to set per peer, and the
  1200 exists because real paths (VPNs, carriers) do not reliably carry more.

## Consequences

- #28 closes as caused and fixed, once the default changes and the scp gate
  runs green over 20 runs at it.
- About 8 % more packets per byte for full-size flows than at 1280 (a TCP
  segment carries 1,132 bytes instead of 1,228) — measured here as *faster*
  anyway, because fragmentation cost more than the extra headers do.
- Existing robots keep whatever MTU their installer set; the agent reports the
  device's real MTU, so a robot left at 1280 still works, only with #28's risk.
  docs/26's installer sets the new default.
