---
title: Agent Core Architecture
description: Design of the libfjarr core (C++/GStreamer) — process and threading model, object model, the agent-side session state machine, offer construction and renegotiation, the media plane, the DataChannel router, the concrete SessionContext/BackendContext APIs, configuration, supervision, and how it is tested.
---

> **Status: review** — the slice-3 design, written before implementation
> (docs/13), the agent-side counterpart of [docs/21](21-web-client-architecture.md).
> It turns the one-line idioms of [docs/02](02-architecture.md#the-media-model-produce-once-fan-out)
> and the [camera-streamer lessons](11-prior-art.md#camera-streamer) into
> the structures slice 3 implements, and fills in the two types docs/09
> left as forward declarations (`SessionContext`, `BackendContext`).
> Wire behaviour stays normative in [docs/08](08-protocol.md); the
> [webrtcbin spike](../agent/spikes/webrtcbin-probe/README.md) supplies the
> element-level facts this design relies on.

## Goals

1. **Capabilities never see sockets, SDP, or GStreamer negotiation.** They
   provide sources and handle envelopes; the core owns every session,
   pipeline and channel. A third-party capability is written against
   `SessionContext` alone.
2. **One loop, one owner.** Every mutation of core state happens on the
   core's GLib main loop. Streaming threads touch exactly one structure
   (the FrameHub) through one lock. Late callbacks from torn-down sessions
   are no-ops by construction (generation counters), not by luck.
3. **Produce once, fan out.** N operators cost one encoder per track and
   tier; session churn never touches capture.
4. **Recover in place, then escalate.** A single session failing never
   affects another; the media plane can be rebuilt without dropping the
   signaling connection; only repeated failure escalates to a supervised
   process restart.
5. **Every wire rule in docs/08 has one owner in the code** — the offer
   builder, the renegotiation queue, the DC router, the heartbeat — with a
   `// spec:` backlink, so the C++ never re-interprets the protocol.
6. **Testable without a robot or a browser.** Unit tests on a real GLib
   loop; integration tests against the real `fjarr-server` with a C++
   operator simulator that speaks the operator side of docs/08 and injects
   the docs/15 faults.

Non-goals for slice 3: any real capability (camera is slice 4), adaptive
bitrate (slice 6), the desktop backends (M2), per-device enrollment (M5),
store-and-forward (M7).

## Process model

The agent is **one process with two separately restartable subsystems**,
not two processes yet ([ADR-0019](adr/0019-agent-process-model.md)):

| Plane | Owns | Restartable without the other? |
|---|---|---|
| **Control plane** | signaling client, capability registry, session bookkeeping, backend bus, config, audit | it *is* the process; a crash here is a process restart |
| **Media plane** | the GStreamer producer pipelines, FrameHub, every per-session consumer pipeline and `webrtcbin` | **yes**: on an unrecoverable pipeline error the media plane is torn down and rebuilt; sessions are closed with `session-close(reason="media-restart")`, the signaling socket stays up, the robot stays *online* |

Process separation (a `fjarr-mediad` child) is the documented upgrade path
if driver hangs turn out to survive an in-process rebuild; the seam is the
media-plane interface below, so nothing in the capability API changes.

**Supervision contract** — systemd is required on shipped robots
([ADR-0019 addendum](adr/0019-agent-process-model.md)): the package ships
a `Type=notify` unit with `Restart=on-failure` and `WatchdogSec=30`;
embedders reach the same through `Agent::supervision()`; containers
without systemd run with the watchdog off and say so at startup:

| Exit code | Meaning | Supervisor action |
|---|---|---|
| 0 | clean stop (`stop()`, SIGTERM) | none |
| 1 | configuration/startup error (bad config, missing elements — the doctor's job at runtime) | do **not** restart in a loop; log loudly |
| 2 | "restart me": the recovery ladder is exhausted (docs/15 "agent SIGKILL mid-session" and "media plane hang" rows) | restart with the unit's backoff |

The daemon writes `READY=1` after the first `hello-ack` and `WATCHDOG=1`
from the core loop every `WatchdogSec/3`; a wedged core loop is therefore
killed by systemd, which is the only defense against a deadlock in our own
code — which is why the packaged agent does not run without it.

## Threading model

```text
 core loop (GMainContext, one thread)      streaming threads (GStreamer)
 ┌──────────────────────────────────┐      ┌────────────────────────────┐
 │ signaling client (libsoup async) │      │ producer: capture→encode   │
 │ session state machines           │      │   → appsink ──┐            │
 │ DC router → capabilities         │      └───────────────┼────────────┘
 │ offer builder / renegotiation    │                      ▼
 │ heartbeat, watchdogs, stats      │           FrameHub (one mutex, ring per track)
 │ post_to_owner() sink             │                      │
 └──────────────▲───────────────────┘      ┌───────────────┼────────────┐
                │ g_idle_add_full           │ consumer (per session/track)│
        worker pool (GThreadPool)          │ appsrc ← pull ─┘ → valve →  │
        capability long work only          │ payloader → webrtcbin       │
                                           └────────────────────────────┘
```

Rules, each of which the camera streamer learned the hard way:

- **All core state is loop-affine.** `Session`, `PeerConnection`,
  `ChannelRouter`, the registry, the config: read and written only from the
  core loop. There are no locks in the control plane.
- **`post_to_owner(fn)`** is the *only* way any other thread (GStreamer bus
  and signal callbacks, streaming pads, worker pool, libsoup) reaches core
  state: it schedules `fn` on the core loop with `g_idle_add_full`
  (`G_PRIORITY_DEFAULT`) and carries a **generation token**; the sink drops
  the call if the token's session generation has moved on.
- **Generation counters.** Every `Session` has a monotonically increasing
  `generation` bumped on every teardown. Every callback context registered
  with GStreamer (`g_signal_connect_data` user data, pad-probe data) is a
  small heap object `{ weak session ref, generation }` freed by
  `GDestroyNotify`; the callback compares generations before touching
  anything. This is what makes a disconnect storm safe.
- **Streaming threads own nothing.** An `appsink` `new-sample` callback
  does one thing: push the `GstSample` into the FrameHub under its mutex.
  An `appsrc` `need-data` callback pulls from the ring under the same
  mutex. No session state is visible from there.
- **Capabilities run on the core loop** (`session_attached`, `on_message`,
  …) and must return quickly; long work goes to `SessionContext::worker()`
  (a `GThreadPool`), whose completion is posted back with `post_to_owner`.
  A capability that blocks the loop trips the systemd watchdog — that is
  the contract, documented in docs/05.
- **Bus messages** are delivered on the core loop (`gst_bus_add_watch` on
  the core context); per-pipeline error handling lives in the media plane.

## Object model

```text
Agent (public, pImpl)
 └─ Core
     ├─ Config                       parsed TOML + env overrides (below)
     ├─ CoreLoop                     GMainContext/GMainLoop, post_to_owner sink, worker pool
     ├─ SignalingClient              libsoup-3 WebSocket, hello/backoff, message dispatch
     ├─ CapabilityRegistry           name → Capability, manifests, config validation, dependencies
     ├─ ModuleLoader                 dlopen of in-tree optional modules (desktop backends, ADR-0021)
     ├─ MediaPlane
     │   ├─ FrameHub                 track_id/tier → ring of encoded GstSamples, subscribers
     │   ├─ SourceRegistry            type name → VideoSource factory (built-in + customer-registered)
     │   ├─ Producer (per output/tier) VideoSource bin → tee → EncoderAdapter → appsink → FrameHub
     │   └─ ConsumerPipeline (per session) appsrc/valve/payloader per track + one webrtcbin
     ├─ SessionManager               session_id → Session; ownership leases (docs/10)
     │   └─ Session
     │       ├─ generation, state, operator, granted capabilities
     │       ├─ PeerConnection       webrtcbin wrapper: offer builder, renegotiation queue,
     │       │                       ICE, DTLS/connection state, get-stats sampler
     │       ├─ ChannelRouter        DataChannels by class, envelope parsing, fjarr.core,
     │       │                       dispatch to capabilities, ChannelSender impls
     │       ├─ TrackSet             per-session tracks (manifest), demand (select-tracks)
     │       ├─ Heartbeat            answers ping; liveness timer; negotiation watchdog
     │       └─ SessionContextImpl   the capability-facing handle
     └─ BackendBus                   backend-stream envelopes ↔ capabilities (BackendContext)
```

Ownership is strictly top-down; nothing below holds an owning reference
upward. `Session` objects are owned by the `SessionManager` and referenced
elsewhere only through `(weak_ptr, generation)` pairs.

## Signaling client

`SignalingClient` wraps a libsoup-3 `SoupWebsocketConnection`
([ADR-0017](adr/0017-libsoup-websocket.md)) on the core context:

- **Hello.** `{role:"agent", auth, agent_info:{fjarr, os, arch, capabilities:[names]}, proto_versions:[1]}`.
  Until enrollment lands (M5), `auth` is the dev scheme the Rust
  `DevSharedTokenRegistry` verifies: `{scheme:"dev-token", robot_id, dev_token}`
  (the token from `FJARR_DEV_DEVICE_TOKEN` — the same variable name on the
  server); the M5 scheme `device-signature` (Ed25519 over a server nonce,
  docs/10) slots into the same field. The credential never appears in logs.
- **TURN.** The agent never holds TURN secrets: every `session-request`
  carries that session's ephemeral credentials (`turn`, docs/08), minted
  by the server exactly like the operator's, and the wrapper passes them
  to that session's `webrtcbin` via `add-turn-server`.
- **Backoff** on socket loss: docs/08 numbers (0.5 s ×2, cap 30 s, ±20 %,
  reset after 30 s stable), a pure `Backoff` class shared with tests.
- **Socket loss ends every session.** The server announces `peer-gone` to
  the operators and forgets the sessions (slice 1); the agent therefore
  tears every session down locally with `DetachReason::PeerGone` and
  `release_all_input`. Operators climb their ladder and come back through a
  fresh `session-request`. Session *resume* across a signaling blip is
  [open question #20](18-open-questions.md) — it needs a protocol addition
  and is deliberately not in slice 3.
- **Dispatch.** Every inbound message is validated structurally (the same
  rules as the TS guards; unknown types and unknown fields ignored, a higher
  `v` answered with `error(payload-invalid)` per docs/08) and routed by
  `session_id` to a `Session`, or to the `SessionManager` for
  `session-request`, or to the `BackendBus` for `backend-stream`.
- **Outbound** goes through one queue on the core loop; nothing else calls
  `soup_websocket_connection_send_text`.

## Agent-side session state machine

States: `requested → building → offered → connected → closing → closed`,
with `renegotiating` as a flag on `connected`, not a state — media keeps
flowing through it.

| From | Event | To | Action |
|---|---|---|---|
| — | `session-request` (server-verified grant) | requested | lease check (docs/10 ownership); capability names re-checked against local config (docs/10); reject with `session-reject(reason)` on any failure |
| requested | attach | building | `session_attached` on every granted capability (they declare their per-session tracks); DataChannels created (`fjarr:control`, `fjarr:realtime`, `fjarr:bulk:<cap>` per declaration, `fjarr:stream:<cap>` when a capability declares it); consumer pipeline built; **caps gate** armed |
| building | all enabled tracks have fixed caps, or no media tracks | offered | `session-accept`, then `offer{sdp, tracks(manifest with mid), manifest_version:1}`; negotiation watchdog (15 s) armed with milestones |
| offered | `answer` | offered | set remote description; queued remote ICE applied; trickle continues |
| offered | DTLS connected **and** control DC open | connected | watchdog cleared; heartbeat liveness armed; `bandwidth-stats` sampler (1 s) started; `SessionEvent{started}` to the embedder |
| connected | `<cap>/select-tracks` (docs/08#track-control, served by the core for every track-owning capability) | connected | valves + tier + keyframe request; `result{ok}`; unknown `track_id` → `payload-invalid`, nothing applied |
| connected | capability `update_tracks` / hot-plug | connected (renegotiating) | coalesce into the renegotiation queue: one un-answered offer at a time, `manifest_version++`, unchanged tracks keep `mid` and keep flowing (docs/08#renegotiation) |
| connected | `ice-restart` from the operator | closing → (operator reopens) | on every `webrtcbin` release to date (1.28 included): `session-close{reason:"ice-restart", retry:true}` — the operator opens a new session at once; on a stack with ICE restart: a new offer with fresh ICE credentials, same manifest and `manifest_version`, queued like a renegotiation |
| connected | operator ping | connected | `pong{t0,t1,t2}`; liveness timer reset |
| connected | no ping for 15 s (3 × 5 s) | closing | `session-close(reason="heartbeat")` |
| connected | ICE/DTLS `failed` | closing | `session-close(reason="ice-failed")` — the operator's ladder decides what to do next; the agent never restarts ICE on its own initiative (it always offers, but only when asked) |
| any | `session-close` / `peer-gone` from the server | closing | — |
| any | pipeline error on this session's consumer | closing | `session-close(reason="media-error")`; a media-plane rebuild is decided by the plane, not the session |
| any | watchdog expiry in `building`/`offered` | closing | `session-close(reason="negotiation-timeout:<last milestone>")` — the milestone name is the diagnostic. Only milestones that need the peer count (`offer-created`, `answer-received`, …): the local `local-description-set` / `ice-gathering-complete` neither re-arm the watchdog nor name the timeout, so an unanswered offer reads `negotiation-timeout:offer-created` |
| closing | — | closed | `Capability::release_all_input` on every capability whose manifest says `input_bearing` (unconditionally, before anything else), `session_detached(reason, detail)`, `session-close` sent, valves closed, FrameHub subscriptions dropped; then, after a bounded flush window (150 ms, so what the release emitted on `fjarr:control` leaves before the transport does), consumer pipeline to NULL, `webrtcbin` disposed on the loop, `SessionEvent{ended}`; generation bumped. Inbound envelopes are dropped from `closing` on; `close_all` (shutdown, socket loss, plane rebuild) skips the window |

The order in `closing` is a safety behaviour with a regression test
(docs/15): a session that ends mid-keydown must inject the key-up before
the pipeline is touched, because pipeline teardown can block.

**Milestones** (the watchdog's vocabulary, also snapshot triggers):
`attached`, `channels-created`, `caps-fixed`, `offer-created`,
`local-description-set`, `answer-received`, `remote-description-set`,
`ice-gathering-complete`, `ice-connected`, `dtls-connected`,
`control-open`, `first-frame-sent`. After `connected`, the further
snapshot triggers are **events**, not milestones (the watchdog is off):
`select-tracks`, `renegotiation`, `state-changed`, `producer-restart`,
`plane-rebuild`. Every milestone/event is logged with the session id and
elapsed time since `attached`; `dot_dir` set makes each dump the pipeline
graph ([docs/24](24-pipeline-introspection.md)).

## Offer construction and renegotiation

The `PeerConnection` wrapper owns the only code that talks to `webrtcbin`.
The rules it implements, with the element-level facts the
[webrtcbin spike](../agent/spikes/webrtcbin-probe/README.md) established on
GStreamer 1.24.2 and re-verified on 1.28.2 in slice 2.9 (its README has the
exact API sequences):

- **`bundle-policy=max-bundle`, always.** With the default policy the
  DataChannel transport never connects (spike Q6). One transport carries
  every track and channel.
- **Channels before media, pipeline ≥ READY first.** `create-data-channel`
  asserts in NULL state; the wrapper brings the peer pipeline to READY,
  creates all DataChannels, then requests `sink_%u` pads, so the initial
  offer carries `m=application` and the control channel is open by the
  time DTLS completes (Q1).
- **Caps-gated offer.** A `GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM` probe on
  each track's payloader source pad waits for caps with fixed `payload`
  and `ssrc` (1–3 ms after PLAYING); the offer is created only when every
  track has fixed caps. Disabled tracks are still added — their caps come
  from the encoder configuration, so enabling later is a valve flip, never
  a renegotiation. An `offer_creation_started` latch makes the gate
  idempotent under `on-negotiation-needed` racing the probes (Q2).
- **Manifest `mid` comes from the offer SDP.** The transceiver's `mid`
  property stays NULL until the *answer* is applied (Q2), so the manifest
  builder parses `a=mid:` per m-section from the offer it just created.
  `sink_%u` pads take the next free m-line index (the DC occupies one), so
  the mapping pad → m-section → `track_id` is recorded at pad request time.
- **Renegotiation queue.** `update_tracks(new set)` diffs by `track_id`.
  *Additions* request a new `sink_%u` pad, wait for its caps and re-offer;
  the existing m-sections keep their `mid` and media on them continues
  (spike Q3: 34–36 ms max gap on the untouched track, within the docs/16
  budget). *Removals* (settled in slice 2.9, [ADR-0022](adr/0022-baseline-ubuntu-2604-gstreamer-128.md)):
  close the track's valve **first**, then set its transceiver to
  `inactive` and re-offer, the JSEP way; the track leaves the manifest and
  the transceiver stays in a pool, so the next addition of the same kind
  (re-plugging a monitor) reuses the m-section by flipping it back to
  `sendonly`. The valve-first order matters: an offerer keeps pushing RTP
  on an `inactive` m-section until its valve closes ([re-run](../agent/spikes/webrtcbin-probe/README.md#re-run-on-gstreamer-128)).
  The 1.24 stall was the *answerer* sending EOS down its source pad, and it
  reproduces on a 1.28 answerer with default properties; with
  `reuse-source-pads=TRUE` it is gone (no `RcvbufErrors`, data channel both
  ways, the other track untouched). Every webrtcbin answerer Fjarr ships
  (`fjarr-opsim`, the loop tests) therefore sets that property and needs
  GStreamer ≥ 1.26; browsers never had the mechanic (Chromium check in
  slice 3a). The offerer side needs nothing version-specific, so an
  embedder on 1.24 gets the same removal path.
  The queue holds at most one un-answered offer; changes arriving in flight
  fold into the next offer; `manifest_version` increments per offer *sent*.
- **ICE restart: not on any GStreamer.** `webrtcbin` ignores the offer
  options argument (a `TODO` in the source through 1.28), reuses the
  previous ICE credentials on every re-offer (three `FIXME: deal with ICE
  restarts` sites) and never calls libnice's restart. The agent therefore
  answers the operator's `ice-restart` with
  `session-close{reason:"ice-restart", retry:true}` (docs/08#reconnection):
  the operator opens a new session immediately and the agent builds a fresh
  peer connection. It is a new session: capabilities see `session_detached`
  then `session_attached`, the operator's demand is re-flushed, and the
  docs/10 ownership lease — keyed on the operator identity and 30 s
  fail-open — carries across the gap, so no other operator can take control
  during a restart. The wrapper keeps a `supports_ice_restart()`
  probe so a stack that gains it ([open question #21](18-open-questions.md))
  switches to the in-place re-offer with no protocol change.
- **ICE.** Trickled both ways; `candidate: ""` marks end-of-candidates;
  remote candidates arriving before a remote description is set are queued.
  TURN via `add-turn-server` (`turn(s)://user:pass@host:port[?transport=tcp]`,
  Q6) with credentials from config (dev) or the docs/10 scheme (M5);
  `ice-transport-policy` from config for relay-only tests.
- **Stats.** `get-stats` returns `outbound-rtp` with `ssrc`, `bytes-sent`,
  `packets-sent` (Q5) — keyed back to `track_id` through the ssrc recorded
  at the caps gate; that is the `bandwidth-stats` source.
- **Answerer facts for `fjarr-opsim`** (a webrtcbin answerer): `pad-added`
  fires on the first RTP packet (~200 ms after the answer) with pads named
  `src_<counter>`, not by m-line — map through the pad's `transceiver`
  property (Q3c).

## Media plane

### Video sources: one contract, three ways to provide one

Every track — a test pattern, a USB webcam, a stereo/depth camera with a
vendor SDK, a network camera, a remote-desktop monitor — enters the media
plane through the same **`VideoSource` contract**
([docs/09](09-interfaces.md#the-video-source-contract)): a GStreamer bin
with one or more named output pads producing raw frames (or, when the
device encodes itself, an elementary stream), plus availability and
lifecycle. The core never knows what kind of device is behind a bin.

A customer provides a source in one of three ways, in increasing effort:

| Tier | How | Covers | Code |
|---|---|---|---|
| **1 — declarative** | a GStreamer description string in `fjarr.toml`: `source = "v4l2src device=/dev/video0 ! image/jpeg,width=1280,height=720 ! jpegdec"`; the core wraps it with `gst_parse_bin_from_description` and a ghost `src` pad | anything with a GStreamer plugin: USB/CSI cameras (`v4l2src`, `libcamerasrc`), network cameras (`rtspsrc`), files (`filesrc ! decodebin`), test patterns (`videotestsrc`), vendor plugins the camera maker ships — e.g. Stereolabs ZED (`zedsrc` from the ZED GStreamer plugins, which also exposes left/right/depth), NVIDIA Jetson CSI (`nvarguscamerasrc`), Basler (`pylonsrc`), Allied Vision (`vimbaxsrc`) | none |
| **2 — registered type** | a C++ `VideoSource` implementation registered under a type name (`agent.register_source_type("acme.stereo", factory)`), referenced from config as `source = { type = "acme.stereo", serial = "…" }` with params validated against the type's JSON Schema | SDK-backed devices without a GStreamer plugin (the bin wraps `appsrc` fed by the SDK — Intel RealSense through librealsense, a ZED through its SDK when depth post-processing must run on-device before encoding, industrial GigE cameras via their vendor SDK), multi-output devices (left/right/depth from one device), sources needing custom hot-plug or reconfiguration | one class |
| **3 — capability** | a capability that creates tracks from its own device model (the way `fjarr.desktop` turns monitors from a `DesktopBackend` into tracks) | devices whose *control* surface is the point, not just video | a capability |

**Vendor support is never a core dependency** ([ADR-0020](adr/0020-vendor-sources-as-gstreamer-plugins.md)):
drivers Fjarr distributes are GStreamer plugins in separate, per-architecture
packages (`fjarr-gst-<vendor>`) reached through tier 1; the core links no
SDK, a customer installs only the packages for the hardware they own, and a
configured track whose element is missing is `unavailable` with the reason
(or a startup error if the track is `required = true`) while everything
else runs. Tier 2 is for the embedding application's own code. The same rule covers
the desktop: capture is GStreamer (`ximagesrc`, `pipewiresrc`), while
input, monitors, cursor and clipboard live in in-tree backend modules
loaded at runtime from separate packages, so the core carries no X11,
Wayland or PipeWire dependency ([ADR-0021](adr/0021-desktop-backends-as-runtime-modules.md)).

Tier 1 and tier 2 sources are both consumed by the built-in `fjarr.camera`
capability, whose config is a list of tracks referencing sources
([docs/06](06-capabilities.md#fjarrcamera--camera-video-m1-reference-implementation));
a customer therefore adds a camera Fjarr has never heard of by editing
config, and adds a whole new *kind* of camera by registering one class,
without touching the capability or the core. Built-in source types:
`gst` (tier 1, the default when `source` is a string), `test`
(`videotestsrc` with pattern/size/fps params), `v4l2` (device path or
`by-id` name, format, size, fps — a convenience over tier 1 with hot-plug
via udev), `rtsp` (url, latency, TCP/UDP).

The contract, in prose (the C++ is in docs/09):

- **Outputs.** A bin exposes one ghost pad `src` (single-output) or
  `src_<name>` pads (multi-output, e.g. `src_left`, `src_right`,
  `src_depth`); each output becomes one track. Output caps are raw
  (`video/x-raw`, any format — the core inserts `videoconvert` /
  `vapostproc` as needed), raw in device memory (`memory:DMABuf`,
  `memory:VAMemory`, NVMM later) for zero-copy into the hardware encoder,
  or an elementary stream (`video/x-h264`, `video/x-h265`) for cameras
  with on-board encoders, in which case the core parses and never
  transcodes. A source declares each output's kind (`video` or `audio`;
  audio outputs feed `fjarr.audio`).
- **Lifecycle.** `describe()` (outputs, declared caps, stable identity),
  `create_bin()` on demand, and the bin's normal GStreamer state changes;
  the core creates the bin when the first tier of the first output is
  demanded and disposes it after the idle grace, so an unplugged or idle
  camera holds no device handle.
- **Availability and hot-plug.** `available()` and
  `on_availability_changed(callback)`: a USB camera unplugged mid-session
  makes its tracks disappear and reappear exactly like monitors do —
  through `update_tracks` and the docs/08 renegotiation — with the same
  stable `track_id` (from the config key, never from a device index).
  Sources without native events may poll; `v4l2` uses udev.
- **Errors.** A bin that errors on the bus is restarted with the producer
  backoff; a source may additionally report a permanent failure
  (`unavailable(reason)`) so the core stops retrying and the track shows
  the reason in the manifest label.
- **Threading.** Source code runs on GStreamer streaming threads inside
  its bin, and its callbacks (availability) are marshaled by the core;
  a source never touches sessions, encoders or the FrameHub.

**User-friendliness is a deliverable**, not a hope: `fjarr-agent
--probe-source '<description or type>'` builds the bin standalone,
negotiates caps, prints the resolved format/size/fps and memory type,
runs for two seconds and reports frame rate and any bus error — so a
customer can validate a new camera on the robot without a browser or a
server. The doctor gains a row per configured source.

### Encoders and tiers

The core owns encoding through the `EncoderAdapter` seam (VA-API H.264
first; the doctor's `vah264enc`), so a source never picks an encoder and
hardware differences (Jetson later) stay in one place. The adapter
chooses the upload/convert path from the source's memory type (DMABuf →
`vapostproc` without a copy; system memory → `videoconvert !
vapostproc`); a pre-encoded output bypasses it (`h264parse
config-interval=-1` only).

Tiers (docs/16) are **separate producers of the same source output**: the
output goes through a `tee`; `active` encodes at full resolution/fps,
`thumbnail` at 960×540@5 with a low bitrate cap. A producer starts lazily
on first demand and stops when no session demands its tier for a grace
period (10 s), so an idle robot encodes nothing. `select-tracks` therefore
selects `(track_id, tier)` in the FrameHub, and switching tiers is a
subscription change plus a keyframe request, not a renegotiation.

### FrameHub

One mutex, one entry per `(track_id, tier)`: the latest keyframe, a ring
of the last N encoded samples (N sized to one GOP + 1), and the subscriber
list (`weak_ptr<Subscriber>`). Producers push under the lock; consumers
subscribe with a **keyframe gate**: a new subscriber receives nothing until
the next keyframe (or the retained one), then everything, with PTS rebased
to the consumer's clock so a late joiner starts at zero. A subscriber that
falls behind (ring overrun) is skipped to the next keyframe, never fed
stale frames — the same newest-wins discipline the web client applies to
telemetry.

Keyframe requests (`force-key-unit` events upstream of the encoder) are
issued on: subscribe, `select-tracks` enable, PLI/FIR from the peer
(`webrtcbin` forwards them as upstream events), and tier switch. They are
rate-limited per producer (≥ 1 s apart) so ten operators joining at once
cost one keyframe.

### Consumer pipeline (per session)

```text
[per track]  appsrc(is-live, format=time) ! queue(leaky=downstream, max 1 GOP)
             ! valve ! rtp{h264,vp8,opus}pay(pt, ssrc, config-interval=-1)
             ! webrtcbin.sink_%u
```

`valve drop=TRUE` is the disabled state — the transceiver stays negotiated
and no RTP flows; enabling flips the valve and requests a keyframe. The
`queue` is bounded to one GOP so a stalled network drops frames rather
than growing latency. `bandwidth-stats` (docs/06) is sampled every second
from `get-stats` (`outbound-rtp` bytes/packets per `ssrc` → `track_id`) on
the core loop and sent as an event on control.

### Fan-out: what is shared, what is per consumer {#fan-out}

The whole point of the media plane is that adding an operator costs
almost nothing. The cost model, stage by stage, for one source output
with `V` viewers across `T` demanded tiers:

| Stage | Count | Cost driver | Shared? |
|---|---|---|---|
| Capture / source bin | 1 per output | device, format conversion | yes — one device handle, one capture thread |
| `tee` after the source | 1 | none (buffer refs) | yes |
| Scale + encode | **1 per demanded tier** (`T ≤ 2`) | the dominant cost: VA-API encode, or software if the adapter falls back | yes — the encoder never knows how many viewers exist |
| FrameHub ring | 1 per `(output, tier)` | ~1 GOP of encoded samples (≈ 0.5–2 MB at active tier) | yes |
| Delivery to a subscriber | `V` | one `gst_buffer_ref` + a shallow metadata-only copy for PTS rebase; **no pixel copy, ever** | per consumer, ≈ µs |
| `appsrc ! queue ! valve ! payloader` | `V` | RTP packetization of an already-encoded stream (fraction of a percent per viewer) | per consumer |
| `webrtcbin` (SRTP encryption, RTCP, ICE, DTLS, SCTP) | `V` | AES per packet — the real per-viewer cost, unavoidable since every peer has its own DTLS-SRTP keys | per consumer |
| Keyframe on join / enable | shared, rate-limited (≥ 1 s) | a keyframe is a bitrate spike for *every* viewer of that tier; ten joins in a second cost one | yes |

Guarantees the implementation must keep (each with a loop test):

- **Zero-copy fan-out.** Encoded samples are shared by reference from the
  producer's `appsink` through the ring to every `appsrc`; the only
  per-consumer allocation is the `GstBuffer` metadata copy that carries
  the rebased timestamps (`gst_buffer_copy_region` with
  `GST_BUFFER_COPY_METADATA`, memory shared). The docs/16 "0 allocations
  per frame" budget is checked with heaptrack; a `memcpy` of pixel data
  anywhere after the encoder is a bug.
- **One encoder per demanded tier, none when idle.** A tier's producer
  exists only while at least one session demands it (`select-tracks`),
  with a 10 s grace so an operator toggling a tile does not restart the
  encoder. The hardware encoder count is bounded by `outputs × tiers`,
  which is what docs/16's iGPU budget is written against; the encoder
  adapter reports its instance limit and the core refuses a tier (with a
  logged reason and the track marked `unavailable` in the manifest)
  rather than silently falling back to software.
- **A slow or dead consumer never touches the others.** Each consumer has
  its own bounded `queue(leaky=downstream)` and its own `appsrc` in
  non-blocking mode; the hub's delivery loop never waits on a consumer.
  A consumer that falls a GOP behind is resynced at the next keyframe.
- **Consumer churn never touches capture or encode.** Subscribe and
  unsubscribe are list operations under the hub mutex; the producer
  pipeline's state does not change when sessions come and go.

**Why a hub and not one `tee` in one pipeline.** The camera streamer
tried both: its v2 linked every peer's branch into the single producer
pipeline, so one peer's `webrtcbin` error took the encoders down for
everyone and a stalled peer back-pressured the capture. Its v1 hub with
per-peer pipelines had none of those problems and the same per-viewer
cost, because `tee` also only passes buffer references. The hub adds one
lock and one ring per tier — a price worth paying for the fault isolation,
the keyframe gate for late joiners, and the ability to feed
**non-WebRTC subscribers** from the same ring: the snapshot request
(`fjarr.camera/snapshot`, decoded from the retained keyframe or taken from
the raw `tee` at full resolution), a future local recorder, and the
analytics adapter, none of which need a peer connection.

**What is deliberately not shared.** RTP packetization and SRTP could in
principle be shared by teeing *RTP packets* into every `webrtcbin` with
the same SSRC; the saving is a payloader per viewer (negligible) and it
would cost per-peer PLI/NACK handling, per-peer valves and the ability to
give different tiers to different viewers. Not worth it; revisit only if
profiling shows payloading above 1 % per viewer.

**Audio** fans out the same way: one Opus encoder per source, the ring
holds encoded frames, each session gets its own `appsrc ! rtpopuspay`.

**Measurement.** The loop test streams one 1080p30 source to `N` consumer
branches ending in `fakesink` for `N ∈ {1, 2, 5, 10}` and records CPU per
process, allocations per frame, and per-consumer delivery latency
(hub push → `appsrc` push); `fjarr-opsim` repeats it with real
`webrtcbin` answerers on the compose network. The docs/16 line "≤ 5 % CPU
per additional viewer" is asserted on the opsim numbers (the loop numbers
must be far below it, since they exclude SRTP), and the trend is plotted
per commit so a regression in the hub shows up before a customer's
ten-operator control room does.

### Media-plane recovery

A bus `ERROR` on a consumer pipeline closes that session
(`media-error`). A bus `ERROR` on a **producer** (capture died, encoder
reset) triggers a producer restart with backoff (0.5 s → 5 s, 5 attempts);
subscribers see a gap and a fresh keyframe. If a producer cannot come back
within its budget, or the pipeline fails to reach `PLAYING` after a
rebuild, the media plane escalates: every session closes with
`media-restart`, the plane is disposed and rebuilt from config; the third
plane rebuild within 10 minutes exits with code 2. Each step is a counted
metric and a log line with the element name — the camera streamer's
ladder, with the numbers written down.

## DataChannel router

`ChannelRouter` attaches to the `GstWebRTCDataChannel` objects the session
created and implements docs/08#datachannel-topology:

- **Parsing.** Text messages on control/realtime are parsed as envelopes
  and validated (`v`, `cap` pattern, `kind`, object payload); invalid ones
  are counted and dropped; oversized outbound envelopes (> 16 KiB UTF-8)
  are refused with an error to the sending capability.
- **`fjarr.core`** is served by the router itself: `ping` →
  `pong{ok,t0,t1,t2}` immediately from the core loop (`t1`/`t2` =
  `g_get_real_time() / 1000`, milliseconds), `time-sync` → a `time-sync`
  result with the same payload. Every `ping` or `time-sync` resets the
  session's liveness timer.
- **Track control** (`<cap>/select-tracks`, `bandwidth-stats`,
  docs/08#track-control) is served by the router for every capability
  that declared tracks, so no capability implements it.
- **Dispatch.** `cap` → the capability attached to this session, on the
  core loop, with the session's `SessionContext`. A capability not attached
  to the session (grant did not include it) gets no message; the router
  answers requests to unknown capabilities with
  `result{ok:false, error:{code:"capability-unknown"}}`.
- **`ChannelSender` implementations** per class:
  - control: reliable ordered; `send(Envelope)`; `buffered_amount()` from
    the channel's `buffered-amount` property; `on_drain` from
    `buffered-amount-low` with the threshold at LOW_WATER.
  - realtime: `send(Envelope)` on the unordered/unreliable channel; the
    sender never queues — if `buffered_amount()` exceeds 64 KiB the message
    is dropped and counted (newest-wins is the capability's job; the core
    never lets a lossy channel grow a backlog).
  - bulk (per capability): `send_binary(frame)` with the docs/08 watermarks
    (HIGH 4 MiB / LOW 1 MiB); above HIGH it returns `false` and sends
    nothing, so the capability pumps on `on_drain` — unbounded sends are a
    spec violation, so the sender refuses them. Frames larger than the
    negotiated SCTP `max-message-size` (65 536 on webrtcbin 1.24 and 1.28) are
    rejected with `payload-invalid`; chunking is the capability's job
    (docs/08 file frames are ≤ 256 KiB *and* ≤ the SCTP limit).
  - DataChannel parameters come only from the class table:
    control `ordered=true` reliable; realtime `ordered=false,
    max-retransmits=0`; bulk `ordered=true` reliable; stream
    `ordered=false, max-retransmits=0`; `protocol` empty, ids negotiated by
    SCTP (never `negotiated=true`), created in the order control, realtime,
    bulk…, stream… so the `m=application` index is stable.
- **Channel creation** per session: `control` and `realtime` always;
  `bulk:<cap>` / `stream:<cap>` for each attached capability whose manifest
  declares them. Reliability parameters (`ordered`, `max-retransmits`) are
  set from the class table in docs/08 and nowhere else.

## The concrete `SessionContext` and `BackendContext`

These replace the forward declarations in docs/09 (which is updated in the
same commit as this document):

```cpp
namespace fjarr {

struct TrackSpec {
  std::string track_id;            // stable; camera: the config key; desktop: "desk-<connector>"
  std::string label;
  TrackKind kind;                  // Video | Audio
  SourceRef source;                // a VideoSource (docs/09) + output name; owned by the media plane
  std::optional<MonitorInfo> monitor;
};

class SessionContext {
public:
  const SessionId& id() const;
  const OperatorInfo& operator_info() const;
  const nlohmann::json& granted_params(std::string_view cap) const;

  // Tracks: declared during session_attached (frozen into the first offer)
  // and changeable later (renegotiation, docs/08#renegotiation).
  void add_track(TrackSpec spec);                 // only valid inside session_attached
  void update_tracks(std::vector<TrackSpec> full_set);   // diffed by track_id, coalesced
  TrackState track_state(std::string_view track_id) const;  // enabled, tier, subscribers

  // Channels (docs/08 classes). Bulk/stream senders exist only if declared.
  ChannelSender& control();
  ChannelSender& realtime();
  ChannelSender& bulk();     // this capability's fjarr:bulk:<cap>
  ChannelSender& stream();   // this capability's fjarr:stream:<cap> (ADR-0018; M4)

  // Correlation helpers (docs/08#envelope) — the only way to answer a request.
  void accept(const Envelope& request);
  void feedback(const Envelope& request, nlohmann::json payload);
  void result(const Envelope& request, nlohmann::json payload);   // payload.ok required
  void fail(const Envelope& request, std::string_view code, std::string_view message);
  void event(std::string_view type, nlohmann::json payload);      // kind=event on control

  // Off-loop work: run `job` on the pool, then `done` back on the core loop
  // — dropped if the session is gone by then (generation-guarded).
  void run_async(std::function<void()> job, std::function<void()> done);

  // Safety helpers (docs/15): a deadman the capability arms per input stream.
  std::unique_ptr<DeadmanHandle> arm_deadman(std::chrono::milliseconds budget, std::function<void()> on_expiry);

  void close(std::string_view reason);   // capability-initiated session end
};

class BackendContext {
public:
  ChannelSender& channel();        // backend-stream envelopes over the signaling connection
  bool online() const;             // signaling connected
  // Store-and-forward (M7): durable event types declared by the capability
  // are queued while offline, bounded per docs/16; everything else is
  // dropped when offline. Slice 3 ships the interface with drop-when-offline.
  void declare_durable(std::vector<std::string> event_types);
};

} // namespace fjarr
```

Rules: every method is core-loop-only (asserted in debug builds by thread
id); `add_track` outside `session_attached` throws; `result()` on an
`event` envelope throws; a capability that never answers a request is a
bug the operator sees as a timeout — the core does not synthesize
results.

## Configuration

`AgentConfig::from_file` reads TOML (toml++, MIT — docs/14) with
`FJARR_*` environment overrides winning:

```toml
[agent]
robot_id        = "robot-024"
server_url      = "wss://fjarr.acme.com/ws"
credential_file = "/etc/fjarr/device.key"  # M5 device key; until then the dev token:
dev_token       = ""                       # or FJARR_DEV_DEVICE_TOKEN (env wins); never logged
ice_policy      = "all"                    # all | relay (relay-only for tests)
log_level       = "info"                   # trace|debug|info|warn|error  (FJARR_LOG_LEVEL)
log_format      = "text"                   # text | json                  (FJARR_LOG_FORMAT)
dot_dir         = ""                       # FJARR_DOT_DIR: snapshots as files (docs/24)
watchdog_secs   = 0                        # 0 = WatchdogSec/3 from the unit; the packaged agent refuses to start outside systemd unless `allow_unsupervised = true` (containers, dev)
allow_unsupervised = false

[media]
encoder        = "auto"                    # auto | vaapi | software — never a silent fallback (below)
gop_seconds    = 2                         # keyframe interval; ring and consumer queue are sized to it
active_kbps    = 4000                      # fixed until adaptive bitrate (slice 6)
thumbnail_kbps = 300
tier_grace_ms  = 10000                     # producer lingers this long after the last demand

[introspect]                               # docs/24
enabled      = true
bind         = "127.0.0.1"                 # 0.0.0.0 requires `token`
port         = 7381
socket       = ""                          # Unix socket path instead of TCP
token        = ""
history      = 64                          # snapshots kept per pipeline

[capabilities."fjarr.test"]                # docs/06 — enabled by the install; hooks off in production
enabled    = true
test_hooks = false

[capabilities."fjarr.camera"]              # validated against the capability's JSON Schema
enabled = true

[capabilities."fjarr.camera".tracks.front]
label    = "Front"
source   = "v4l2src device=/dev/v4l/by-id/usb-Acme_Cam-video-index0 ! image/jpeg,width=1280,height=720,framerate=30/1 ! jpegdec"
required = false                           # true: a missing driver is a startup error (ADR-0020)

[capabilities."com.acme.arm-teach"]
enabled = true
privileges = ["fs-read:/var/lib/acme"]     # must match the manifest's requests
```

`FJARR_<SECTION>_<KEY>` environment variables override any key
(`FJARR_AGENT_SERVER_URL`, `FJARR_INTROSPECT_PORT`); the short forms named
in the comments are aliases kept for the docs and the compose files.

**Encoder policy.** `auto` picks VA-API when the doctor's `vah264enc`
smoke passes and otherwise **refuses to start** (exit 1) with the message
that names the two options; `software` selects `openh264enc` (BSD, docs/14)
explicitly — for CI, the devcontainer without `/dev/dri`, and robots that
knowingly trade CPU for portability — and the doctor WARNs while it is in
use. There is no runtime fallback from hardware to software: a robot that
silently starts burning a core is the failure mode this rule exists to
prevent.

**Media-plane constants** (initial values, tuned by the latency harness):

| Constant | Value | Where it applies |
|---|---|---|
| GOP | `gop_seconds` × fps (2 s → 60 frames at 30 fps), one keyframe per GOP, no B-frames | encoder config, ring size (GOP + 1), consumer queue |
| Consumer queue | `queue leaky=downstream max-size-time=<GOP> max-size-buffers=0 max-size-bytes=0` | per track per session |
| `appsrc` | `is-live=true format=time do-timestamp=false block=false max-bytes=0` | per track per session |
| PTS rebase | first delivered keyframe's PTS becomes 0; every later PTS and DTS get the same offset; durations unchanged | FrameHub delivery |
| Payload types | allocated per session from 96 upward in manifest order (video first, then audio); SSRC left to the payloader (random) | offer builder |
| Realtime drop threshold | 64 KiB buffered | realtime `ChannelSender` |
| Keyframe request rate limit | ≥ 1 s per producer | FrameHub |
| Producer restart backoff | 0.5 s → 5 s doubling, 5 attempts | media plane |
| Plane rebuild escalation | third rebuild within 10 min → exit 2 | media plane |

**Timers** (all GLib sources on the core context, all generation-checked):

| Timer | Period | Owner |
|---|---|---|
| Operator liveness | dead after 15 s without `ping`/`time-sync` | Session |
| Negotiation watchdog | 15 s without a milestone, `building`/`offered` only | Session |
| `bandwidth-stats` + `get-stats` sample | 1 s | Session |
| Counters log line | 60 s | Core |
| Tier producer grace | `tier_grace_ms` | MediaPlane |
| `sd_notify` WATCHDOG=1 | `WatchdogSec/3` | Core |
| Signaling backoff | docs/08 | SignalingClient |

The core validates each `[capabilities.X]` table against `X`'s
`config_schema` (nlohmann `json-schema-validator`, MIT) before
`configure()`; an unknown table is an error (exit 1), as is a grant naming
a capability whose table has `enabled = false` (rejected per session,
docs/10).

## Observability

- **Logs**: one line per event on stderr: `<ISO-8601 ms UTC> <LEVEL>
  <component> <message> key=value…` (values quoted when they contain
  spaces); `log_format = "json"` emits the same fields as one JSON object
  per line. Every line inside a session carries `session=<id>`;
  milestone lines carry `ms=<elapsed since attached>`. Credentials, grants
  and TURN passwords are never logged at any level.
- **Pipeline introspection** ([docs/24](24-pipeline-introspection.md)):
  snapshots (DOT, JSON, summary) at every milestone into a per-pipeline
  ring, served live by the local endpoint and the viewer, written to
  `dot_dir` when set, exposed over the session by `fjarr.introspect` —
  the camera streamer's best debugging tool, made a product feature and a
  test oracle.
- **Counters** (exposed later via observability, M7; for now logged every
  60 s): sessions started/ended by reason, negotiation timeouts by
  milestone, dropped envelopes by cause, producer restarts, plane rebuilds,
  keyframe requests.
- **`SessionEvent`** to the embedder: `started`, `ended{reason}`,
  `error{code}`, `audio-uplink`, with operator label — the audit hook of
  docs/10.

## Safety behaviours (docs/15)

- `release_all_input()` on every input-bearing capability is the **first**
  action of `closing`, on every path (operator close, peer-gone, heartbeat,
  media error, watchdog, signaling loss, process stop via SIGTERM).
- A capability that consumes realtime commands arms a deadman
  (`SessionContext::arm_deadman`); the core fires `on_expiry` on the core
  loop when no `feed()` arrived within the budget, and again on session
  end. The teleop test capability in slice 3 uses it so the behaviour has
  a test before any real actuator exists.
- SIGTERM: sessions close with `session-close(reason="agent-shutdown")`,
  input released, then exit 0 — bounded to 3 s, after which the process
  exits anyway.

## Memory and lifetime discipline, and the tooling that enforces it

A C++ wrapper over a C object system fails in three ways — a missed unref
(leak), an extra unref (use-after-free), and the wrong thread (data race) —
and each has a tool that catches it *before* a robot does. Four layers,
every one with a command an engineer or an AI agent runs and a text+JSON
result it reads ([docs/15](15-testing-strategy.md#memory-safety-c)):

1. **Compile time.** Every GObject/GstMiniObject lives in the RAII kit
   (`GstElementPtr`, `GstSamplePtr`, `GstPromisePtr`, `SignalConnection`,
   `PadProbe`, `SourceGuard` for GLib sources…), `[[nodiscard]]` on every
   factory. A `/verify` grep gate refuses raw `g_object_unref`,
   `gst_object_unref`, `g_free`, `g_source_remove`, `g_signal_connect`
   outside `agent/src/core/glib/` — the kit is the only place that may
   touch reference counts. clang-tidy runs `bugprone-*`,
   `cppcoreguidelines-owning-memory`, `misc-unused-*`, `-Werror`.
2. **Sanitizers, three presets, all in CI.** `asan` (Address + Undefined +
   Leak, with `lsan.supp` for GLib's intentional static allocations),
   **`tsan`** (the threading model's proof: FrameHub's lock, `post_to_owner`,
   the worker pool — a data race in the media plane is a TSan failure, not
   a Heisenbug), and `release`. Unit and loop tests run under all three;
   ASan and TSan are gates.
3. **GStreamer's own accounting, in every test and every scenario.**
   The `leaks` tracer (`GST_TRACERS="leaks(filters="GstElement,GstPad,
   GstBuffer,GstSample,GstPromise",stack-traces-flags=full)"`) is
   loaded by the test binaries and by `fjarr-agent --memcheck`; its action
   signals (`activity-start-tracking`, `activity-get-checkpoint`) bracket
   every test case and every `fjarr-opsim` scenario, so "created but never
   freed since the checkpoint" is an **assertion**, per test, with stack
   traces. `GST_DEBUG=GST_REFCOUNTING:7` and `GOBJECT_DEBUG=instance-count`
   (where the platform GLib supports it) are one environment variable
   away; the doctor reports whether they are available.
4. **Runtime census, always on.** The RAII kit counts live wrappers by
   type at negligible cost (`ObjectCensus`: elements, pads, samples,
   promises, sources, sessions, pipelines), the FrameHub reports buffers
   held per ring, the channel senders their buffered bytes; the
   introspection endpoint serves it at `GET /memory` with RSS and a
   `?since=<checkpoint>` diff ([docs/24](24-pipeline-introspection.md)).
   The soak scenario (200 connect/disconnect cycles through `fjarr-opsim`)
   asserts the census returns to baseline and RSS growth stays inside the
   docs/16 budget — the leak test that runs against the real code path,
   not a mock.

Two more tools run nightly rather than per commit: **valgrind memcheck**
on the loop tests (`G_SLICE=always-malloc G_DEBUG=gc-friendly`,
GStreamer's `gst.supp`) for the errors sanitizers structurally miss, and
**heaptrack** on a streaming scenario for the "no allocation on the hot
path" budget (docs/16): steady-state pushes through FrameHub and appsrc
must allocate nothing per frame beyond the buffer refs GStreamer itself
takes.

The agent-first surface is deliberately small: `make agent-test`
(release), `make agent-test-asan`, `make agent-test-tsan`,
`make agent-leaks SCENARIO=<opsim scenario>` (prints the leaks-tracer
diff), `make agent-memcheck` (valgrind), `make agent-heaptrack`, and
`curl localhost:7381/memory` — each printing a one-screen verdict and
writing JSON next to it.

## Testing (docs/15)

**Unit** (GoogleTest, `make agent-test`, run under the ASan preset in CI):
`Backoff`; generation-guarded `post_to_owner` (a torn-down session's
callback is dropped; a live one runs on the loop thread); `FrameHub`
(keyframe gate, PTS rebase, ring overrun skips to the next keyframe, fan-out
to N subscribers, producer restart); envelope parsing/validation against
the golden fixtures in `protocol/fixtures` (all three tiers share them);
`ChannelSender` watermarks with a fake channel; manifest builder; the
renegotiation queue's coalescing (three `update_tracks` while one offer is
in flight → one further offer, `manifest_version` 2 then 3); config loading
and schema validation; the milestone watchdog.

**Loop tests** (GStreamer in-process, no network): a `videotestsrc`
producer through the real encoder path into the FrameHub and into a
consumer branch ending in `fakesink`, asserting caps gating, valve
toggling and keyframe-on-enable — the slice's equivalent of the web mock.

**Integration** (`fjarr-opsim`): a C++ **operator simulator** built on the
spike's second `webrtcbin` plus a libsoup client speaking the operator side
of docs/08. It connects through the real `fjarr-server`, requests the test
capability, answers the offer, checks media arrives (buffer counts per
track), drives `select-tracks`, `ice-restart`, hot-plug via a test hook,
and injects the docs/15 faults: socket drop, silent operator (no pings),
answer never sent (watchdog), SIGSTOP of the agent, TURN-only. It is the
CI substitute for a browser until slice 5's Playwright run, and remains the
fault-injection tool afterwards.

**Budgets**: the loop test records CPU per additional consumer (docs/16
"≤ 5 % per viewer") on the CI runner as a trend, not a gate, until the
latency harness (slice 7) exists.

## `fjarr-opsim`: the operator simulator

A C++ binary in `agent/tools/opsim/` (package `fjarr-tools`) that speaks
the operator side of docs/08 against the real `fjarr-server`, answers with
its own `webrtcbin` (the spike's answerer, so its `pad-added`/`src_N`
quirks are known), and runs named scenarios:

```text
fjarr-opsim --server ws://fjarr-server:8080/ws --robot demo-robot-01 \
            --grant-secret "$FJARR_GRANT_HS256_SECRET" \
            --scenario <name> [--json out.json] [--timeout 60]
```

It mints its own operator grant (HS256 over the dev secret with GLib's
`GHmac`, the same claims the demo-backend uses) so it needs no browser and
no backend. Scenarios, each with the assertions it makes and the state it
expects the agent to end in:

| Scenario | Drives | Asserts |
|---|---|---|
| `smoke` | connect, `select-tracks` on, first frame, `echo`, close | media on both tracks' stamps advance; echo round trip; `session-close` reaches the server |
| `toggle` | enable/disable `test-pattern` 20× | valve state via `/pipelines/session:<id>.json`; keyframe on every enable; no frames while disabled |
| `hotplug` | `hotplug{plugged:true}` then `false` while streaming | re-offer with `manifest_version` 2 then 3; `test-pattern` stamp counter has no gap > 1 frame; `test-second` flows then disappears |
| `silent-operator` | stop sending pings | agent closes with `session-close{reason:"heartbeat"}` within 15–20 s; `release_all_input` observed as `deadman{expired}` |
| `no-answer` | never answer the offer | `session-close{reason:"negotiation-timeout:offer-created"}` at 15 s |
| `socket-drop` | drop the operator's socket mid-stream | agent gets `peer-gone`, closes the session, census returns to baseline |
| `ice-restart` | send `ice-restart` | `session-close{reason:"ice-restart", retry:true}` within 100 ms; new session brokered and streaming |
| `deadman` | `drive` at 20 Hz, then stop | `deadman{state:"expired"}` within 600 ms of the last `drive` |
| `relay-only` | `--ice-policy relay` on both sides | media flows through coturn (relay candidates in both stats) |
| `soak` | N connect/stream/close cycles (default 200) | `/memory` census equal to baseline, RSS growth < 5 MB, no `error` counters |
| `netem-<profile>` | applies a docs/25 profile on the agent container, runs `smoke` | frames keep arriving; health-relevant stats recorded |

Output: a one-screen verdict per assertion (`PASS`/`FAIL name: detail`)
and, with `--json`, the same as data plus the captured signaling and
envelopes; exit 0 on all pass, 1 on any fail, 3 on timeout. `make
agent-leaks SCENARIO=<name>` runs a scenario under the `leaks` tracer and
prints the diff.

## Slices 3a, 3b, 3c and their gates

Slice 3 as first written bundled four reviewable deliverables; it lands as
three increments, each on `main` with its own retrospective review
(docs/20), in this order:

**3a — browser lab and loopback** (web only, no C++ dependency): the
`browser` compose service and `lab` profile, `@fjarr/e2e` with the
`stack`/`dashboard`/`cdp`/`loopback` fixtures (the `stack` fixture
tolerates a missing introspection endpoint until 3b), the wire tap in
`@fjarr/core`, `LoopbackAgent`, the frame-stamp reader, `fjarr-lab`'s
core commands, `make lab-up`/`e2e`, the CI job — plus a half-day spike
running the existing webrtcbin probe's offerer against the lab's Chromium
as answerer for the spike's Q1/Q3/Q6, whose report attaches to ADR-0007.
*Gate:* the slice-2 `<VideoTile>`/`<VideoGrid>`/push-to-talk suites pass
in real Chromium against `LoopbackAgent`; the client ladder's signaling
rungs run under the CDP `offline` profile; the Chromium-answerer spike
report exists. **Met 2026-09-19** (`make e2e`: the `loopback` and `stack`
projects; the ladder over real fjarr-server sockets found four defects the
mock could not — [docs/25](25-browser-lab.md#implementation-notes-slice-3a)).

**3b — agent core, `fjarr.test`, `fjarr-opsim`**: everything in this
document through "Testing", the docs/09 headers, the introspection walker
with `dot_dir` and a minimal endpoint (`/pipelines`,
`/pipelines/<id>.{json,txt,dot}`, `/sources`) with
`introspect.schema.json` under the conformance gate, the ASan gate, TSan
and the `leaks` tracer as trends, and the **minimal demo wiring** the gate
needs: `fjarr-server` configured from the environment, the demo-backend
minting real HS256 grants that list `fjarr.test`, `demo-robot` registering
`fjarr::TestCapability` through the public API.
*Gate:* (1) `docker compose --profile demo up`: the demo dashboard shows
the test pattern within the docs/16 startup budget, `select-tracks`
toggles it, `<ConnectionQuality>` reads real `getStats`; (2) every
`fjarr-opsim` scenario except `soak` and `netem-*` passes in CI; (3) the
lab's ladder scenarios run against the real agent with the agent's fault
switches, the `session-close{retry:true}` rung included, and the frame
stamp proves zero dropped frames on the untouched track during `hotplug`;
(4) unit + loop tests green under ASan; the `release_all_input` and
deadman regression tests exist; the headers match docs/09. **Met
2026-09-20** — `web/e2e/tests/stack/{agent,dashboard}.spec.ts` (first
frame in the dashboard 1.3 s after connect; hot-plug with max stamp gap 1;
deadman 497 ms; `session-close{retry:true}` on `ice-restart`; the
introspection JSON validated against the schema), `make opsim-all`,
`make agent-test-asan`, `agent/tests/test_session.cpp`.

**Implementation notes (slice 3b)** — where the code settled something
the text above left open, or learned from the lab:

- *Caps for the offer come from the transceiver's `codec-preferences`*,
  not from a data-driven pad probe: a disabled track pushes no buffers, so
  the payloader's caps would never fix; the wrapper sets
  `application/x-rtp, media=video, encoding-name=H264, payload=<pt>,
  clock-rate=90000, packetization-mode=1` on each transceiver at pad
  request time and the `caps-fixed` milestone is immediate. The pad probe
  on the payloader still records the negotiated caps for introspection.
- *Tier profiles*: `active` is the source size at ≤ 30 fps, `thumbnail`
  is half the source capped at 960×540 at 5 fps (fjarr.test's 1280×720
  source therefore gives the docs/06 640×360). GOP = `gop_seconds × fps`.
- *Transport errors are not media errors*: a bus `ERROR` from the
  SCTP/DTLS/ICE elements (the peer went away) closes the session with
  `ice-failed`; only errors elsewhere in the consumer pipeline are
  `media-error`. The lab found this when the client's heartbeat tore its
  peer down and the agent blamed its own media plane.
- *The SSRC is signalled from the first offer.* Without `ssrc` in the
  transceiver's codec preferences, webrtcbin signals no `a=ssrc` for a
  track that has not flowed yet and a fresh one once it has; Chromium then
  recreates its receiver on the re-offer and loses a GOP on an *untouched*
  track during hot-plug (found by the lab as a 30-frame stamp gap with
  the browser's inbound counters resetting). The payloader's SSRC therefore
  goes into the preferences with the payload type.
- *PLI/FIR from the peer reach the producer.* webrtcbin turns them into
  upstream force-key-unit events that would die at the consumer's
  `appsrc`; a probe there relays them through the hub to the producer's
  encoder (rate-limited and deferred by the media plane).
- *"Zero dropped frames" is measured at the receiver's decoder*: across a
  hot-plug the browser's `framesDropped` and `packetsLost` for the untouched
  track stay at zero and `framesDecoded` keeps climbing, while the
  frame-stamp counter (read per presented frame) may show a single
  coalesced presentation when Chromium applies the new remote description —
  a compositor skip, not a lost frame. The lab test asserts both.
- *Snapshot coalescing is trailing-edge*: a trigger inside the 250 ms
  window is deferred to the window's end, never dropped, so the served
  snapshot is always the latest state within a quarter second.
- *Input lease*: a session whose operator does not hold the lease is
  accepted read-only — requests to input-bearing capabilities get
  `capability-denied`, events are dropped and counted; the lease refreshes
  on the owner's pings and fails open after 30 s (docs/10).
- *Closing has a flush window.* `release_all_input` runs synchronously and
  what it emits on `fjarr:control` (`deadman{expired}`) must reach the
  operator; a NULL state change in the same loop turn discarded it. The
  session therefore detaches, tells the operator (`session-close`) and
  closes its valves at once, drops every inbound envelope from then on, and
  tears the consumer pipeline down 150 ms later (`close_all` — shutdown,
  socket loss, plane rebuild — skips the window). Track registrations on
  the plane are counted, because a reconnecting operator registers the same
  track id before the old session's deferred close unregisters it.
- *The watchdog names only peer-facing milestones*: `local-description-set`
  and `ice-gathering-complete` neither re-arm it nor name the timeout, so an
  unanswered offer closes as `negotiation-timeout:offer-created`.
- *A late joiner gets the ring, not just the keyframe*: the hub delivers the
  whole current GOP (keyframe first, then its deltas, every one decodable in
  order) to the new subscriber only, deduplicated by a per-hub sequence
  number against frames still queued for fan-out; a ring that overflowed
  past its keyframe is no catch-up and the joiner waits for the requested
  one. The PTS base is set once per subscriber, so a resync keeps the RTP
  timeline monotonic.
- *A second tier starts downstream-first*: the branch is synced sink →
  encoder → queue and only then linked to the tee; the other order let a
  PLAYING queue push into a not-yet-READY encoder bin, take `FLUSHING` and
  park the tee for good with no bus error (a regression test starts the
  thumbnail tier on a running producer).
- *A removed track's branch goes; the transceiver stays.* Parented elements
  cannot be renamed, so a re-added track rebuilds `appsrc ! queue ! valve !
  payloader` under its own name and relinks it to the pooled `sink_%u`.
- *The input lease ends with the owner's last session* (docs/10): nobody
  waits out the 30 s fail-open after a clean close. Promotion of an
  already-open read-only session when the lease frees is not implemented:
  it reconnects.
- *Signals are loop callbacks*: `Agent::stop_on_signal(SIGTERM)` installs a
  `g_unix_signal` source on the core context; shutdown never runs in
  async-signal context and `Supervision::stop_deadline_ms` bounds it. On
  shutdown the WebSocket close handshake is pumped (≤ 300 ms) so operators
  see `session-close{agent-shutdown}` rather than `peer-gone`.
- *TSan sees the kit's hand-offs.* GLib and GStreamer are not built with
  ThreadSanitizer, so a closure handed to the loop, a thread-pool job, a
  promise or a probe would look like a race between its two ends; the kit
  pairs every such hand-off with a release/acquire on one atomic
  (`glib::handoff_release/acquire`), and `tsan.supp` names the
  uninstrumented modules. With that, the 3b tests run clean under TSan and it
  is a gate (docs/15).
- *Nothing inbound may throw through GLib*: the router, the signaling hooks
  and the RAII kit's source trampolines catch `std::exception` (any granted
  operator can send `{"tracks":[1]}`); inbound envelopes above 16 KiB are
  dropped and counted, error messages echoing input are clamped.
- *Deferred to 3c/M4*: the `stream` sender (ADR-0018), the backend bus,
  `/memory`, the `leaks` tracer gate, TSan as a gate, `soak`/`netem-*`, a
  producer bus-error → restart test under ASan, the duplicate
  `deadman{expired}` a capability emits before the core's own expiry.

**3c — introspection completeness and the memory ladder**: `/events`,
the history ring and scrubbing, `/stats`, `/memory` with checkpoints, the
diagnostics bundle, `make introspect` and `fjarr-lab introspect`; the
`leaks` tracer bracketing promoted to a gate, the `soak` and `netem-*`
scenarios, valgrind and heaptrack nightly, TSan promoted to a gate if the
suppressions make it stable. The bundled viewer moves to slice 5 so it is
built once as `<PipelineGraph>` and served from `GET /` as a data file.
*Gate:* docs/24 acceptance minus the viewer, via `curl` and `fjarr-lab`;
the docs/15 memory rows green; the 200-cycle soak returns to the census
baseline.

## Where `fjarr.test` lives

`fjarr::TestCapability` is a public class in `libfjarr` (docs/06), enabled
by `[capabilities."fjarr.test"]`. `fjarr-agent` registers it from config;
`demo-robot` registers it through the public API like any customer
capability, and the compose `demo-robot` service runs the `demo-robot`
binary — the demo stays a customer-shaped embedding.

## What is kept from the camera streamer, and what is changed

The engineering digest of the camera streamer's sender (docs/11) settled
several details of this design. Kept nearly verbatim (with file-level
provenance recorded in the implementation, not here — the source is
gitignored reference material):

- the RAII kit (`GstElementPtr` and friends, `SignalConnection`,
  `ScopeGuard`) — extended with sample/buffer/promise/message wrappers and
  a `SignalConnection` that holds a weak reference to its instance;
- the per-peer pipeline (one `webrtcbin` + branches per session in its own
  `GstPipeline`), so one peer's failure never reaches the producers or the
  other peers — the v1 design, after v2's single shared pipeline showed why;
- the `post_to_owner` + generation-context discipline, the caps-gated offer
  with its latch, the teardown ordering of `close_peer_session`, and the
  milestone-fed negotiation watchdog;
- the FrameHub worker thread pushing into `appsrc` (never from the appsink
  streaming thread), `appsrc` with `block=FALSE` and a bounded queue so a
  slow peer drops instead of stalling the hub;
- DOT dumps at lifecycle points.

Changed deliberately, each a documented flaw in the original:

| Original | Here |
|---|---|
| marshaling onto the *default* `GMainContext`; owner thread recorded late in `run()` | the core owns a private `GMainContext` created in the constructor; the owner thread id is fixed when the loop starts and every `post_to_owner` targets that context |
| timer callbacks bypass the marshaling layer and touch state directly | timers are sources on the core context and still go through the generation check — no undocumented invariants |
| callback contexts hold a raw pointer to the implementation and check the generation *after* dereferencing it | contexts hold `(weak_ptr<Session>, generation)`; the check precedes any access; the core object outlives every source because the loop drains before disposal |
| one reliable-ordered DataChannel for ping, control and stats alike | one channel per docs/08 class, reliability set from the class table only |
| no backpressure: `buffered-amount` never read | `ChannelSender` enforces the docs/08 watermarks; the realtime sender drops instead of queueing |
| keyframe forced on the shared encoder at every join/enable | keyframe requests rate-limited per producer; a retained keyframe serves late joiners |
| ring buffer filled but never read | the ring is the late-join catch-up and the keyframe gate |
| SDP round-tripped through text between the promise and `set-local-description` | the `GstWebRTCSessionDescription` object is passed through; text only at the wire |
| heartbeat timeout equal to one interval (a single lost pong kills a session) | the operator pings; the agent's liveness budget is three intervals |
| static TURN with embedded credentials, config through environment defaults | TOML config, TURN credentials from the docs/10 scheme |
| process restart on any producer error | producer restart with backoff, then plane rebuild, then exit 2 |
| bandwidth counted at `appsrc` push | `get-stats` `outbound-rtp` (bytes actually sent, retransmissions included) |

## Decisions folded into this design

- In-process planes with a documented process-separation seam
  ([ADR-0019](adr/0019-agent-process-model.md)).
- One `VideoSource` contract for every kind of source, provided
  declaratively, as a registered type, or by a capability; the core owns
  encoding (`EncoderAdapter`) and never transcodes a pre-encoded output.
- Tiers are separate producers, started on demand; tier switch is a
  subscription change, never a renegotiation.
- The agent never initiates ICE restart; it answers the operator's request
  (docs/08 keeps "agent always offers" true while the *decision* stays on
  the side that observes the failure).
- Session resume across a signaling blip is not in slice 3
  ([open question #20](18-open-questions.md)).
- `fjarr-opsim` is a first-class test artifact, like the web mock agent.
