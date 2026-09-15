---
title: ADR Process
description: How Fjarr records architecture decisions.
---

An ADR captures one decision that had real alternatives: the context, the
options weighed, the choice, and its consequences — so future maintainers
(including us, later) don't re-litigate or accidentally reverse it blind.

## Process

1. Copy [0000-template.md](0000-template.md) → `NNNN-short-slug.md` (next
   free number), status `proposed`.
2. Discuss in the PR; land it `proposed` if implementation must wait for
   evidence (spikes), or `accepted` when decided.
3. **Accepted ADRs are immutable.** Changing course = a new ADR with
   `Supersedes: NNNN` (and the old one gains `Superseded-by`).
4. Small reversible choices don't need an ADR. Anything expensive to
   reverse — protocol shapes, licenses, platform commitments — does.

## Index

| ADR | Title | Status |
|---|---|---|
| [0001](0001-webrtc-gstreamer.md) | WebRTC + GStreamer for media transport | accepted |
| [0002](0002-ubuntu-2404-baseline.md) | Ubuntu 24.04 / GStreamer 1.24 baseline | accepted |
| [0003](0003-polyglot-monorepo.md) | Polyglot monorepo | accepted |
| [0004](0004-rust-signaling.md) | Rust for the signaling server | accepted |
| [0005](0005-web-library-split.md) | TS core + React bindings + demo split | accepted |
| [0006](0006-desktop-backend-selection.md) | Desktop backend selection | proposed (M2 spikes) |
| [0007](0007-webrtcbin-vs-webrtcsink.md) | webrtcbin+FrameHub vs webrtcsink | proposed (M1 spike) |
| [0008](0008-datachannel-topology.md) | DataChannel topology & reliability classes | accepted |
| [0009](0009-privilege-separation.md) | Privilege separation on the robot | proposed |
| [0010](0010-devcontainer-environment.md) | Devcontainer + compose + Xvfb robot-sim | accepted |
| [0011](0011-license-open-core.md) | AGPL-3.0 open core + commercial licensing | accepted |
| [0012](0012-agent-cpp.md) | Agent in C++ | accepted |
| [0013](0013-websocket-signaling.md) | WebSocket signaling with a transport seam | accepted |
| [0014](0014-astro-starlight-website.md) | Astro + Starlight for the website | accepted |
| [0015](0015-backend-integration-strategy.md) | Backend integration: unified contract, sidecar + managed twin | accepted |
| [0016](0016-swupdate-ota.md) | SWUpdate as the OTA foundation | proposed (M8) |
| [0017](0017-libsoup-websocket.md) | libsoup-3 for agent networking | accepted |
