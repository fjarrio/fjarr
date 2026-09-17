---
title: "ADR 0022: Baseline bump to Ubuntu 26.04 LTS / GStreamer 1.28"
---

- **Status**: accepted (supersedes [ADR-0002](0002-ubuntu-2404-baseline.md))
- **Date**: 2026-09-17

## Context

ADR-0002 fixed the robot baseline at Ubuntu 24.04 LTS / GStreamer 1.24.
The [webrtcbin spike](../../agent/spikes/webrtcbin-probe/README.md) found
two behaviours on 1.24 that shape the agent design: no ICE restart, and
an answerer that stalls its whole bundled transport when a transceiver is
set to `inactive`. Ubuntu 26.04 LTS shipped in April 2026; what it carries
was verified in throwaway containers and by reading the `webrtcbin`
source of the 1.24 and 1.28 branches:

| Component | 24.04 (ADR-0002) | 25.04 | 25.10 | **26.04 LTS** |
|---|---|---|---|---|
| GStreamer | 1.24.2 | 1.26.0 | 1.26.6 | **1.28.2** |
| libnice | 0.1.21 | 0.1.22 | 0.1.22 | **0.1.23** |
| libsoup | 3.4.4 | 3.6.5 | 3.6.5 | **3.6.6** |
| clang | 18 | 20 | 20 | **21** |

Source findings (`gstwebrtcbin.c`, `gst-libs/gst/webrtc/nice/nice.c`):

- **ICE restart is not implemented in 1.24, 1.26 or 1.28.** The
  `create-offer`/`create-answer` options argument is unused (`/* TODO:
  use the options argument */`), previous ICE credentials are reused on
  every re-offer (`/* FIXME: deal with ICE restarts */` at all three
  credential sites), and the libnice wrapper exposes gathering and
  `set_local_credentials` but never `nice_agent_restart`. The docs/08
  `session-close{retry:true}` fallback is therefore the design, not a
  stopgap; in-place ICE restart on the agent requires an upstream
  contribution ([open question #21](../18-open-questions.md)).
- **The `inactive` stall is a 1.24 answerer-side EOS.** On 1.24 a
  transceiver going `inactive` unconditionally sends EOS on its source
  pad; the downstream sink returns EOS, the `nicesrc` task stops, and with
  `max-bundle` the one transport stops draining its socket — exactly the
  spike's `RcvbufErrors`. 1.26+ adds `reuse-source-pads`, which suppresses
  the EOS. A browser answerer has no such mechanics.

## Options considered

- **Stay on 24.04 / 1.24** for the LTS-to-LTS window. Keeps the
  environment as verified, but ships a stack whose known defects we would
  work around for the product's first two years, and 24.04's support
  window ends before the fleet features (M7/M8) mature.
- **Bump to 26.04 LTS / 1.28.** Current LTS, the `reuse-source-pads`
  fix, newer libnice/libsoup, a clang with better sanitizers. Costs a
  dev-container/CI/Dockerfile rebuild and a spike re-run before the C++
  core is written. Does not buy ICE restart.
- **Bump only GStreamer** (backport 1.28 onto 24.04). Rejected: a
  self-built GStreamer on robots is the packaging burden ADR-0020/0021
  exist to avoid.

## Decision

Ubuntu 26.04 LTS / GStreamer 1.28 is the baseline for robots, the dev
container and CI. The bump is its own increment ("slice 2.9") before
slice 3b: Dockerfiles, CI, docs/04 and docs/14 updated; the doctor
re-verified (VA-API on 1.28 `vah264enc`, `openh264enc`, `pipewiresrc`,
libei); the webrtcbin spike re-run on 1.28 with a `reuse-source-pads=TRUE`
answerer (Q3 `inactive`) and, from slice 3a, against a Chromium answerer.
Removal of a track uses `inactive` if the re-run passes, `sendonly` plus a
closed valve otherwise ([docs/23](../23-agent-core-architecture.md#offer-construction-and-renegotiation)).
The GitHub-hosted runner stays whatever Ubuntu GitHub offers; the agent
jobs run inside our dev image so the toolchain is the baseline regardless.

## Consequences

Customers on 24.04 robots are not supported by the packaged agent until
they upgrade; embedders may build `libfjarr` against 1.24 at their own
risk (the `inactive` path is then the documented `sendonly` fallback).
Every version in docs/14 is re-checked in slice 2.9. ADR-0002 is
superseded.
