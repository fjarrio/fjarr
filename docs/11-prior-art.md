---
title: Prior Art
description: Lessons mined from the camera streamer, teleop-car, and fleet daemon — adopt the proven, fix the flawed.
---

Five earlier in-house projects were analyzed in depth before Fjarr's specs
were written. **They are referred to by descriptive codenames only** — the
projects, their owners and their source trees are deliberately not
identified, and the code itself is not published (it lives in the
gitignored `inspiration/` folder on maintainers' machines). Per project:
**Adopt** (proven, reuse) and **Fix** (documented in specs as requirements,
not repeated). They are inspiration, not obligation.

## The camera streamer {#camera-streamer}

C++/GStreamer/`webrtcbin` ROS 2 camera streamer (NVIDIA Jetson, stereo cameras);
"worked reliably" in the field. ~5.9k LOC.

### Adopt

| Pattern | Where it lands in Fjarr |
|---|---|
| **v3 producer/consumer split** — persistent producer (capture→encode→`appsink`) → `FrameHub` fan-out (weak_ptr subscribers, per-source ring) → disposable per-peer `appsrc ! queue leaky=downstream ! valve ! rtp*pay ! webrtcbin` | [docs/02](02-architecture.md#the-media-model-produce-once-fan-out); N viewers = 1 encoder |
| RAII kit: `GstElementPtr` et al., `SignalConnection`, `ScopeGuard` (~200 uncoupled lines) | copied into `libfjarr` core |
| **Generation-counter callback contexts** + `post_to_owner()` single-loop marshaling | required idiom, [docs/09](09-interfaces.md) — the use-after-free defense |
| **Caps-gated offers** (pad probe waits for fixed pt+ssrc on every enabled track; `offer_creation_started` latch) | required; kills the classic webrtcbin race |
| `valve drop` stream toggle without renegotiation; keyframe-gate + PTS rebase on join; explicit `force-IDR` on connect/enable | `fjarr.camera` + `fjarr.desktop` specs |
| **Recovery ladder**: peer-local teardown → 5 s DC ping/pong → 15 s negotiation watchdog fed by named progress milestones → supervised process restart only after 5 consecutive failures | agent core lifecycle |
| Track manifest in the offer; per-second `bandwidth-stats` | [docs/08](08-protocol.md#track-manifest) |
| DOT pipeline graphs published at stable lifecycle stages | agent debug endpoint; `graphviz` in the devcontainer |
| MQTT **last-will** as "peer gone" | generalized to server-side `peer-gone` ([docs/08](08-protocol.md#signaling)) |
| Element-availability preflight (`validate_codec_elements`) | doctor + agent startup checks |

### Fix

- **Zero congestion control / adaptive bitrate** (fixed CBR 2 Mbps): hard
  requirement in [docs/16](16-performance-budgets.md); drives
  [ADR-0007](adr/0007-webrtcbin-vs-webrtcsink.md) (`webrtcsink` ships GCC
  natively).
- Shipping path was software VP8 480×320@15 — HW encode was written but never
  enabled. Fjarr's VA-API path is doctor-verified from M0.
- Single hardcoded TURN URL with embedded credentials → [docs/10](10-security.md#turn).
- No audio; ROS topic→JSON bridging hand-written per topic → adapter seam.

## The teleop car {#teleop-car}

Browser + Python/aiortc RC car over MQTT signaling; coturn + haproxy infra.
Memorable UX, but most durable value is in the anti-patterns.

### Adopt

`client_id` request/response correlation; gather-then-send offer helper;
robot discovery via retained metadata + timestamp liveness eviction; gamepad
input shaping (wheel/pedal normalization, brake multiplier, Ackermann
angular-velocity conversion, WebGL predicted-path overlay — nice for teleop
capabilities later).

### Fix (now spec requirements)

| Anti-pattern found | Requirement it produced |
|---|---|
| No PC state machine; status LEDs read `pc.connectionState` from refs during render → never re-render | `@fjarr/core` exposes a **reactive state machine — state, not refs** ([docs/09](09-interfaces.md#core-framework-agnostic)) |
| No trickle ICE (blocked on full gathering), no `restartIce()`, no reconnect | trickle REQUIRED; reconnect + ICE restart built into core ([docs/08](08-protocol.md#signaling)) |
| All control on one default reliable-ordered DC at 3×50 Hz JSON (incl. resending "beep off" 50×/s) | reliability classes per channel; coalescing; [docs/08](08-protocol.md#datachannel-topology) |
| TURN user/password compiled into the frontend; `lt-cred-mech` static user; anonymous MQTT — anyone could drive the car | ephemeral HMAC TURN creds; authenticated signaling; no secrets in bundles ([docs/10](10-security.md)) |
| Robot's 1 s deadman timer silently regressed (dead code) | safety behaviors are spec'd + tested, never incidental ([docs/15](15-testing-strategy.md#safety-behaviors)) |
| Infra memory: haproxy basic-auth + Cloudflare TLS, coturn on host networking | TURN capacity/ports documented honestly ([docs/04](04-supported-platforms.md#network-requirements)) |

## The fleet daemon {#fleet-daemon}

Python/gRPC robot↔backend daemon in production across a large customer fleet. The
control-plane teacher.

### Adopt

| Pattern | Where it lands |
|---|---|
| One bidi stream, one `Event` envelope (`oneof`) | the [envelope](08-protocol.md#envelope) |
| `event_id` correlation with **accept → feedback\* → result** for long ops | protocol-wide; file transfer + OTA are built on it |
| Store-and-forward with a **persistence whitelist** (durable telemetry yes; responses/signaling/heartbeats never) | agent store-and-forward ([docs/06](06-capabilities.md#fjarrtelemetry--sensortelemetry-streaming-m4)) |
| Flap-resistant backoff: pure functions, ×2 to 30 s cap, ±20 % jitter, **reset only after 30 s stable** | copied verbatim ([docs/08](08-protocol.md#signaling)) |
| Change-triggered telemetry with a rate floor (2 s heartbeat, 200 ms min gap) | telemetry + observability capabilities |
| Two-layer liveness; backend-connectivity state **republished into the robot** | agent exposes link state to the customer's stack |
| Hard translation boundary (pure codec / adapter / orchestration files) | the `RobotAdapter` seam; testable without a robot |
| **Fault-injecting mock backend as a first-class artifact** (incl. "go silent without disconnecting") | [docs/15](15-testing-strategy.md#fault-injection) |
| Session ownership as a narrow documented policy; **fail-open on stale locks** | [docs/10](10-security.md#session-ownership) |
| Signaling as opaque JSON inside a typed envelope | [docs/08 design rule 1](08-protocol.md#design-rules) |
| Control plane / media plane as separately supervisable processes | [docs/02](02-architecture.md#control-plane-vs-media-plane) |
| `{id, version}` pokes as change notifications | observability version reporting |

### Fix (now spec requirements)

- MAC-as-identity + fleet-shared bearer token → per-device enrollment
  ([docs/10](10-security.md#device-identity)).
- Unbounded SQLite offline store (no cap/eviction; blocking the event loop) →
  bounded queue, oldest-first eviction, drop counter, off-loop I/O.
- Strict oldest-first replay (8 h of stale telemetry before anything current)
  → live lane + backfill lane.
- Arbitrary shell over a host FIFO as the update mechanism → never; OTA is
  SWUpdate with signed artifacts ([ADR-0016](adr/0016-swupdate-ota.md)).
- `deploy.py` OTA: no A/B, no rollback, abort-unsafe mid-flash, self-replacing
  via `sleep 10 && …` → the entire `fjarr.ota` design brief.
- Handlers replying `accepted=true` while `pass # TODO` → a response MUST
  mean the thing happened ([docs/13](13-development-workflow.md)).
- Schema submodule drift breaking imports → protocol schemas versioned as a
  published artifact, codegen in CI ([docs/08](08-protocol.md#versioning)).
- Uniform error swallowing in the reconnect loop (`UNAUTHENTICATED` retried
  like `UNAVAILABLE`) → error taxonomy distinguishes retryable from fatal.

## The fleet dashboard {#fleet-dashboard}

React 19 fleet dashboard consuming the camera streamer; the operator-side
counterpart, analyzed for the slice-2 web design ([docs/21](21-web-client-architecture.md)).

### Adopt

| Pattern | Where it lands |
|---|---|
| **Session in an app-root context above the router** (they migrated away from a per-page `OldRunProvider`) — navigation never drops the connection; header controls it from anywhere; a floating overlay follows the user | docs/21 sessions: `FjarrClient` + `SessionManager` at the root, sessions owned by the client, never by components |
| `subscribe(type, cb) → unsubscribe` pub-sub; other providers/components subscribe to slices | docs/21 three delivery modes; envelopes keyed by `(cap, type)` |
| **Two data tiers**: slow state through React state, high-rate telemetry through refs read on rAF (`implementOdometryRef` → `ImplementAnimator`) | docs/21 `useTelemetry` (state) vs `useLatest` (ref) — first-class, not ad hoc |
| Tracks stored independent of `<video>` elements; late-mounting elements `reattach()`; components add themselves to the "used" set → `receiver-select-streams` | docs/21 demand model: reference-counted `acquire/release`, visibility-aware, coalesced `select-tracks` |
| Track identity resolved via `transceiver.mid` (after parsing SDP to recover it) | fixed at the source: `mid` in the manifest (docs/08) |
| Isolated heartbeat controller with session-id guard; graceful disconnect with ack + timeout; `beforeunload` warning while connected; "reconnect last robot" | docs/08 heartbeat; docs/21 host-app conveniences (opt-in) |

### Fix (now spec requirements)

- 1,825-line provider with a ~20-field context value → every consumer
  re-renders on every status tick. → stable client in context, all
  reactivity via store selectors (`useSyncExternalStore`).
- Robot domain (`systemHealth`, `battery`, localized toasts, route ids) baked
  into the connectivity layer. → library has no robot vocabulary; domain
  providers are built on `useTelemetry` in the host app.
- All tracks enabled by default until something mounts; video refs must be
  "ready" at connect time. → default nothing enabled; tracks fully decoupled
  from elements.
- One session (`connectedCamera-streamerId`) as a global. → N sessions per client,
  explicit handles everywhere.
- Reconnect by toggling disconnect/connect; no `restartIce`; MQTT signaling
  filtered client-side; static TURN creds from build env. → docs/21 state
  machine, docs/08 signaling with per-session TURN creds.
- No publish abstraction (raw `send(type, msg)` on one reliable channel). →
  docs/21 publishing table: channel class from the capability, newest-wins
  realtime publishers, deadman re-publish.

## The receiver playground {#receiver-playground}

A small React playground receiver for the camera streamer (`inspiration/receiver-playground`). Two ideas worth more than its size:

### Adopt

| Pattern | Where it lands |
|---|---|
| `subscribeToStream(id, handler)`: N `<VideoStream>` components receive the *same* `MediaStream` — one RTP track, decoded once, rendered many times, no extra bandwidth; late subscribers get the existing stream immediately | docs/21 track registry: fan-out is a guarantee, demand aggregates per track |
| `StreamSelector` stats panel: 1 s `getStats()` polling, `inbound-rtp` bytes/packets/loss/jitter/PLI/FIR/NACK/frames, candidate-pair RTT, bitrate from byte deltas, a four-level quality label with loss/jitter/RTT thresholds | docs/21 stats & health: sampler in core, per-track by `mid`, selected-pair RTT + relay detection, decode/freeze/HW-decoder metrics, windowed rates, health score with reasons + hysteresis, thresholds from docs/16 |
| Buffer ICE candidates until the remote description is set | docs/21 state machine (also in the camera streamer) |

### Fix

- Stats averaged/summed across all tracks (per-track view lost); RTT
  averaged over every succeeded candidate pair rather than the selected
  one; lifetime counters shown where rates are needed.
- The stats interval lives in a React effect that depends on the previous
  sample — it tears down and recreates the timer on every tick.
- All streams enabled by default (`fill(true)`), a synthetic "main" stream
  aggregating every track, and stream identity guessed from
  `event.streams[0].id` or `mid` — replaced by manifest `mid` (docs/08) and
  demand-driven enablement.
