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
| [0002](0002-ubuntu-2404-baseline.md) | Ubuntu 24.04 / GStreamer 1.24 baseline | superseded by 0022 |
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
| [0018](0018-stream-channel-class.md) | Stream channel class (lossy binary frames) | accepted |
| [0019](0019-agent-process-model.md) | Agent process model — one process, two restartable planes | accepted |
| [0020](0020-vendor-sources-as-gstreamer-plugins.md) | Vendor camera support ships as separately packaged GStreamer plugins | accepted (desktop sentence superseded by 0021) |
| [0021](0021-desktop-backends-as-runtime-modules.md) | Desktop backends are in-tree runtime modules in separate packages | accepted |
| [0022](0022-baseline-ubuntu-2604-gstreamer-128.md) | Baseline bump to Ubuntu 26.04 LTS / GStreamer 1.28 | accepted |
| [0023](0023-network-tunnel-virtual-interface.md) | The network tunnel is a virtual interface, not forwarded sockets | accepted |
| [0024](0024-native-operator-client.md) | `fjarr-connect` — a native operator client, data channels only | accepted |
| [0025](0025-encoder-families.md) | Four encoder families behind one adapter, and what each owes CI | accepted |
