---
title: Web Client Architecture
description: Design of @fjarr/core and @fjarr/react — sessions that follow the user, subscription tiers, and demand-driven media delivery.
---

> **Status: review** — the slice-2 design, written before implementation
> (docs/13); implemented in `web/packages/core` + `web/packages/react`
> (slice 2, 2026-09-16), unit-tested against the `@fjarr/core/testing`
> mock agent and reviewed retrospectively
> ([slice-2 review](reviews/slice-2-review.md)). Inspired by the fleet dashboard's session provider
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
page therefore never tears a session down (the fleet dashboard's core
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
| connected | ICE `disconnected` (past a 3 s grace) or `failed` | reconnecting | send `ice-restart` over signaling; the agent re-offers; keep tracks/consumers attached |
| connected | heartbeat 3× missed / signaling socket lost / no re-offer within 10 s | reconnecting | new signaling round via same grant (if unexpired) |
| reconnecting | recovered | connected | re-flush demand (agent keyframes on enable) |
| reconnecting | attempts exhausted | failed | consumers see `failed` and an `error` event (`reconnect-exhausted`); `retry()` available |
| reconnecting | `retry()` | connecting round now | skips the remaining backoff (a host "reconnect now" button) |
| any | `peer-gone` / `session-close` from server | closed (reason) | release tracks, keep subscriptions registered for a possible `open()` again |
| connected / reconnecting | `session-close{retry:true}` (agent media restart, ICE-restart fallback on GStreamer 1.24) | reconnecting | a counted round, started at once — the agent asked for a fresh session |
| any | `error(grant-expired)` | reconnecting | refetch grant via provider, then retry immediately — the first refresh per attempt is free (no backoff, not counted); a second consecutive `grant-expired` is an ordinary backed-off round, so a host minting rejected tokens can never hot-loop; never a generic failure |
| any | `error(session-unknown)` | closed (reason) | the server no longer knows our session (a message crossed `peer-gone` on the wire): an orderly close, not a failure |

The attempt budget (default 5 rounds) is renewed by a connection that stayed
up for 30 s, the same rule the docs/08 backoff uses; a connect timeout
(default 15 s, grant fetch included) bounds every round so a hanging backend
or a socket that never opens still climbs the ladder.

The rungs are the [docs/08 reconnection ladder](08-protocol.md#reconnection);
backoff is docs/08's (0.5 s ×2, cap 30 s, ±20 % jitter, reset after 30 s
stable). All of this is core logic, unit-tested against a fake transport
and a fake peer connection (docs/15) — the dashboard's ad-hoc `toggle →
cleanup → start` is the anti-pattern.

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
| Byte stream (terminal input, clipboard payload) | `session.channel(cap)` → `.write(bytes)` | the capability's **bulk** channel (`fjarr:bulk:<cap>`, reliable-ordered — [docs/08](08-protocol.md#datachannel-topology)); bytes written before the channel opens are queued (bounded) | ordered; `bufferedAmount`/`onDrain` exposed; incoming bytes via `.onData(cb)` |
| Large binary (file upload) | `session.bulk(cap)` → `.sendFrames(iter)` | bulk (same DC) | docs/08 backpressure: pumps while below HIGH_WATER, resumes on `bufferedamountlow` |
| Lossy binary frames (point clouds, depth, custom sensors — either direction) | `session.stream(cap)` → `.send(frame)` / `.onFrame(cb)` | **stream** (unordered, no retransmit, binary; [ADR-0018](adr/0018-stream-channel-class.md)) | frame-level newest-wins with sequence numbers; chunked to the SCTP message limit; consumers get whole frames or nothing. **Lands in M4** with the first stream-class capability ([roadmap](17-roadmap.md#m4--files-telemetry-logs-sensors)); slice 2 ships the surface without the chunker |

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

**Renegotiation-safe.** A new offer mid-session ([docs/08](08-protocol.md#renegotiation))
is *diffed* against the registry by `track_id`: new tracks are added
(pending until their `RTCTrackEvent`), removed tracks are released and their
consumers see `status: "unavailable"` while keeping their handles, and
untouched tracks are left strictly alone — no element re-attach, no
flicker. Demand is re-flushed only for tracks whose entry changed. Offers
with a `manifest_version` older than the applied one are ignored.

**Fan-out is free.** Any number of consumers (a grid tile, a floating
overlay, a picture-in-picture, a canvas overlay) attach to the *same*
track: the browser receives and decodes each RTP stream exactly once
regardless of how many elements render it, so a second tile costs zero
network bandwidth and zero decode — only compositing (the
receiver playground's `subscribeToStream` idea, [prior art](11-prior-art.md#receiver-playground),
made a guarantee of the registry). Demand is therefore *aggregated* per
track, never duplicated: ten consumers of `cam-front` produce one enabled
track at the highest tier any of them asks for.

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
consumers`, `preference = sharpness if any visible consumer asks for it`.
Changes are debounced (~250 ms) and coalesced into one `select-tracks`
request **per track-owning capability** (`fjarr.camera` for camera tracks,
`fjarr.desktop` for monitors — [docs/06](06-capabilities.md#fjarrcamera--camera-video-m1-reference-implementation));
a demand change touching both produces two requests, never one per consumer.
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

### Audio tracks

Audio is a track like video (`kind: "audio"` in the manifest, docs/08),
served by the `fjarr.audio` capability ([docs/06](06-capabilities.md#fjarraudio--two-way-audio-planned)):

- **Downlink** (hear the robot's surroundings, machine sounds): acquired
  through the same demand model — `useAudioTrack(session, trackId)` /
  `<AudioSink>`; default off; per-consumer mute. Browsers refuse to play
  unmuted audio without a user gesture, so `<AudioSink>` exposes
  `status: "blocked-autoplay"` and an `unlock()` to call from a click; the
  library never fakes a gesture.
- **Uplink** (talk to a person at the robot): the operator's microphone is a
  *published* media track. Because the agent always offers (docs/08), the
  agent pre-allocates a `recvonly` audio transceiver in the initial offer
  whenever the grant includes `fjarr.audio` with `talk: true`; the browser
  attaches its mic with `RTCRtpSender.replaceTrack()` — **no renegotiation**.
  `usePushToTalk(session)` is the default UX (open mic is an explicit
  opt-in), echo cancellation/noise suppression flags pass through to
  `getUserMedia`, and every uplink start/stop is a session event for audit
  (docs/10 — a live microphone is as sensitive as a terminal).

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

## Stats and connection health

`RTCPeerConnection.getStats()` is the ground truth for what the network is
doing; the dashboard tooling around it was the best part of the
receiver playground ([prior art](11-prior-art.md#receiver-playground)) and is
redesigned here as a core service rather than a component's `setInterval`.

- **Sampler in core, one per session** (default 1 s, configurable), publishing
  into the telemetry store so consumers use mode-1 selectors
  (`useTrackStats(session, trackId)`, `useSessionStats(session)`) — no timers
  in React, no re-created intervals.
- **Per-track, not averaged.** `inbound-rtp` reports are keyed by their
  `mid` → `track_id` via the manifest. Per video track: bitrate (windowed,
  from byte deltas), packets lost (cumulative *and* windowed loss rate),
  jitter, jitter-buffer delay, frames decoded/dropped/per-second, resolution,
  freeze count and duration, key frames, PLI/FIR/NACK counts, average decode
  time, `decoderImplementation` + `powerEfficientDecoder` (is hardware
  decode engaged?). Per audio track: level, concealed samples/events.
- **Transport**: RTT from the **selected** candidate pair only (not an
  average over all pairs), `availableIncomingBitrate`, and the pair's
  local/remote candidate types — so the UI can say *"relayed via TURN"*
  versus *"direct"*, which explains most latency complaints on the spot.
- **Outbound** (published audio, future uplinks): bytes sent,
  `qualityLimitationReason`, retransmissions. **Data channels**: per-channel
  messages/bytes in and out, `bufferedAmount`.
- **Agent correlation**: the agent's `bandwidth-stats` envelope (docs/06)
  gives *sent* bytes per track; sent − received over the same window is the
  relay/path loss picture the latency harness (docs/15) plots.
- **Health score with reasons, not a color.** `useSessionHealth(session)`
  yields `{ level: "good" | "degraded" | "poor", reasons: ["loss 6% > 5%",
  "rtt 340 ms > 300 ms"] }` with thresholds taken from the
  [performance budgets](16-performance-budgets.md#connection-health-thresholds)
  and hysteresis (a level changes only after three consecutive samples
  agree) so a single bad second doesn't flap the badge.
  `<ConnectionQuality>` renders it; hosts can render their own from the
  same hook.

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
| `<AudioSink>` / `usePushToTalk` | downlink audio with autoplay-unlock status; PTT uplink | — |
| `useSessionPeers` / `<SessionPeers>` | who else is on this robot and who owns input (docs/08 `session-peers`, M5) | — |
| `useGamepadHaptics` | maps capability `haptic` events to the Gamepad vibration actuator | — |
| `<ConnectionQuality>` | health level + reasons from the stats sampler | `StreamSelector` stats panel (generalized) |
| `<DesktopView>` | the remote-desktop surface — design in [docs/22](22-remote-desktop-client.md), which also lists the core requirements slice 2 must satisfy for it | — |
| `<TerminalView>` | capability view (M2), registered via `registerCapabilityView` | — |
| `<PipelineGraph>` / `usePipelines` | live GStreamer pipeline graphs from `fjarr.introspect` ([docs/24](24-pipeline-introspection.md)); d3-graphviz rendering, history scrubbing (slice 5) | — |

Third-party capabilities register views with the same registry
([docs/05](05-extension-model.md#web-side-capability-components)).

## Capability-specific requirements on the core

Some capabilities need small hooks in the core that are cheap now and
breaking later. Slice 2 provides them up front:

- **Remote desktop** ([docs/22 core requirements](22-remote-desktop-client.md#core-requirements-for-slice-2-so-m3-needs-no-core-change)):
  track acquire options `preference` + `latencyMode` (→ `select-tracks`,
  `jitterBufferTarget`), a page-level keyboard **focus registry**, cursor
  events on the realtime class, `useTimeSync`.
- **Terminal**: an ordered byte channel with `onDrain` (publishing table).
- **Audio**: autoplay-unlock status and `replaceTrack` uplink on a
  pre-allocated transceiver (media section).

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
- **Stats sampler** unit tests against recorded `getStats()` snapshots:
  per-track keying by `mid`, windowed deltas, selected-pair RTT, relay
  detection, health reasons + hysteresis.
- **Multi-session** tests: three sessions on one client with independent
  state machines; a fault injected on one leaves the other two untouched.
- **Browser e2e** in the [browser lab](25-browser-lab.md): first against
  the in-browser **loopback agent** (a real `RTCPeerConnection` pair in the
  page — autoplay, `srcObject`, IntersectionObserver, `getStats`,
  `replaceTrack`, renegotiation and ICE restart on a real WebRTC stack),
  then against demo-robot via the demo stack once it streams. The core's
  **wire tap** (`client.on("wire", …)`) makes DataChannel traffic
  observable there, since CDP cannot see inside SCTP.
- All of the above except the browser e2e run in `make web-test` with no
  browser: `@fjarr/core/testing` provides the scripted socket, peer
  connection and `MockAgent` (docs/15) that host dashboards can reuse.

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
