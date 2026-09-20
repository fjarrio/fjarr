---
title: Glossary
description: Shared vocabulary used identically across the C++, Rust, and TypeScript codebases.
---

Terms are normative: code, docs, and protocol fields use these words with
exactly these meanings. Add here first; then use.

| Term | Meaning |
|---|---|
| **Milestone** | A product-level promise (`M0`…`M8`) with a gate anyone can check; the roadmap's unit ([docs/17](17-roadmap.md), [docs/13](13-development-workflow.md#milestones-and-slices)). |
| **Slice** | One reviewable increment inside a milestone: lands on `main`, reviewed retrospectively in `docs/reviews/`; split or inserted as planning finds necessary ([docs/13](13-development-workflow.md#milestones-and-slices)). |
| **Agent** | The Fjarr process on the robot: `libfjarr` embedded in the customer's software, or the `fjarr-agent` reference daemon. |
| **Capability** | A pluggable unit of functionality (camera video, remote desktop, file transfer…). Owns media tracks and/or DataChannel message namespaces. See [docs/05](05-extension-model.md). |
| **Capability consumer** | Whoever the capability serves: a **peer consumer** (a dashboard connected P2P) or the **backend consumer** (observability ingest, OTA orchestration). |
| **Session** | One WebRTC peer connection between an agent and one operator client, carrying whatever capabilities were granted. Identified by `session_id`. |
| **Session grant** | The short-lived signed token (JWT) minted by the customer's backend that authorizes creating a session with specific capabilities. See [docs/09](09-interfaces.md). |
| **Signaling** | The message exchange (offer/answer/ICE) that establishes sessions, relayed by `fjarr-server`. Never carries media. |
| **Sidecar** | `fjarr-server` deployed next to the customer's backend, per [ADR-0015](adr/0015-backend-integration-strategy.md). |
| **Fjarr Cloud** | The managed, multi-tenant implementation of the same contract as the sidecar. |
| **Envelope** | The single tagged-union message format used on control channels; every message has `type`, `event_id`, and a payload. See [docs/08](08-protocol.md). |
| **Correlation (`event_id`)** | Requests carry a unique id; responses echo it. Long operations reply accept → feedback\* → result, all with the same id. |
| **Track** | One WebRTC media stream (e.g. one camera, one monitor). Announced in the **track manifest** before media flows. |
| **Track manifest** | Metadata listing every track in an offer (`track_id`, label, kind, codec, payload type) so UIs can label streams before frames arrive. |
| **Producer pipeline** | The persistent GStreamer pipeline owning capture + encode. Survives peer churn. |
| **Consumer pipeline** | The disposable per-session pipeline feeding one `webrtcbin`. |
| **FrameHub** | The fan-out between producer and consumer pipelines: encode once, serve N sessions. |
| **DC** | WebRTC DataChannel. Fjarr defines reliability **classes** per channel ([docs/08](08-protocol.md#datachannel-topology)). |
| **Robot ID** | The customer's canonical identifier for a device. Fjarr never invents its own device identity; it authenticates the customer's. |
| **Tenant** | One customer organization in a multi-tenant `fjarr-server`/Cloud deployment. |
| **Relay / TURN** | Media forwarded through coturn when direct P2P fails. Ephemeral HMAC credentials only ([docs/10](10-security.md)). |
| **Portal** | XDG Desktop Portal — the D-Bus permission layer for Wayland screen capture and input. |
| **EIS / libei** | Emulated Input (server/library) — the Wayland-native input injection path. |
| **uinput** | Linux kernel interface for creating virtual input devices below the display server. |
| **Backend consumer flows** | Robot↔backend traffic that is not P2P: observability ingest, OTA campaign control. |
| **Store-and-forward** | The agent's bounded offline buffer for durable events, replayed on reconnect (whitelisted types only). |
| **Deadman** | A safety timeout that stops actuation when control input goes silent. Spec'd and tested, never incidental ([docs/15](15-testing-strategy.md)). |
| **ADR** | Architecture Decision Record — [docs/adr/](adr/README.md). |
