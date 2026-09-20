---
title: Extension Model
description: The capability/plugin API — how everything user-visible plugs into the Fjarr core.
---

> **The centerpiece spec.** Fjarr's core knows nothing about cameras,
> desktops, or files — everything user-visible is a **capability plugin**.
> This API is designed on paper against all seven planned capabilities
> ([docs/06](06-capabilities.md)) *plus one deliberately invented stress-test*
> before any is implemented, and is only called "generic" once two unrelated
> capabilities ship against it unchanged ([roadmap](17-roadmap.md), M1–M2).

## What the core provides

Per tier, the core owns exactly the transport substrate:

- **Agent core** (`libfjarr`): signaling client, session lifecycle, peer
  connection, FrameHub, DataChannel router, store-and-forward queue, config,
  logging/DOT debug.
- **Server core** (`fjarr-signaling`): session brokering, grant verification,
  webhooks, TURN minting, metering, tenancy.
- **Web core** (`@fjarr/core`): session state machine, signaling client,
  track/DC demux; `@fjarr/react` renders it.

Capabilities never touch sockets, SDP, or ICE.

## The capability contract (agent side)

A capability is a named unit registered with the agent. Authoritative C++
signatures live in [docs/09](09-interfaces.md); semantically, a capability:

1. **Declares identity**: reverse-DNS name (`fjarr.camera`, `fjarr.desktop`,
   `com.acme.arm-teach`) + semver. The name prefixes every protocol surface
   it owns.
2. **Declares needs** at registration:
   - media tracks it can produce (label, kind, encoder requirements). The
     manifest declares track **capacity** (identity + kind); the concrete
     per-session track set is provided by the capability at
     `session_attached` (monitors and cameras are runtime facts) and frozen
     into that session's offer manifest with stable `track_id`s
     ([M1 API-fit review, F1](reviews/m1-api-fit-review.md)); the set may
     change mid-session (`SessionContext::update_tracks`, e.g. monitor
     hot-plug) — the core coalesces changes into one serialized
     renegotiation and keeps unchanged tracks flowing
     ([docs/08](08-protocol.md#renegotiation));
   - **dependencies**: names of other capabilities it requires (e.g.
     `fjarr.ota` → `fjarr.files`). The core validates presence at
     registration; the typed inter-capability handle is deferred to M4
     ([F4](reviews/m1-api-fit-review.md)) — declaring the field now avoids
     a later ABI break;
   - DataChannel classes it needs (reliability per [docs/08](08-protocol.md));
   - config schema (JSON Schema — validated by the core, surfaced to
     integrators);
   - required privileges (e.g. `uinput`, filesystem paths) — granted
     explicitly by integrator config, never assumed.
3. **Receives lifecycle calls**: `configure(config, sources)` (the
   source factory resolves `source = …` values, docs/09) → per-session
   `session_attached(session, granted_params)` / `session_detached` →
   `shutdown`. All calls arrive on the core's main loop; capabilities must
   not block it (the core provides a worker-pool handle).
4. **Exchanges messages** through the DC router: it sees only envelopes
   addressed to its namespace (`fjarr.camera/select-tracks`); replies are
   correlated by `event_id` with the accept→feedback→result triple for long
   operations.
5. **Is gated by the session grant**: a session only sees capabilities its
   grant lists; the core enforces this before the plugin is ever attached.

## Consumers: peer or backend

A capability declares which consumer kinds it serves:

- **Peer consumer** — a P2P-connected operator client (camera video, remote
  desktop, file transfer, terminal).
- **Backend consumer** — the `fjarr-server`/Cloud itself, over the agent's
  signaling connection (observability ingest, OTA orchestration). Backend
  messages use the *same envelope format* on a reserved stream, so a
  capability can serve both (file transfer works P2P for an operator and
  backend-driven for log collection).

This distinction exists **from day one** so observability/OTA (M7/M8) need no
core change.

## Web-side capability components

Each capability ships its dashboard counterpart as an entry in
`@fjarr/react`'s registry keyed by the same reverse-DNS name:

- a React component (or several) receiving `{ session, capability }` props
  (`CapabilityViewProps` in `@fjarr/react`);
- a headless hook layer in `@fjarr/core` for teams building their own UI;
- type definitions for its envelope messages (generated from the protocol
  schemas — [docs/08](08-protocol.md#versioning)).

Third-party capabilities register components at app start:
`registerCapabilityView("com.acme.arm-teach", AcmeArmTeachPanel)`.

## Adapter seams (not capabilities)

Adapters customize *how the core talks to its environment*:

| Seam | Tier | First implementation |
|---|---|---|
| `TelemetrySource` / `RobotAdapter` | agent | **ROS 2 adapter** — the core stays ROS-free (fleet-daemon lesson); adapters translate ROS topics into capability data |
| Video source | agent | one contract for every camera/screen/test source ([docs/09](09-interfaces.md#the-video-source-contract)); customers add sources by config string, by registering a type, or via a capability — no core change |
| Encoder adapter | agent | VA-API; Jetson `nvv4l2h264enc` later |
| Desktop backend | agent | per [docs/07](07-desktop-backends.md) — X11/Wayland/uinput behind one interface |
| Auth hooks | server | JWT verification; company SSO later |
| Signaling transport | agent | WebSocket ([ADR-0013](adr/0013-websocket-signaling.md)); MQTT adapter possible behind the same seam |

## The stress test: an invented sixth capability

To keep the API honest, [docs/06](06-capabilities.md#stress-test) specifies
**`com.example.arm-teach`** (a fictional third-party teach-pendant: bidirectional
low-latency joint streaming + a persistent per-robot file artifact + a custom
UI panel). Every API revision must answer: *could arm-teach be built with no
core patch?* If not, the API — not arm-teach — is wrong.

## Configuration & branding without forking

Integrators configure Fjarr with one declarative document (agent side:
`fjarr.toml`; dashboard side: props/provider config):

- which capabilities are enabled, their config-schema-validated settings;
- privilege grants (explicit allow-lists for paths, devices);
- branding surface on `@fjarr/react` (theme tokens, labels) — components are
  headless-first so the customer's design system wins.

## Compatibility rules

- Capability names are permanent; behavior changes bump the capability's
  semver, negotiated per session (both sides advertise; lowest common minor).
- The core↔capability ABI (C++) is **not** stable until M6; until then
  capabilities compile against the source tree (documented plainly).
- Protocol messages follow [docs/08 versioning](08-protocol.md#versioning).
