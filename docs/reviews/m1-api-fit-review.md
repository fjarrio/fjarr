---
title: "M1 Review: Extension API Fit"
description: Paper-validation of the capability API (docs/05, docs/09) against all seven planned capabilities plus the arm-teach stress test — an M1 gate artifact.
---

**Status**: complete (2026-09-15) · **Gate**: [roadmap M1](../17-roadmap.md)
· **Method**: walk every capability's needs ([docs/06](../06-capabilities.md))
through the `Capability` contract ([docs/05](../05-extension-model.md),
[docs/09](../09-interfaces.md)) *before* the core is implemented, so
interface mistakes are corrected on paper. Findings below were folded into
the specs in the same change.

## Fit table

| Capability | Media | Channels | Consumers | Verdict |
|---|---|---|---|---|
| `fjarr.camera` | N declared tracks, per-session valve | control | peer | **fits**; needs F1 clarification (declared vs active tracks) |
| `fjarr.desktop` | per-monitor tracks, count known only at runtime | control + realtime (pointer) | peer | **fits with F1**: monitors resolved at `session_attached`, manifest declares capacity |
| `fjarr.telemetry` | — | control | peer + backend | **gap F2**: no backend-consumer entry point existed on `Capability` |
| `fjarr.files` | — | control + bulk | peer + backend | **gap F3**: channel-sender/backpressure surface was undefined |
| `fjarr.terminal` | — | control (PTY I/O) | peer | **fits**; worker-pool + detach semantics cover PTY lifecycle |
| `fjarr.observability` | — | control (backend stream) | backend | **gap F2** (same as telemetry); config schema covers intervals |
| `fjarr.ota` | — | control + bulk (delta artifacts) | backend | **gap F4**: depends on `fjarr.files` backend mode — inter-capability dependency was unspecified; accept→feedback→result fits |
| `com.example.arm-teach` (stress test) | — | realtime 100 Hz bidirectional + control | peer | bidirectional DCs fit; privilege grant fits; custom view fits; **gap F4** (waypoint file via `fjarr.files`) |

## Findings and resolutions

### F1 — Declared vs active tracks *(clarification)*

`CapabilityManifest.tracks` looked static, but desktop monitors (and
hot-plugged cameras) are known only at runtime. **Resolution**: the manifest
declares track *capacity* (identity + kind); the concrete per-session track
set is provided by the capability at `session_attached` and frozen into that
session's offer manifest. `track_id`s remain stable per docs/08. →
[docs/05](../05-extension-model.md#the-capability-contract-agent-side).

### F2 — Backend consumer had no entry point *(interface addition)*

`Capability` only had session-scoped hooks, but telemetry/observability/OTA
converse with the backend outside any operator session. **Resolution**: a
`BackendContext` (session-independent, envelope send/receive over the
agent's signaling connection, store-and-forward aware) plus optional
`backend_attached` / `backend_detached` / `on_backend_message` hooks with
default no-op implementations — peer-only capabilities are unaffected. →
[docs/09](../09-interfaces.md#the-capability-interface-agent-side).

### F3 — Channel sender and backpressure surface *(definition)*

`fjarr.files` cannot be written against an undefined send API, and
backpressure is a spec-level requirement (docs/08#backpressure).
**Resolution**: `ChannelSender` defined now — envelope + binary send,
`buffered_amount()`, low-watermark drain callback — exposed via
`SessionContext`/`BackendContext`. → [docs/09](../09-interfaces.md).

### F4 — Inter-capability dependencies *(direction set, binding deferred)*

OTA consumes `fjarr.files`; arm-teach persists a waypoint file through it.
**Resolution (direction)**: `CapabilityManifest.dependencies` lists required
capability names; the core validates presence at registration and will
expose a typed handle (`ctx.capability("fjarr.files")`). The handle's API is
**deliberately deferred to M4** when `fjarr.files` defines its service
surface — recorded in [open questions](../18-open-questions.md). Declaring
the field now prevents an ABI break later.

## Conclusion

No structural rework required: the core model (manifest + lifecycle +
namespaced envelopes + declared channels + consumer kinds) survived all
eight walk-throughs. Four amendments landed (two additive interface
changes, one clarification, one deferred-but-declared field). The
implementation may proceed against the amended docs/05 + docs/09.
