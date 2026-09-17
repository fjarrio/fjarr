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
cfg.robot_id = my_robot_id;              // the CUSTOMER'S canonical id
cfg.credential = my_device_credential;   // per-device, from enrollment

fjarr::Agent agent{cfg};
agent.register_capability(std::make_unique<fjarr::CameraCapability>(cams));
agent.register_capability(std::make_unique<acme::ArmTeachCapability>());
agent.on_session_event([](const fjarr::SessionEvent& ev) { /* audit */ });
// SessionEvent { type: "started"|"ended"|"error"|"audio-uplink"; session_id;
//                operator {id,label}; reason (ended); code (error) }
agent.run();   // blocks; or agent.start()/stop() on the host's loop
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

struct CapabilityManifest {
  std::string name;              // reverse-DNS, e.g. "fjarr.camera"
  SemVer version;
  std::vector<TrackDecl> tracks;         // track CAPACITY (docs/05; actual
                                         // per-session set at attach — F1)
  std::vector<ChannelDecl> channels;     // DC classes it needs (docs/08)
  nlohmann::json config_schema;          // JSON Schema for its config
  std::vector<Privilege> privileges;     // explicit grants required
  ConsumerKinds consumers;               // peer, backend, or both
  std::vector<std::string> dependencies; // required capabilities (F4)
  bool input_bearing = false;            // takes the docs/10 ownership lease;
                                         // release_all_input() is called on every detach
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
  virtual void configure(const nlohmann::json& validated_config) = 0;
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
  // Backend consumer hooks (F2) — session-independent conversation with
  // fjarr-server/Cloud over the agent's signaling connection. Default
  // no-ops so peer-only capabilities are unaffected.
  virtual void backend_attached(BackendContext&) {}
  virtual void backend_detached() {}
  virtual void on_backend_message(BackendContext&, const Envelope&) {}
  virtual void shutdown() = 0;
};

// SessionContext (per session) and BackendContext (per agent) are concrete
// classes specified in docs/23 (agent core architecture): tracks
// (add_track at attach, update_tracks for renegotiation — the core
// coalesces into one serialized offer and keeps other tracks flowing,
// docs/08#renegotiation), per-class ChannelSender access, the
// accept/feedback/result/fail correlation helpers, run_async on the worker
// pool, arm_deadman, close. Both are core-loop-only; capabilities never
// touch sockets, SDP or GStreamer negotiation.
// spec: docs/23-agent-core-architecture.md#the-concrete-sessioncontext-and-backendcontext

} // namespace fjarr
```

### The desktop backend interface (Wayland-first shape)

```cpp
namespace fjarr {

class DesktopBackend {
public:
  virtual ~DesktopBackend() = default;
  virtual std::vector<Monitor> monitors() = 0;          // stable connector ids, geometry
  // Hot-plug: fires on connect/disconnect/mode change with the full new set;
  // the capability diffs it and calls SessionContext::update_tracks
  // (docs/08#renegotiation). Backends without native events poll.
  virtual void on_monitors_changed(std::function<void(std::vector<Monitor>)>) = 0;
  virtual CaptureSource start_capture(MonitorId) = 0;   // yields a GstElement/bin
  virtual void stop_capture(MonitorId) = 0;             // never disturbs other captures
  // Input: absolute normalized coordinates within one monitor's region —
  // the Wayland mapping_id model; X11 implements INTO this shape.
  virtual void pointer_motion(MonitorId, double nx, double ny) = 0;
  virtual void pointer_button(MouseButton, bool down) = 0;
  virtual void pointer_wheel(double dx, double dy) = 0;
  virtual void key(LinuxKeycode, bool down) = 0;
  virtual void release_all_input() = 0;   // MUST be called on session end
  virtual ClipboardHandle clipboard() = 0;
};

} // namespace fjarr
```

Implementations: `X11DesktopBackend` (XTest), `WaylandDesktopBackend`
(portals/libei), `UinputInjector` (composable injection override) — chosen by
config after [ADR-0006](adr/0006-desktop-backend-selection.md).

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
  GstCaps* declared_caps;  // what the output will produce (raw, DMABuf/VAMemory, or x-h264…)
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

`fjarr-agent --probe-source '<description | type spec>'` validates a source
standalone (negotiated caps, memory type, measured fps, bus errors) so a
new camera can be brought up on the robot without a server or a browser.
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
user database.

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
    <DesktopView monitorId="HDMI-1" />          {/* M3 */}
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
