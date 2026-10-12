---
title: "ADR 0037: Release Content over HTTPS Chunks"
description: Release content travels as resumable, content-addressed chunks over HTTPS from object storage, under a per-robot budget per time interval; Fjarr's WebRTC session carries control and progress, never the bulk bytes.
---

- **Status**: accepted
- **Date**: 2026-10-12
- **Supersedes**: —

## Context

Releases are hundreds of megabytes to gigabytes, robots are often on 4G with
carrier data caps, and links drop. docs/06 had said release content would
travel "over the file capability's backend mode", that is over Fjarr's own
WebRTC transport. Behind carrier-grade NAT that means through TURN, whose
bandwidth the relay's operator pays for, and SCTP throughput at gigabyte
scale has not been measured. The update engines' own network paths are weak
on mobile links ([ADR-0035](0035-releases-orchestrated-appliers-narrow.md)).

## Options considered

1. **Over Fjarr's WebRTC data channel.** One transport for everything, but
   relay costs and untested throughput.
2. **HTTPS, content-addressed chunks, from object storage.** Resumable at
   the chunk, verified per chunk against the signed manifest, delta by
   fetching only chunks the robot lacks, storage without egress fees (R2, as
   the apt repository already uses).
3. **Option 2 plus robots sharing chunks over a depot's LAN.**

## Decision

**Option 2** (Erik, 2026-10-12), with a **budget per time interval** per
robot ("2 GB a day", "20 GB a month resetting on the 14th"), a rate cap that
keeps live video and teleoperation responsive, and a download prediction per
robot before a campaign starts. Fjarr's live connection carries control,
progress and run logs only. Depot sharing (option 3) is deferred; the chunk
format allows it later without change.

## Consequences

- Fjarr Cloud and self-hosters run an S3-compatible chunk store; the bundle
  tool uploads only new chunks.
- A release can download over several days, and fetching is separate from
  installing.
- The engines install from the robot's local cache and never stream.
- Revisit if robots must update where HTTPS to storage is blocked but a Fjarr
  session works, which would bring option 1 back as a fallback.
