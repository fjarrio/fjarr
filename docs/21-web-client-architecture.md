---
title: Web Client Architecture
description: Design of @fjarr/core and @fjarr/react — sessions that follow the user, subscription tiers, and demand-driven media delivery.
---

> **Status: review** — the slice-2 design, written before implementation
> (docs/13). Inspired by the fleet-dashboard's session provider
> ([prior art](11-prior-art.md#fleet-dashboard)): its good ideas are kept
> and pushed further; its structural problems are explicitly designed out.

## Goals

1. **A session follows the user.** Connecting to a robot is an app-level
   fact: navigate between pages, open/close panels, mount/unmount tiles —
   the WebRTC session stays up and every consumer re-attaches instantly.
2. **Consumers subscribe to slices, cheaply.** A video tile, a floating
   overlay, a 3D map, a status pane and a terminal each get exactly the data
   they need, at the delivery cadence they need, without re-rendering each
   other.
3. **Bandwidth follows demand.** A track is sent only while something on
   screen wants it, at the quality the largest visible consumer needs.
4. **Library, not app.** No robot vocabulary, no toasts, no routing, no
   storage assumptions inside the library; the host dashboard composes it.
5. **Many sessions, one page.** Connecting to three robots at once is one
   client with three independent sessions — separate state machines,
   heartbeats, track registries, TURN credentials — rendered side by side.
   There is no ambient "the connected robot" anywhere in the library: every
   hook and component takes an explicit session handle.
6. **Publish as easily as subscribe.** Teleoperation commands, terminal
   keystrokes, clipboard payloads and file bytes all flow *toward* the robot
   through the same typed, channel-class-aware API — with coalescing for
   lossy realtime data and backpressure for bulk.

Non-goals: replacing the host's state management, UI kit, or auth.

## Layering

```text
@fjarr/react   FjarrProvider · useSession · useSessionState · useMessage
               useTelemetry · useLatest · useVideoTrack · <VideoTile>
               <FloatingVideo> · <VideoGrid> · <SessionStatus> · <ConnectButton>
                     │ (thin: hooks over core stores via useSyncExternalStore)
@fjarr/core    FjarrClient ─┬─ SessionManager ── Session (state machine)
(zero deps)                 │        ├─ SignalingTransport (WSS, docs/08)
                            │        ├─ PeerConnection wrapper (trickle ICE,
                            │        │  restartIce, reconnect backoff)
                            │        ├─ EnvelopeRouter (cap/type pub-sub,
                            │        │  request → accept/feedback*/result)
                            │        ├─ TrackRegistry (manifest, MediaStreamTrack
                            │        │  by track_id, demand → select-tracks)
                            │        ├─ Heartbeat (docs/08, 5 s / 3 missed)
                            │        └─ Stores (state, telemetry, stats)
                            └─ GrantProvider (injected: host owns auth)
```

`@fjarr/core` is framework-agnostic and dependency-free (docs/14). Its
stores implement `{ subscribe, getSnapshot }` so React binds through
`useSyncExternalStore` with **no context value churn**: the context holds one
stable `FjarrClient`; everything reactive is a store selector.

## Sessions

### Placement

`<FjarrProvider client={client}>` mounts once, at the app root **above the
router**. Sessions are owned by the client's `SessionManager`, keyed by
`robot_id`; React components hold handles, never own sessions. Unmounting a
page therefore never tears a session down (the fleet-dashboard's core
insight, made structural rather than incidental).

```tsx
const client = createFjarrClient({
  serverUrl: "wss://fjarr.acme.com/ws",
  grant: (robotId) => myBackend.fetchGrant(robotId),   // host owns auth
});
<FjarrProvider client={client}><App/></FjarrProvider>
```

### Lifecycle and idle policy

```ts
const session = client.sessions.open("robot-024");   // idempotent
session.close("operator-closed");
```

`open()` returns the existing session if one exists; opening three robots
is three calls, each yielding an independent handle. Subtrees bind to one
robot with `<SessionScope session={s}>` so leaf components can call
`useSession()` without prop-drilling — but the scope is a convenience over
explicit handles, never a singleton (a page renders three scopes for three
robots). Optional
**idle policy**: `client.sessions.open(id, { idle: { closeAfterMs } })`
closes a session that has had zero consumers (no subscriptions, no
acquired tracks) for the grace period — the explicit choice replaces the
dashboard's implicit "connected until the user clicks disconnect".
Default: **no auto-close** (an operator who navigated away expects the
robot to still be there when they come back).

### State machine

States are exactly docs/09's: `idle → connecting → connected →
reconnecting → (connected | failed) → closed`. Transitions:

| From | Event | To | Action |
|---|---|---|---|
| idle | `open()` | connecting | fetch grant → WSS hello → await `hello-ack` (session_id, TURN creds) → `session-request` brokered by server → offer |
| connecting | offer | connecting | set remote, answer, trickle ICE both ways |
| connecting | DTLS up + control DC open | connected | start heartbeat; flush demand → `select-tracks` |
| connected | ICE `disconnected` | reconnecting | `restartIce()`; keep tracks/consumers attached |
| connected | ICE `failed` / heartbeat 3× missed | reconnecting | new signaling round via same grant (if unexpired) |
| reconnecting | recovered | connected | re-flush demand (agent keyframes on enable) |
| reconnecting | attempts exhausted | failed | consumers see `failed`; `retry()` available |
| any | `peer-gone` / `session-close` from server | closed (reason) | release tracks, keep subscriptions registered for a possible `open()` again |
| any | `error(grant-expired)` | reconnecting | refetch grant via provider, then retry — never surfaces as a generic failure |

Reconnect backoff is docs/08's (0.5 s ×2, cap 30 s, ±20 % jitter, reset
after 30 s stable). All of this is core logic, unit-tested against a fake
transport (docs/15) — the dashboard's ad-hoc `toggle → cleanup → start` is
the anti-pattern.

## Subscriptions: three delivery modes

Different consumers need different cadences. Forcing everything through
React state is the dashboard's main structural cost (its 20-field context
value re-renders every consumer on every status tick). The library offers
three modes over one router:

| Mode | API | Re-renders? | For |
|---|---|---|---|
| **State** | `useTelemetry(session, selector, equals?)` | on selected-value change only | status panes, badges, buttons |
| **Stream** | `session.on(cap, type, handler)` / `useMessage(session, cap, type, handler)` | never | logging, toasts, one-off events, side effects |
| **Latest ref** | `useLatest(session, cap, type)` → `{ current }` | never | rAF consumers: 3D maps, gauges, canvas overlays |

The router keys envelopes by `(cap, type)` per docs/08; `useTelemetry`'s
selector reads from a per-session **telemetry store** that keeps the latest
envelope per `(cap, type[, key])` — *newest-wins*, so a burst of 100 Hz
joint states never queues. Equality defaults to `Object.is`; pass a shallow
comparator for objects.

Requests use docs/08 correlation:

```ts
const result = await session.request("fjarr.camera", "select-tracks", payload);
for await (const fb of session.requestStream("fjarr.files", "file-offer", payload)) { … }
```

`request()` resolves on `result`, rejects with a typed `FjarrError(code)`;
`requestStream()` yields `accept`/`feedback*` then returns the `result`.

**Domain state stays in the host.** The library ships no
`systemHealth`/`battery` concepts. A host builds its own
`RobotStatusProvider` on `useTelemetry` in ~30 lines; `demos/demo-dashboard`
demonstrates exactly that.

## Publishing: sending toward the robot

Subscribing is half the job; teleoperation, terminal input, clipboard and
files all *send*. The same router exposes a publish side, and the channel
class (docs/08#datachannel-topology) is chosen by the **capability
declaration**, never by the caller guessing:

| Need | API | Channel class | Semantics |
|---|---|---|---|
| One-off event/command | `session.send(cap, type, payload)` | control (reliable, ordered) | fire-and-forget envelope, `kind: "event"` |
| Command with outcome | `session.request(cap, type, payload)` | control | accept/feedback*/result correlation |
| Continuous lossy stream (joystick, pointer, joint targets) | `session.publisher(cap, type, { key?, maxHz })` → `.publish(payload)` | realtime (unordered, no retransmit) | **newest-wins per key**, rate-capped (default 60 Hz); a burst never queues, the latest value always goes out |
| Byte stream (terminal input, clipboard payload) | `session.channel(cap)` → `.write(bytes)` | control or bulk per declaration | ordered; `bufferedAmount`/`onDrain` exposed |
| Large binary (file upload) | `session.bulk(cap)` → `.sendFrames(iter)` | bulk (dedicated DC) | docs/08 backpressure: pumps while below HIGH_WATER, resumes on `bufferedamountlow` |

React bindings: `usePublisher(session, cap, type, opts)` returns a stable
`publish` function; `useCommand(session, cap, type)` wraps `request()` with
pending/error state for buttons.

**Safety tie-in (docs/15#safety-behaviors).** A realtime publisher can be
created with `{ deadman: { intervalMs } }`: while the consumer holds it, the
library re-publishes the last value at that interval even if the input is
idle, so the agent's deadman sees a live stream; releasing the publisher
stops the heartbeat and the agent's deadman fires within its budget. This
is how a joystick component unmounting (or a tab going hidden) stops a
robot without any component author remembering to send "zero" — the
regression that silently broke the teleop-car deadman ([prior art](11-prior-art.md#teleop-car)).

Worked example — a teleop stick:

```ts
const drive = usePublisher(session, "com.acme.teleop", "cmd_vel", {
  maxHz: 50, deadman: { intervalMs: 100 },
});
// gamepad loop:
drive.publish({ linear: y * maxSpeed, angular: -x * 1.25 });
```

and a terminal:

```ts
const pty = session.channel("fjarr.terminal");   // reliable-ordered per its manifest
term.onData((keys) => pty.write(encoder.encode(keys)));
```

## Media: demand-driven track delivery

The efficiency idea the dashboard got right — enable tracks only when a
video component is in the DOM — becomes automatic and richer.

### Track registry

On `offer`, the manifest (docs/08) populates a per-session registry:
`track_id → { label, cap, kind, codec, pt, mid, monitor }`. Incoming
`RTCTrackEvent`s map to `track_id` by **`transceiver.mid`** — the agent
includes `mid` in the manifest ([protocol amendment](08-protocol.md#track-manifest)),
so no SDP parsing or payload-type guessing is ever needed.
`MediaStreamTrack`s live in the registry, **independent of any element**:
a `<video>` that mounts later simply attaches to the existing track.

### Demand model

Every consumer declares what it needs; the registry folds demand into one
per-track decision and tells the agent:

```ts
const handle = session.tracks.acquire("cam-front", {
  tier: "active" | "thumbnail",   // quality it needs (docs/16 tiers)
  visible: true,                  // updated by the consumer as it changes
});
handle.update({ visible: false });
handle.release();
```

Per track: `enabled = any consumer visible`, `tier = max over visible
consumers`. Changes are debounced (~250 ms) and coalesced into a single
`fjarr.camera/select-tracks` request ([docs/06](06-capabilities.md#fjarrcamera--camera-video-m1-reference-implementation)).
Reconnection re-sends the full demand snapshot. **Default is nothing
enabled** — the dashboard's "all tracks on until told otherwise" wastes
bandwidth on every connect.

### Visibility

`useVideoTrack` (below) drives `visible` from `IntersectionObserver` on the
element plus `document.visibilityState`, with a short grace period so
scrolling past a tile doesn't flap the stream. Consumers that must keep a
track warm off-screen (a floating overlay minimized to a corner, a
picture-in-picture) pass `keepWarm: true`. Re-enabling gets a keyframe
from the agent (docs/06), so the first frame after scroll-back is
immediate.

### Hooks and elements

```ts
const { track, attach, status } = useVideoTrack(session, "cam-front", { tier: "active" });
// attach(el) sets el.srcObject = new MediaStream([track]) and calls play();
// status: "requested" | "streaming" | "disabled" | "unavailable"
```

`<VideoTile session trackId tier>` is the headless-first default: a
`<video muted playsInline autoPlay>` with the hook wired, a slot for
overlays, and `data-fjarr-status` for styling. Stats (`bandwidth-stats`
envelopes, per docs/08) are a telemetry store: `useTrackStats(session,
trackId)`.

## Components (`@fjarr/react`)

All headless-first: logic in hooks, minimal default styling, every visual
overridable; the host's design system wins (docs/05).

| Component | Responsibility | Dashboard ancestor |
|---|---|---|
| `<SessionStatus>` | reactive state chip; always re-renders on transition | `ConnectionIcon` |
| `<ConnectButton robotId>` | open/close with correct labels per state | `ConnectionButton` |
| `<VideoTile>` | one track, demand-managed | RunPage grid cells |
| `<VideoGrid>` | N tiles from the manifest; layout order/labels from a host-provided ordering (no hard-coded camera names) | `buildGridVideoTrackRows` |
| `<FloatingVideo>` | draggable/resizable/anchored overlay that persists across routes; `keepWarm` when collapsed | `FrontCameraOverlay` (generalized, robot-agnostic) |
| `<DesktopView>` / `<TerminalView>` | capability views (M3/M2), registered via `registerCapabilityView` | — |

Third-party capabilities register views with the same registry
([docs/05](05-extension-model.md#web-side-capability-components)).

## Host-app conveniences (opt-in, never automatic)

- **Persistence adapter**: `createFjarrClient({ persistence })` with
  `{ get, set }` so the host can remember the last robot (localStorage,
  URL, their store) — the library never touches `localStorage` itself.
- **`useBeforeUnloadWhileConnected()`** — the dashboard's warning-on-close,
  as a hook the host mounts if it wants it.
- **Events, not toasts**: `client.on("session-event", …)` emits typed
  lifecycle/errors; how they're shown is the host's job.

## Testing (docs/15)

- **Golden fixtures against TS types** — closes [open question #15](18-open-questions.md):
  every `protocol/fixtures/valid/*.json` must parse into `@fjarr/core`'s
  `Envelope`/`Message` types; invalid ones must be rejected by the runtime
  guards (a `vitest` test next to `check.mjs`).
- **State machine** unit tests with a scripted fake transport: every row of
  the transition table, plus the fault menu (silent server, `grant-expired`,
  `peer-gone`, ICE `failed`).
- **Demand model** unit tests: fold/debounce/coalesce, visibility flapping,
  reconnect re-flush.
- **Publisher** unit tests: newest-wins coalescing under burst, rate cap,
  deadman re-publish while held and silence after release, bulk pump
  honoring `bufferedAmount` watermarks.
- **Multi-session** tests: three sessions on one client with independent
  state machines; a fault injected on one leaves the other two untouched.
- **Browser e2e** (Playwright, headless Chromium) against robot-sim via the
  demo stack: video renders, track toggles within budget.

## Decisions folded into this design

- External stores + `useSyncExternalStore` instead of a state library —
  keeps `@fjarr/core` dependency-free and React-version-agnostic
  (zustand-style ergonomics without the dependency).
- Multi-session from day one; single-session apps simply open one.
  `<SessionScope>` is sugar over explicit handles, never a global.
- Publish side mirrors subscribe side; channel class comes from the
  capability manifest, coalescing/backpressure from the channel class.
- Demand-driven delivery with visibility is on by default; `keepWarm` is
  the escape hatch.
- Agent supplies `mid` in the manifest (protocol amendment, docs/08).

Decided with the maintainer (2026-09-16): idle policy defaults to **never
auto-close** (hosts opt into `closeAfterMs`); `<VideoGrid>` **ships in
`@fjarr/react`**, headless-first, with host-provided ordering. The 250 ms
demand debounce is an initial value to be tuned against the latency
harness.
