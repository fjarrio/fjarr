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
agent.run();   // blocks; or agent.start()/stop() on the host's loop
```

`fjarr-agent` (the reference daemon) is ~100 lines doing exactly this from
config. Required idioms inside the library (from the camera-streamer heritage,
[prior art](11-prior-art.md)): RAII wrappers for every GObject, all callbacks
marshaled to one main loop, generation-counted session contexts, caps-gated
offers.

### The capability interface (agent side)

```cpp
namespace fjarr {

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
};

// Sending surface with mandatory backpressure (docs/08#backpressure — F3):
class ChannelSender {
public:
  virtual ~ChannelSender() = default;
  virtual void send(const Envelope& msg) = 0;                  // ≤ 16 KiB
  virtual void send_binary(std::span<const std::byte> frame) = 0; // bulk only
  virtual std::size_t buffered_amount() const = 0;
  virtual void on_drain(std::function<void()> below_low_watermark) = 0;
};

class Capability {
public:
  virtual ~Capability() = default;
  virtual CapabilityManifest manifest() const = 0;
  virtual void configure(const nlohmann::json& validated_config) = 0;
  // Sessions: attach/detach; ctx provides tracks, channel senders, worker pool.
  virtual void session_attached(SessionContext& ctx, const nlohmann::json& granted_params) = 0;
  virtual void session_detached(SessionId id, DetachReason reason) = 0;
  // Envelopes addressed to this capability's namespace (docs/08#envelope).
  virtual void on_message(SessionContext& ctx, const Envelope& msg) = 0;
  // Backend consumer hooks (F2) — session-independent conversation with
  // fjarr-server/Cloud over the agent's signaling connection. Default
  // no-ops so peer-only capabilities are unaffected.
  virtual void backend_attached(BackendContext&) {}
  virtual void backend_detached() {}
  virtual void on_backend_message(BackendContext&, const Envelope&) {}
  virtual void shutdown() = 0;
};

// SessionContext (per session): session_id(), operator identity,
// ChannelSender& channel(ChannelClass), per-session track activation,
// WorkerPool& worker(). BackendContext (per agent): ChannelSender& with
// store-and-forward semantics for durable event types (docs/11 whitelist
// pattern). Both are populated by the M1 core; capabilities never touch
// sockets or SDP.

} // namespace fjarr
```

### The desktop backend interface (Wayland-first shape)

```cpp
namespace fjarr {

class DesktopBackend {
public:
  virtual ~DesktopBackend() = default;
  virtual std::vector<Monitor> monitors() = 0;          // id, geometry, scale
  virtual CaptureSource start_capture(MonitorId) = 0;   // yields a GstElement/bin
  virtual void stop_capture(MonitorId) = 0;
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

### Adapter seams

`TelemetrySource` (push typed values + error events), `EncoderAdapter`
(pipeline fragment factory per codec/hardware), `SignalingTransport`
(WebSocket default). ROS 2 lives in a separate `fjarr-ros2` adapter package —
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

### Core (framework-agnostic)

```ts
const session = createFjarrSession({
  serverUrl: "wss://fjarr.acme.com/ws",
  grant: () => fetchGrantFromMyBackend(robotId),   // host app owns auth
});
// Reactive state machine — STATE, never refs (teleop-car lesson):
// "idle" | "connecting" | "connected" | "reconnecting" | "failed" | "closed"
session.state; session.subscribe(s => …);
session.tracks;                    // from the manifest, labeled
session.capability("fjarr.camera").send({type: "select-tracks", …});
session.stats;                     // per-track bandwidth, RTT
session.close();
```

Reconnect + ICE restart are built in (policy configurable); trickle ICE
always. Transport is injected — the host app decides how grants are fetched.

### React bindings

```tsx
<FjarrProvider config={…}>
  <RobotSession robotId="robot-024" capabilities={["fjarr.camera"]}>
    <CameraView trackId="cam-front" />
    <DesktopView monitor={0} />
    <TerminalView />
    <SessionStatus />   {/* re-renders on state change — guaranteed */}
  </RobotSession>
</FjarrProvider>
```

All components are headless-first (logic hooks: `useFjarrSession`,
`useTrack`, `useCapability`) with styled defaults; third-party capabilities
register views via `registerCapabilityView(name, component)`
([docs/05](05-extension-model.md#web-side-capability-components)).
