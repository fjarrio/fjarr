---
title: Interfaces
description: The three embedding APIs — libfjarr (C++), the backend contract (ADR-0015), and @fjarr web APIs.
---

The three seams a customer integrates against. Signatures here are the
**design source**; skeleton headers/stubs in the repo mirror them and carry
`// spec:` backlinks. Wire behavior: [docs/08](08-protocol.md).

## 1. Robot tier — `libfjarr` (C++)

### Embedding

```cpp
#include <fjarr/fjarr.hpp>

fjarr::AgentConfig cfg = fjarr::AgentConfig::from_file("/etc/fjarr/fjarr.toml");
cfg.apply_env();                          // FJARR_* overrides win (docs/23#configuration)
cfg.agent.robot_id = my_robot_id;         // the CUSTOMER'S canonical id
cfg.agent.dev_token = my_dev_token;       // until M5 enrollment: credential_file

fjarr::Agent agent{cfg};
agent.register_capability(std::make_unique<fjarr::TestCapability>());   // the install smoke test
agent.register_capability(std::make_unique<acme::ArmTeachCapability>());
agent.on_session_event([](const fjarr::SessionEvent& ev) { /* audit */ });
// SessionEvent { type: "started"|"ended"|"error"|"audio-uplink"; session_id;
//                operator_info {id,label}; reason (ended); code (error) }
agent.supervision({ .ready = …, .status = …, .watchdog = …, .watchdog_interval_ms = … });  // sd_notify seam (ADR-0019): READY once configured and running, STATUS= the connection
agent.stop_on_signal(SIGTERM);  // orderly stop as a core-loop callback (never in signal context), bounded by stop_deadline_ms
int rc = agent.run();   // blocks; exit code 0/1/2 per ADR-0019; or agent.start()/stop() on the host's loop
// fjarr::probe_source("v4l2src device=/dev/video0") — what `fjarr-agent --probe-source` prints
```

