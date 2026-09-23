---
title: Open Questions
description: Live list of undecided items, each with an owner and the ADR that will close it.
---

Living document — add freely, close by linking the ADR/spec section that
answered. Nothing here blocks the current milestone unless marked ⚠.

| # | Question | Surfaced | Closes via | Notes |
|---|---|---|---|---|
| 1 | Is fjarr.com worth acquiring (currently registered)? | M0 naming | business call | fjarr.io is primary; revisit if traction |
| 2 | Jetson/NVENC encoder adapter — when? | camera-streamer heritage | roadmap insert post-M6 | design partner demand decides |
| 3 | Two-way audio (talk to person at robot)? | vision | capability spec + ADR | mic/speaker privacy questions too |
| 4 | Session recording/replay (audit + support)? | docs/10 | capability spec | storage + consent design needed |
| 5 | MQTT signaling adapter — real demand? | camera-streamer seam | ADR if requested | seam exists; don't build speculatively |
| 6 | SWUpdate vs RAUC vs Mender — final call | ADR-0016 proposed | ADR-0016 acceptance at M8 start | re-evaluate ecosystems then |
| 7 | Metrics retention policy + storage engine (M7) | observability | M7 spec revision | Cloud cost model input |
| 8 | Staged-rollout campaign semantics (segments, halt rules) | M8 | M8 spec | steal hawkBit vocabulary? |
| 9 | TPM-backed device keys — require or recommend? | docs/10 | M5 security review | NUC TPM2 present; portability vs strength |
| 10 | TURNS / TCP-443 TURN fallback priority | docs/04 | M5 | hospital/enterprise networks |
| 11 | Separate PeerConnection for bulk transfers? | docs/16 | M4 measurement | only if isolation budget fails |
| 12 | `@fjarr/core` for non-React frameworks (Vue/Svelte wrappers)? | delivery model | demand-driven | core is framework-agnostic by design |
| 13 | CLA tooling + legal review of AGPL/commercial dual licensing | ADR-0011 | before first external PR | ⚠ before accepting outside code |
| 14 | Docs site versioning scheme (per release vs latest+next) | docs/19 | M1 release process | Starlight supports versions via plugin |
| 15 | ~~Tie `@fjarr/core` TS types to the golden fixtures~~ **closed 2026-09-16** (slice 2): `web/packages/core/test/fixtures.test.ts` replays every fixture through the runtime guards | slice-1 review | [docs/21 testing](21-web-client-architecture.md#testing-docs15) | all three implementations are now conformance-checked against `protocol/fixtures` |
| 16 | Point clouds / depth: stream class (ADR-0018) vs depth-as-video per sensor type? | docs/21 review | first sensor-streaming capability (M4+) | decision matrix in [docs/06](06-capabilities.md#sensor-transport); the spike runs in **M4**; the ADR-0018 chunker is built only when a capability picks the stream class |
| 17 | Audio uplink default: push-to-talk everywhere, or open-mic allowed per grant param? | fjarr.audio planning | fjarr.audio spec | PTT default is written; the grant-param escape hatch is the question |
| 18 | ~~`fjarr.logs` milestone~~ **closed 2026-09-16**: peer mode in M4, backend mode in M7 | docs/06 | [roadmap](17-roadmap.md#m4--files-telemetry-logs-sensors) | — |
| 21 | In-place ICE restart on the agent — **source-verified absent in webrtcbin 1.24, 1.26 and 1.28** ([ADR-0022](adr/0022-baseline-ubuntu-2604-gstreamer-128.md)); remaining question: contribute it upstream (offer option + `nice_agent_restart`), or check whether `webrtcsink` does it | [spike](../agent/spikes/webrtcbin-probe/README.md), [docs/23](23-agent-core-architecture.md#offer-construction-and-renegotiation) | ADR-0007 spike (slice 6) | rung 2 stays the `session-close{retry:true}` fallback: a media restart instead of an in-place restart |
| 20 | Session resume across a signaling blip (`hello` with `session_id`) — so a WSS hiccup does not tear down a healthy media path | [slice-2 review](reviews/slice-2-review.md), [docs/23](23-agent-core-architecture.md#signaling-client) | after slice 5 (measured need) | needs a protocol addition on all three tiers plus server-side session retention; the operator's ladder already covers the blip with a new round, at the cost of a media restart |
| 22 | IPv6 inside the tunnel — worth it, or is IPv4-only honest for the lifetime of the feature? | [docs/27](27-network-tunnel.md) | M4.5 | ROS 2 and ssh are fine on v4; the cost is a second address scheme and a second policy path |
| 23 | How should tunnel traffic and video share one peer connection under congestion? | [docs/27](27-network-tunnel.md) | M4.5 measurement | related to #11 (separate PeerConnection for bulk); a `scp` must not starve the camera, and the tunnel has no application-level backpressure to lean on |
| 24 | Windows support for `fjarr-connect` (WSL2 only, or native Wintun)? | [ADR-0024](adr/0024-native-operator-client.md) | demand-driven | Linux and macOS are the committed matrix; WSL2 is the free answer if it suffices |
| 25 | Is a forwarded-socket mode ever worth adding for hosts that cannot provide a TUN device? | [ADR-0023](adr/0023-network-tunnel-virtual-interface.md) | demand-driven | deliberately not built (ADR-0023); it cannot carry DDS, so it would only serve `ssh`/`scp` on a locked-down host |
| 26 | Default tunnel range vs cellular carriers that hand out `100.64.0.0/10` WAN addresses | [docs/27](27-network-tunnel.md#addressing) | M4.5 field data | mitigated by only ever adding /32 routes plus an overlap check in `--check`; revisit if a design partner's carrier collides |
| 19 | Presentation mode default: portaled views on one session vs one session per window? | [docs/22](22-remote-desktop-client.md#presentation-mode) | M3 spike | portal is the written primary (one grant, one lease, no cross-window input proxy) but relies on same-origin popups sharing the opener's agent cluster for `srcObject`; the spike measures it on Chrome/Edge/Firefox and under `Cross-Origin-Opener-Policy: same-origin`, which many host apps set |
