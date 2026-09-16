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
| 15 | Tie `@fjarr/core` TS types to the golden fixtures | slice-1 review | slice 2 (web core) | Rust + schema are conformance-checked against protocol/fixtures; the TS `Envelope`/`TrackManifestEntry` copies are not yet, so they can drift silently. Closing plan in [docs/21 testing](21-web-client-architecture.md#testing-docs15): a vitest golden-fixture test lands with the slice-2 core |