`fjarr-agent` (the reference daemon) is ~100 lines doing exactly this from
config (`fjarr.toml` + `FJARR_*` overrides — format in
[docs/23](23-agent-core-architecture.md#configuration)). Required idioms inside the library (from the camera-streamer heritage,
[prior art](11-prior-art.md)): RAII wrappers for every GObject, all callbacks
marshaled to one main loop, generation-counted session contexts, caps-gated
offers.

### The capability interface (agent side)

```cpp
namespace fjarr {

using SessionId = std::string;         // the wire session_id (UUIDv7)

enum class ChannelClass { Control, Realtime, Bulk, Stream };   // docs/08 classes
enum class BulkFraming { Raw, Blob };                          // docs/08#blob-frames
struct ChannelDecl { ChannelClass channel; BulkFraming framing = BulkFraming::Raw; };

struct CapabilityManifest {
  std::string name;              // reverse-DNS, e.g. "fjarr.camera"
  SemVer version;
  std::vector<TrackDecl> tracks;         // track CAPACITY (docs/05; actual
                                         // per-session set at attach — F1)
  std::vector<ChannelDecl> channels;     // DC classes it needs + bulk framing (docs/08)
  nlohmann::json config_schema;          // JSON Schema for its config
  std::vector<Privilege> privileges;     // explicit grants required
  ConsumerKinds consumers;               // peer, backend, or both
  std::vector<std::string> dependencies; // required capabilities (F4)
  bool input_bearing = false;            // release_all_input() is called on every detach
  std::string control_domain;            // "desktop" | "motion" | "" (docs/10): one holder
                                         // per domain, claimed on first input; "" = never
                                         // exclusive (terminal, net, files)
  std::vector<std::string> control_inputs; // the message types that are input in that
                                           // domain (claim it, gated by it); empty = every
                                           // message. fjarr.test: {"drive"}, so `echo` never
                                           // claims motion
};

// Sending surface with mandatory backpressure (docs/08#backpressure — F3):
class ChannelSender {
public:
  virtual ~ChannelSender() = default;
  virtual void send(const Envelope& msg) = 0;     // throws FjarrError(payload-invalid) above 16 KiB UTF-8
  // bulk/stream only: false = above HIGH_WATER, not sent — pump on on_drain
  [[nodiscard]] virtual bool send_binary(std::span<const std::byte> frame) = 0;
  virtual std::size_t buffered_amount() const = 0;
  virtual void on_drain(std::function<void()> below_low_watermark) = 0;
};

// DetachReason is the coarse enum; `reason` is the exact docs/08 string
// ("operator-closed", "peer-gone", "heartbeat", "media-restart",
// "ice-restart", "media-error", "negotiation-timeout:<milestone>",
// "agent-shutdown", "ice-failed").
enum class DetachReason { Closed, PeerGone, Heartbeat, Error };

class Capability {
public:
  virtual ~Capability() = default;
  virtual CapabilityManifest manifest() const = 0;
  // `sources` resolves a `source = …` config value (a description string or {type = …})
  // into a VideoSource through the agent's registry — built-in and register_source_type()
  // types alike — so a capability never re-implements the source contract.
  virtual void configure(const nlohmann::json& validated_config, const SourceFactory& sources) = 0;
  // Sessions: attach/detach; ctx provides tracks, channel senders, worker pool.
  virtual void session_attached(SessionContext& ctx, const nlohmann::json& granted_params) = 0;
  virtual void session_detached(const SessionId& id, DetachReason reason, std::string_view detail) = 0;
  // Safety (docs/15): called FIRST on every detach path for input-bearing
  // capabilities, before pipelines are touched. Default no-op.
  virtual void release_all_input(const SessionId& id) {}
  // Envelopes addressed to this capability's namespace (docs/08#envelope).
  // select-tracks / bandwidth-stats never arrive here: the core serves them
  // for every track-owning capability (docs/08#track-control).
  virtual void on_message(SessionContext& ctx, const Envelope& msg) = 0;
  // Incoming bytes on this capability's bulk channel (docs/08#blob-frames).
  // A `raw` channel delivers every binary message as-is; a `blob` channel
  // delivers chunks whose header the core already parsed and checked.
  // Default no-ops: a capability that only sends declares nothing more.
  virtual void on_binary(SessionContext& ctx, std::span<const std::byte> bytes) {}
  virtual void on_blob_chunk(SessionContext& ctx, const BlobChunk& chunk) {}
  // Backend consumer hooks (F2) — session-independent conversation with
  // fjarr-server/Cloud over the agent's signaling connection. Default
  // no-ops so peer-only capabilities are unaffected.
  virtual void backend_attached(BackendContext&) {}
  virtual void backend_detached() {}
  virtual void on_backend_message(BackendContext&, const Envelope&) {}
  // Track-owning capabilities list their configured sources (id, identity, availability + reason)
  // for GET /sources and the doctor, before any session exists (docs/24, docs/26).
  virtual std::vector<ConfiguredSource> configured_sources() const { return {}; }
  virtual void shutdown() = 0;
};

// SessionContext (per session) and BackendContext (per agent) are concrete
// classes specified in docs/23 (agent core architecture): tracks
// (add_track at attach, update_tracks for renegotiation — the core
// coalesces into one serialized offer and keeps other tracks flowing,
// docs/08#renegotiation), per-class ChannelSender access, the
// accept/feedback/result/fail correlation helpers, run_async on the worker
// pool, arm_deadman, close — and `send_blob(bytes, media_type, done)`, which
// returns the BlobRef to put in the envelope you send next: the core chunks,
// pumps under the docs/08 watermarks and calls `done(ok)` on completion (false
// when the session ended first), so no capability writes its own pump;
// `cancel_blob(id)` drops one not yet sent. `BlobAssembler` (a helper, not a
// hook) collects on_blob_chunk() deliveries into whole blobs under a size
// cap for capabilities that want values rather than streams.
// `watch_readable(fd, on_readable)` (added in M2) watches a file descriptor on
// the core loop and returns a handle that stops watching when destroyed; the
// capability owns the fd and closes it. It exists because `fjarr.terminal`'s
// pty is a file descriptor and nothing else in the interface could serve one:
// `run_async` runs a job once and reports back, which is not a read loop, and
// a capability that spawned its own thread would be the one place in the agent
// whose callbacks did not arrive on the core loop. This was the extension
// API's first amendment, and the second capability found it — which is why
// docs/17 schedules a second capability before more are built on the first.
// `every(period, on_tick)` (added in M4.5) is the same shape for time: it calls
// back on the core loop until the handler returns false or the handle is
// destroyed. `fjarr.net` needed it because `link-stats` is specified per second
// *while the link is open*, and an idle link with rising drop counters is
// exactly the case a support engineer must be able to see — driving the event
// off traffic would show nothing then. It is not a scheduler: work that belongs
// off the loop still goes through run_async.
// Both contexts are core-loop-only; capabilities never touch sockets, SDP
// or GStreamer negotiation.
// spec: docs/23-agent-core-architecture.md#the-concrete-sessioncontext-and-backendcontext

} // namespace fjarr
```

### The desktop backend interface (Wayland-first shape)

```cpp
namespace fjarr {

class DesktopBackend {
public:
  virtual ~DesktopBackend() = default;
  virtual Features features() = 0;  // local_cursor, virtual_monitors, desktop_audio, clipboard
  // Monitors carry an identity (EDID vendor, model, serial) and the wire id
  // derived from it (docs/08#track-manifest); the connector name is only
  // informational, because a replug can change it.
  virtual std::vector<Monitor> monitors() = 0;
  // Hot-plug: fires on connect/disconnect/mode change with the full new set;
  // the capability diffs it BY IDENTITY and calls SessionContext::update_tracks
  // (docs/08#renegotiation). Backends without native events poll.
  virtual void on_monitors_changed(std::function<void(std::vector<Monitor>)>) = 0;
  // A capture can end without anyone asking: its monitor went away, the
  // source stopped, the session ended. Mutter ends a stream whose monitor was
  // unplugged by delivering nothing and saying nothing, so the backend must
  // detect it, not just relay a signal. The capability then drops or rebuilds
  // that track.
  virtual void on_capture_lost(std::function<void(MonitorId, CaptureLost)>) = 0;
  // Returns immediately and never waits for a first frame. Capture may be
  // variable-rate: a still screen produces no frames, not even a first one.
  // The backend provokes a first frame where it can, and repeats the last one
  // (pipewiresrc keepalive-time on mutter). The result is the track's source
  // (the video source contract): unavailable until the display server has
  // handed the stream over, then available, so the capability declares the
  // track at once and the core adds it when it is ready. Null for an unknown
  // monitor. (Changed 2026-09-30 from an opaque CaptureSource, M3 3.1.)
  virtual std::shared_ptr<VideoSource> start_capture(MonitorId, CaptureOptions) = 0;  // {cursor_in_video}
  virtual void stop_capture(MonitorId) = 0;             // never disturbs other captures
  // Virtual monitors: headless robots, or a monitor sized to the operator's
  // window. The size must be asked for (unasked, mutter made it 1x1). It lives
  // until destroyed or the session ends, and its id is only unique within the
  // session. Returns an invalid id when unsupported.
  virtual MonitorId create_virtual_monitor(int width, int height) = 0;
  virtual void destroy_virtual_monitor(MonitorId) = 0;
  // Desktop audio: what the robot's speakers play (the default sink's
  // monitor). Returns an invalid source when unsupported.
  virtual std::shared_ptr<VideoSource> start_audio_capture() = 0;
  virtual void stop_audio_capture() = 0;
  // Local-cursor mode (docs/22#cursor-strategy): the shape, on every change,
  // for captures started without the cursor in the video.
  virtual void on_cursor_shape(std::function<void(const CursorShape&)>) = 0;
  // Input: absolute normalized coordinates within one monitor's region —
  // the Wayland mapping_id model; X11 implements INTO this shape. Measured
  // correct on a non-primary monitor and at 200% scale (ADR-0006).
  virtual void pointer_motion(MonitorId, double nx, double ny) = 0;  // only from the desktop domain's holder (docs/10)
  virtual void pointer_button(MouseButton, bool down) = 0;
  virtual void pointer_wheel(double dx, double dy) = 0;
  virtual void key(LinuxKeycode, bool down) = 0;
  // Text through the robot's keymap; returns the characters it cannot type, typing nothing then.
  virtual std::vector<std::string> type_text(const std::string& utf8, std::string& error) = 0;
  virtual void release_all_input() = 0;   // MUST be called on session end
  virtual ClipboardHandle* clipboard() = 0;
};

} // namespace fjarr
```

Implementations, per [ADR-0006](adr/0006-desktop-backend-selection.md):
`MutterDesktopBackend` (GNOME: mutter's ScreenCast and RemoteDesktop),
`PortalDesktopBackend` (the ScreenCast/RemoteDesktop portals with libei), and
`X11DesktopBackend` (`ximagesrc` and XTest). They are chosen by config and by
what the running session offers. The Wayland ones run in the agent but reach the
desktop through `fjarr-desktop-session`, a helper running as the desktop user.
It hands them a PipeWire descriptor and an EIS descriptor
([ADR-0028](adr/0028-desktop-session-helper.md), [the helper protocol](23-agent-core-architecture.md#desktop-helper-protocol)).
Backends are runtime modules ([ADR-0021](adr/0021-desktop-backends-as-runtime-modules.md)): the
core hands a module's factory its configuration, the core loop's `GMainContext`
(every callback runs there) and a log sink. `UinputInjector` stays a designed override
that no chosen backend needs.

### The video source contract {#the-video-source-contract}

Every track enters the media plane through one contract — a test pattern,
a webcam, an SDK-backed stereo camera, a network camera, a desktop monitor
([docs/23](23-agent-core-architecture.md#video-sources-one-contract-three-ways-to-provide-one)).
Customers add sources by config (a GStreamer description string), by
registering a type, or through a capability:

```cpp
namespace fjarr {

struct SourceOutput {
  std::string name;        // "src" for single-output sources; "left"/"right"/"depth" …
  TrackKind kind;          // Video | Audio
  std::string declared_caps;  // caps string of what the output will produce (raw, DMABuf/VAMemory, or x-h264…)
  // Elementary-stream outputs (`video/x-h264`, passthrough — docs/06): the core parses and
  // packetizes, never transcodes, and builds no encoder for the track. A source with a lower
  // second stream names that output "thumbnail" so the track has two tiers; a lone elementary
  // output is one tier without adaptation (docs/23#rate-control-and-tier-switching).
};

struct SourceInfo {
  std::string identity;    // stable device identity (serial, by-id path, url)
  std::vector<SourceOutput> outputs;
};

class VideoSource {
public:
  virtual ~VideoSource() = default;
  virtual SourceInfo describe() const = 0;
  /// A bin exposing ghost pad "src" (or "src_<name>" per output). Created
  /// on first demand, disposed after idle; may be called again later.
  virtual GstBin* create_bin() = 0;
  virtual bool available() const = 0;
  virtual void on_availability_changed(std::function<void(bool)> cb) = 0;   // hot-plug
  virtual void on_unavailable(std::function<void(std::string reason)> cb) {} // permanent failure
  virtual std::string unavailable_reason() const { return ""; }              // why, in words, for /sources and --check
};

// What a capability resolves `source = …` config with (the agent's registry behind it).
class SourceFactory {
public:
  virtual ~SourceFactory() = default;
  /// A description string (tier 1) or {type = "…", …params} (tier 2); throws FjarrError(config).
  virtual std::unique_ptr<VideoSource> create(const nlohmann::json& source_config) const = 0;
  virtual std::vector<std::string> types() const = 0;
};

// Registered source types: config `source = { type = "acme.stereo", … }`
// → factory(params validated against schema). Built-ins: gst, test, v4l2, rtsp.
struct SourceType {
  std::string name;                   // reverse-DNS for third parties
  nlohmann::json params_schema;
  std::function<std::unique_ptr<VideoSource>(const nlohmann::json& params)> create;
};
void Agent::register_source_type(SourceType type);   // before run()/start()

} // namespace fjarr
```

`fjarr-agent --probe-source '<description | {type = "…", …}>'` validates a
source standalone (negotiated caps, memory type, measured fps, bus errors)
so a new camera can be brought up on the robot without a server or a
browser; the type form takes the same inline table as `fjarr.toml`.
Vendor drivers Fjarr distributes are GStreamer plugins in separate packages,
never linked into the core ([ADR-0020](adr/0020-vendor-sources-as-gstreamer-plugins.md));
`register_source_type` is for the embedding application's own sources.

### Adapter seams

`VideoSource` (above), `TelemetrySource` (push typed values + error
events), `EncoderAdapter` (pipeline fragment factory per codec/hardware),
`SignalingTransport` (WebSocket default). ROS 2 lives in a separate `fjarr-ros2` adapter package —
the core never links ROS.

## 2. Backend tier — the integration contract (ADR-0015)

The centerpiece: identical for the self-hosted sidecar and Fjarr Cloud. The
customer's backend implements **two things** and may call **one API**.

### a) Session grants (customer backend → operator client)

A JWT signed with a key registered in `fjarr-server` (per tenant):

```json
{
  "iss": "acme-backend", "aud": "fjarr", "exp": 1789467900,
  "tenant": "acme", "robot_id": "robot-024",
  "operator": {"id": "anna@acme.com", "label": "Anna"},
  "capabilities": [
    {"name": "fjarr.camera"},
    {"name": "fjarr.desktop", "params": {"view_only": false}},
    {"name": "fjarr.files", "params": {"read": true, "write": false}}
  ]
}
```

Short-lived (≤ 5 min to *start* a session; the session may outlive it).
The customer's own auth decides who gets grants — Fjarr never sees their
user database. The demo backend stands in for that auth with a **role**
the dashboard picks before connecting: `operator` is granted the robot's
media capabilities, `developer` additionally `fjarr.introspect`
([docs/24](24-pipeline-introspection.md)) — a demo-only convention, not
part of this contract, kept so the demo never grants everything to
everyone.

A grant names only capabilities the robot has. The robot checks every name
against its own configuration and refuses the whole session with
`capability-denied` if any is missing ([docs/10](10-security.md)). A backend
that serves robots with different capabilities therefore records what each
robot has, and grants the role's capabilities that the robot also has. The
demo backend's registry does this, because its desktop robot has no camera.

### b) Webhooks (fjarr-server → customer backend)

HMAC-signed POSTs, at-least-once, retried with backoff:
`robot.online`, `robot.offline`, `session.started`, `session.ended`
(reason + stats incl. relay bytes), `usage.report` (periodic metering),
later `ota.*`, `observability.*`. Idempotency via `event_id`.

### c) Control-plane REST (customer backend → fjarr-server)

OpenAPI document `protocol/openapi.yaml` (SDKs generated from it):
`PUT /v1/robots/{robot_id}` (register/enroll → returns device credential
bootstrap), `DELETE /v1/robots/{id}`, `GET /v1/robots?status=`,
`DELETE /v1/sessions/{id}` (force-close), `GET /v1/usage`,
`POST /v1/tenants/{t}/keys` (grant-verification keys). Auth: per-tenant API
token.

### d) The operator API (optional — for `fjarr-connect`) {#operator-api}

A browser dashboard carries the customer's logged-in session; a terminal
carries nothing. A customer whose developers use the
[network tunnel](27-network-tunnel.md) therefore exposes two endpoints behind
their own auth, and Fjarr stays out of their authentication as everywhere
else:

| Endpoint | Returns |
|---|---|
| `GET /fjarr/robots` | the robots *this human* may reach: `robot_id`, `label`, `status`, `last_seen` — their fleet table, plus the presence they already receive on the `robot.online`/`robot.offline` webhooks of (b) |
| `POST /fjarr/grants` | a session grant for one `robot_id` — the same JWT as (a), minted by the same code. The body may also carry `capabilities: [name, …]`: the grant then holds only those of them the caller's policy allows, and never more than without it. `fjarr-connect shell` asks for `["fjarr.terminal"]` so its session carries no `fjarr.net` ([docs/27](27-network-tunnel.md#shell)); without the field the backend grants what it always would |
| `POST /fjarr/cli-codes` | for a terminal with no browser: creates a pending login code and returns `{code, poll_token, expires_in}`; the human approves it in the dashboard, and the CLI polls |
| `PUT /fjarr/cli-codes/{code}` | called by the dashboard, inside the signed-in app, with `{credential}` for the user approving it: marks the code approved, once |
| `GET /fjarr/cli-codes/{poll_token}` | the CLI's poll: `{status: "pending"}`, `{status: "approved", credential}` (returned exactly once, then the code is gone), or `{status: "expired"}` |

All three need the caller's identity only on the `PUT`: creating a code and
polling it are unauthenticated by design, since the terminal doing them has no
credential yet. A code is eight characters from an unambiguous alphabet, lives ten
minutes, and the `poll_token` is separate from the `code` so that knowing what the
human typed does not let anyone collect the credential. The dashboard shows the
code back to the human before approving, and a code that was never shown cannot be
approved by guessing: 8 characters from 32 is 2^40, and the backend rate-limits the
`PUT`.

The credential itself is opaque to Fjarr — a bearer token the customer's backend
mints for a signed-in user (`POST /me/fjarr-cli-token` in the demo, but whatever
they already have) and checks on `GET /fjarr/robots` and `POST /fjarr/grants`. Its
lifetime is theirs.

Why here and not on `fjarr-server`: the server knows which robots are
connected, but only the customer's backend knows **who the caller is**, so
only it can scope the list to one human instead of the whole tenant. The
control-plane API of (c) must never be reached from a laptop — its per-tenant
token is a fleet-wide administrative credential.

`fjarr-connect login` obtains its operator credential by handing off to the
customer's dashboard, which mounts the drop-in component from
[docs/21](21-web-client-architecture.md#cli-login); lifetime is the
customer's choice, since it is their identity system.

**Optional by design.** `fjarr-connect` can instead run a configured command
that prints a grant, or take one directly, so no integration is ever blocked
on building this ([docs/27](27-network-tunnel.md#discovery)). `demo-backend`
and `demo-dashboard` implement it as the reference.

### The wire types on their own (`fjarr-protocol`)

The signaling message types are their own crate, depending on `serde` and one
UUIDv7 call and nothing else. `fjarr-signaling` re-exports them at
`fjarr_signaling::protocol`, so nothing that used them had to change.

It exists so that something can speak the wire without linking a server:
`fjarr-connect` ([ADR-0024](adr/0024-native-operator-client.md)) needs these types
and none of axum, hyper, `jsonwebtoken`, `reqwest` or the HMAC stack that the
server tier carries. Sharing the declarations rather than restating them is what
makes wire drift a compile error instead of a test failure — and the same argument
applies to any third party writing a Rust operator.

### The Rust crate beneath (`fjarr-signaling`)

For Rust shops and as the substrate of both editions:

```rust
let app = axum::Router::new()
    .merge(fjarr_signaling::router(fjarr_signaling::Config {
        grant_verifier: my_verifier,        // impl GrantVerifier
        robot_registry: my_registry,        // impl RobotRegistry
        event_sink: my_sink,                // impl EventSink (webhooks/bus)
        turn: TurnConfig::hmac(secret, ttl),
    }));
```

Hook traits: `GrantVerifier`, `RobotRegistry`, `EventSink`, `Meter` — default
implementations = the standalone sidecar behavior.

## 3. Dashboard tier — `@fjarr/core` + `@fjarr/react`

The full design — sessions that follow the user, N sessions per page, the
three subscription modes, the publish side, and demand-driven track
delivery — is [docs/21](21-web-client-architecture.md); the sketches below
are the API summary.

### Core (framework-agnostic)

```ts
const client = createFjarrClient({
  serverUrl: "wss://fjarr.acme.com/ws",
  grant: (robotId) => fetchGrantFromMyBackend(robotId),   // host app owns auth
});
const session = client.sessions.open("robot-024");        // idempotent, N per page
// Reactive state machine — STATE, never refs (teleop-car lesson):
// "idle" | "connecting" | "connected" | "reconnecting" | "failed" | "closed"
session.getState(); session.subscribe(() => …);
session.tracks.list();                                     // from the manifest, labeled
const cam = session.tracks.acquire("cam-front", { tier: "active", visible: true });
session.on("fjarr.telemetry", "joint-state", (env) => …);  // stream mode
await session.request("fjarr.camera", "select-tracks", …); // accept/feedback*/result
session.publisher("com.acme.teleop", "cmd_vel", { maxHz: 50 }).publish(…);
await session.bulk("fjarr.introspect").receive(ref);        // a blob an envelope referred to (docs/08#blob-frames)
session.stats.getSnapshot();                               // per-track, transport
session.close("operator-closed");
```

Reconnect + ICE restart are built in (policy configurable); trickle ICE
always. Transport is injected — the host app decides how grants are fetched.

### React bindings

```tsx
<FjarrProvider client={client}>
  <SessionScope session={client.sessions.open("robot-024")}>
    <VideoTile trackId="cam-front" tier="active" />
    <DesktopView />  {/* M3: the primary monitor; monitorId="<EDID slug>" pins one */}
    <TerminalView />                            {/* M2 */}
    <SessionStatus />   {/* re-renders on state change — guaranteed */}
  </SessionScope>
</FjarrProvider>
```

All components are headless-first (logic hooks: `useSession`,
`useSessionState`, `useTelemetry`, `useVideoTrack`, `usePublisher`) with
styled defaults; third-party capabilities register views via
`registerCapabilityView(name, component)`
([docs/05](05-extension-model.md#web-side-capability-components)).
