---
title: Capabilities
description: The capability catalog — feature set, priority, and acceptance criteria per capability.
---

Every capability lists: priority (which [milestone](17-roadmap.md)), consumer
kinds, and **acceptance criteria** — the demo-visible behavior that defines
"done". Wire details live in [docs/08](08-protocol.md); APIs in
[docs/09](09-interfaces.md).

## `fjarr.camera` — camera video (M1, reference implementation)

Peer consumer. Ports the proven camera-streamer v3 model ([prior art](11-prior-art.md)).

- Multiple tracks per robot (e.g. front/rear/arm), each announced in the
  track manifest with a stable `track_id` and display label.
- Per-track enable/disable **without renegotiation** (valve + keyframe
  request); disabled tracks cost ~0 bandwidth.
- Active/inactive quality tiers per [budgets](16-performance-budgets.md);
  adaptive bitrate from day one (the camera-streamer gap).
- Per-second `bandwidth-stats` on the control DC, per track.
- **Any source, by config.** Tracks reference sources through the
  [video source contract](09-interfaces.md#the-video-source-contract):
  a GStreamer description string covers every camera with a plugin, a
  registered type covers SDK-backed or multi-output devices, and the track
  id is the config key (stable across replug):

  ```toml
  [capabilities."fjarr.camera".tracks.front]
  label  = "Front"
  source = "v4l2src device=/dev/v4l/by-id/usb-Acme_Cam-video-index0 ! image/jpeg,width=1280,height=720,framerate=30/1 ! jpegdec"

  [capabilities."fjarr.camera".tracks.stereo-left]
  label  = "Stereo left"
  source = "zedsrc camera-resolution=2 camera-fps=30 stream-type=0"    # a vendor plugin (Stereolabs ZED), tier 1

  [capabilities."fjarr.camera".tracks.arm]
  label  = "Arm"
  source = { type = "acme.stereo", serial = "0123" }                    # registered in-process, tier 2
  output = "left"                                                       # which of the source's outputs (default "src")

  [capabilities."fjarr.camera".tracks.rear]
  label    = "Rear"
  source   = { type = "v4l2", device = "usb-Acme_Rear-video-index0", format = "mjpeg", width = 1280, height = 720, fps = 30 }
  required = true                                                       # a missing camera is a startup error (ADR-0020)

  [capabilities."fjarr.camera".tracks.gate]
  label  = "Gate camera"
  source = { type = "rtsp", url = "rtsp://10.0.0.7/stream1", latency = 200, protocols = "tcp" }

  [capabilities."fjarr.camera".tracks.pattern]
  label  = "Test"
  source = { type = "test", pattern = "smpte", width = 1280, height = 720, fps = 30 }
  ```

  Unplugging a camera removes its track through the same renegotiation
  as a monitor hot-plug; replugging restores the same `track_id`.

Control messages: `select-tracks` and `bandwidth-stats` are the generic
[track-control messages](08-protocol.md#track-control) the core serves for
every track-owning capability; `fjarr.camera` adds nothing of its own. The
client folds all consumers' demand into one `select-tracks` per capability
([docs/21](21-web-client-architecture.md#demand-model)); nothing is enabled
until something on screen asks.

**Accepted when:** 3 browsers watch 2 tracks of the demo-robot concurrently;
toggling a track takes effect < 500 ms without renegotiation; kill/restore of
the network recovers the stream without page reload; budgets hold on the NUC.

## `fjarr.desktop` — remote desktop (M3; backend spikes M2)

Peer consumer. The capability that started the project (the original design
discussion is archived in the gitignored `inspiration/` folder; the full
detail is incorporated here).

- One video track per monitor (multi-monitor = multiple tracks, dashboard
  picks; never one huge stitched frame). Track identity derives from the
  monitor's **stable connector id** (docs/08), never from an index.
- **Hot-plug is a first-class requirement, on both sides.** Monitors may be
  connected, disconnected, re-plugged, or change mode at any time during a
  session: the agent's backend reports changes (`on_monitors_changed`), the
  capability updates its track set and the core renegotiates
  ([docs/08](08-protocol.md#renegotiation)) while **every other monitor's
  video keeps flowing**; a `monitors` event precedes the renegotiation so
  the UI reacts instantly. Same-monitor mode/DPI changes update the track in
  place (new geometry, keyframe). Zero monitors (headless robot, everything
  unplugged) is a valid state the session survives; a re-plugged monitor
  returns under its old identity. Web side: [docs/22](22-remote-desktop-client.md#monitors-and-geometry).
- **Presentation mode**: the operator's screens become the robot's screens
  — one fullscreen browser window per robot monitor with Keyboard Lock,
  automated on Chromium via the Window Management API, manual (drag, then
  fullscreen) elsewhere; one session and one ownership lease behind all
  windows ([docs/22](22-remote-desktop-client.md#presentation-mode)).
- Pointer: absolute normalized coordinates per monitor, lossy channel;
  buttons/wheel reliable. Keyboard: physical `KeyboardEvent.code` →
  Linux keycodes, reliable channel; layout handling per backend
  ([docs/07](07-desktop-backends.md)).
- Clipboard (text first; images/files later): offer/request MIME model.
- **Unattended access**: works after reboot with nobody logged in — the
  defining industrial requirement; backend chosen accordingly
  ([ADR-0006](adr/0006-desktop-backend-selection.md)).
- Privilege separation for injection ([ADR-0009](adr/0009-privilege-separation.md)).
- Browser-side design — input pipeline, focus, browser-reserved shortcuts,
  cursor strategy, latency knobs, clipboard UX — is
  [docs/22](22-remote-desktop-client.md); the same `select-tracks` message
  as the camera capability carries `preference: "sharpness"` for text.
- **Desktop audio** (planned, after `fjarr.audio`): the robot's system audio
  output as a `kind: "audio"` track — PipeWire capture on Wayland, PulseAudio
  monitor source on X11 (a per-backend criterion in [docs/07](07-desktop-backends.md)).
  Same demand model and autoplay handling as `fjarr.audio`.

**Accepted when:** operator controls the robot-sim desktop end-to-end
(input-to-photon within [budgets](16-performance-budgets.md)); reboot of the
sim brings the desktop back with no local interaction; a stuck-modifier can
never persist after disconnect (input state reset on session end);
**hot-plug**: with two monitors streaming, adding a third shows it in the UI
within 2 s with zero dropped frames on the other two, unplugging one leaves
a placeholder and the rest untouched, re-plugging restores it under the same
`track_id`, a mode change keeps input coordinates correct, and unplugging
everything then plugging one back recovers without a reconnect;
**presentation mode**: on a two-screen Chromium desktop one click fills
both screens with the two mapped robot monitors, Alt+Tab typed on either
screen reaches the robot, closing the dashboard tab closes both windows,
and on Firefox the same button opens the windows and the operator finishes
manually without errors.

## `fjarr.telemetry` — sensor/telemetry streaming (M4)

Peer + backend consumers.

- Envelope messages on a dedicated DC; **change-triggered with a rate floor**
  (heartbeat every T even when quiet, min-gap when chatty — fleet-daemon pattern).
- Typed values with units; schema declared by the integrator via
  `TelemetrySource` adapters (ROS 2 adapter first).
- Store-and-forward for the backend consumer: whitelist of durable types,
  bounded queue with oldest-first eviction and a drop counter (fleet-daemon lessons,
  including the ones they got wrong).

**Accepted when:** demo-robot streams battery/temperature/pose to the
dashboard live; a 10-minute signaling outage replays durable telemetry on
reconnect, newest-first lane before backfill, without unbounded growth.

## `fjarr.files` — file transfer (M4, greenfield)

Peer + backend consumers. No prior art in the inspiration projects (fleet-daemon had
literally zero `bytes` fields) — designed fresh in [docs/08](08-protocol.md#file-frames).

- Manifest → chunked binary frames (64–256 KiB, ≤ `sctp.maxMessageSize`) on
  a dedicated reliable DC; SHA-256 whole-file integrity.
- **Backpressure** both directions (`bufferedAmount` low-water pumping —
  never queue-unbounded).
- **Resume** by received-ranges after reconnect; a 4 GB diagnostic dump over
  LTE must survive connection churn.
- Bulk traffic isolated from interactive channels (own DC; separate
  PeerConnection if measurements demand — [budgets](16-performance-budgets.md)).
- Explicit direction grants: `files:read`, `files:write`, per-path
  allow-lists in agent config.

**Accepted when:** 1 GB transfers both ways with a mid-transfer network kill
resumed to a verified hash; interactive video latency unaffected during bulk.

## `fjarr.terminal` — remote terminal (M2)

Peer consumer. Deliberately the **second** capability implemented: media-free,
so it proves the extension API generalizes beyond video.

- PTY on the robot (login shell of a configured user), reliable ordered DC,
  xterm.js component in `@fjarr/react`.
- Resize, UTF-8, scrollback handled client-side; session recording hook
  (audit) from day one — terminal access is the scariest capability
  ([docs/10](10-security.md)).

**Accepted when:** interactive shell round-trip < 150 ms LAN; disconnect
kills the PTY (no orphan shells); every session start/end is audit-logged.

## `fjarr.observability` — fleet observability (M7, flagship paid tier)

Backend consumer.

- Release/version reporting: agent, OS, customer software versions (via
  adapter), reported on change + on connect.
- Generic host metrics out of the box: CPU temp, load, mean per-process CPU,
  disk usage/health, memory, network counters.
- **User-defined metrics and error events** through the same adapter seam:
  counters, gauges, and error reports with severity + fingerprint (for
  fleet-wide error-rate views).
- Transport: telemetry envelope, backend stream, store-and-forward; server
  aggregates (Cloud: retention, dashboards, alerting).

**Accepted when:** a 10-robot simulated fleet renders version spread, error
rate, and host-metric trends in the fleet view; one robot going dark raises
its liveness state within 30 s.

## `fjarr.ota` — fleet OTA updates (M8)

Backend consumer. Built on **SWUpdate**
([ADR-0016](adr/0016-swupdate-ota.md)); the fleet daemon's `deploy.py` is the
documented anti-pattern this replaces (no A/B, no rollback, abort-unsafe).

- **A/B partitioning, atomic apply, automatic rollback** on boot-failure
  (grub/u-boot env + health-check confirmation window).
- Artifacts: SWUpdate `.swu`; **streamed binary diffs** (delta updates) to
  spare LTE data plans; served via the file capability's backend mode with
  resume.
- Pre/post install hooks (customer scripts, e.g. "drive to charger first" —
  the fleet-daemon requirement done safely: refusable, timeout-bounded, logged).
- Campaigns (Cloud/paid): staged rollout by fleet segment, failure-rate
  auto-halt, per-device accept→feedback→result reporting.
- **Opinionated out-of-the-box**: a reference Ubuntu 26.04 partition layout +
  image-build recipe ships with the docs; deviate only via documented hooks.

**Accepted when:** a demo fleet updates via delta with one device
deliberately failing its post-hook — that device rolls back automatically and
the campaign reports it; power-cut mid-write leaves the device bootable on
the old slot.

## `fjarr.audio` — two-way audio (planned) {#fjarraudio--two-way-audio-planned}

Peer consumer. Opus over native WebRTC audio tracks — no new dependencies
on either side (GStreamer `opusenc`/`opusdec`, browser `getUserMedia`).

- **Downlink**: one or more `kind: "audio"` tracks in the manifest (robot
  microphones — hear motors, alarms, people). Demand-driven like video:
  default off, per-consumer mute, browser autoplay policy surfaced (docs/21).
- **Uplink** (talk to a person at the robot): granted per session with
  `params.talk = true`; the agent pre-allocates a `recvonly` transceiver in
  its offer so the browser attaches its mic via `replaceTrack()` without
  renegotiation. Played through a configured output device on the robot with
  a **hard volume cap** in agent config.
- **Push-to-talk by default**; open mic is explicit. Uplink start/stop are
  audited session events (docs/10) — a live microphone is as sensitive as a
  terminal.
- Not in scope: alert sounds/beeps in the dashboard (host UI concern),
  desktop-audio capture for `fjarr.desktop` (later, same track model).

**Accepted when:** an operator hears the robot-sim's synthetic audio source
within budget (docs/16); PTT delivers speech to the sim's sink with
echo cancellation on; releasing PTT stops the uplink within 200 ms; audit
events recorded. Milestone: **M3** ([roadmap](17-roadmap.md#m3--see-control-and-hear-the-robot)).

## `fjarr.logs` — live log tailing (planned)

Peer + backend consumers. `journalctl`/application log streams over a
reliable-ordered channel with server-side filtering (unit, level, regex),
backpressured so a chatty robot can't flood the operator; the same source
feeds observability (M7) in the backend mode. Cheap, and the single most
requested support feature after "show me the screen". Milestone: peer mode
**M4**, backend mode **M7** ([roadmap](17-roadmap.md)).

## `fjarr.test` — the built-in test capability (slice 3) {#fjarrtest--the-built-in-test-capability-slice-3}

Peer consumer, built into `libfjarr` as the public class
`fjarr::TestCapability` and enabled by config. It exists for two reasons:
it proves the core end to end before any real capability exists
([docs/23](23-agent-core-architecture.md#slices-3a-3b-3c-and-their-gates)), and it is the
customer's **install smoke test** — after `curl … | sh`, a test pattern in
the dashboard proves the robot, the server and the grant flow work before
a single camera is configured ([docs/26](26-robot-install-and-drivers.md)).
It ships in the `fjarr-agent` package for that reason; its hooks are inert
unless enabled.

Tracks (all `codec: "H264"`, active tier 1280×720@30, thumbnail 640×360@5,
frame stamp drawn per [docs/25](25-browser-lab.md#frame-stamp-the-latency-harnesss-oracle)):

| `track_id` | label | source | present |
|---|---|---|---|
| `test-pattern` | "Test pattern" | `videotestsrc pattern=smpte is-live=true` | always |
| `test-second` | "Second pattern" | `videotestsrc pattern=ball is-live=true` | only while the hot-plug hook has it "plugged" (default: unplugged) |

Messages (envelopes, `cap: "fjarr.test"`):

| `type` | kind | channel | payload | semantics |
|---|---|---|---|---|
| `echo` | request → result | control | request `{any}` → result `{ok:true, echo:<the request payload>, t_agent: ms}` | correlation round trip |
| `hotplug` | request → result | control | `{"plugged": bool}` | adds/removes `test-second` through `update_tracks` → docs/08 renegotiation; `result{ok:true, manifest_version}`; requires `test_hooks = true` |
| `silence` | request → result | control | `{"pings": bool, "media": bool, "ms": number}` | for `ms`: stop answering `ping` and/or close all valves — the operator-visible "agent went silent" faults; requires `test_hooks = true` |
| `drive` | event | realtime | `{"v": number, "seq": number}` | the deadman-armed consumer: the capability arms `SessionContext::arm_deadman(500 ms)` on the first `drive`; each `drive` feeds it |
| `deadman` | event (agent → operator) | control | `{"state": "armed" \| "expired" \| "fed", "ms_since_feed"}` | emitted on every transition, so a test can assert that `drive` silence of 500 ms expires the deadman and that session end expires it too |

`fjarr.test` is **input-bearing** for the docs/10 ownership lease (one
driver at a time) and implements `release_all_input` by expiring the
deadman and emitting `deadman{state:"expired"}` — which is how the docs/15
"release on session end" regression test observes the behaviour without a
real actuator.

Config: `[capabilities."fjarr.test"] enabled = true`, `test_hooks = false`
(the demo and CI set it true; a production install leaves the hooks off,
and the grant must list `fjarr.test` for an operator to reach it at all).

**Accepted when:** the demo dashboard shows the pattern within the docs/16
startup budget; `select-tracks` toggles it; `hotplug` adds the second
track with zero dropped frames on the first (frame-stamp counter);
`silence{pings}` makes the operator's ladder climb and recover; `drive`
silence expires the deadman in ≤ 600 ms and so does `session-close`.

## `fjarr.introspect` — pipeline introspection (slice 3 core, slice 5 UI) {#fjarrintrospect--pipeline-introspection-slice-3-core-slice-5-ui}

Peer consumer, built in. Exposes the agent's live pipeline snapshots
(DOT, JSON, summary text, with history) over the session so a robot's
media plane can be inspected from the dashboard without shell access —
the product feature specified in [docs/24](24-pipeline-introspection.md).
Grant-gated (`{"name":"fjarr.introspect"}`, developer/support roles);
large bodies ride `fjarr:bulk:fjarr.introspect`.

**Accepted when:** from the demo dashboard's Diagnostics tab, an operator
with the introspect grant watches the session pipeline update live while
toggling tracks and triggering the hot-plug test hook; the same snapshots
are readable with one `curl` on the robot; an operator without the grant
gets `capability-denied`.

## Sensor data transport: video track or stream class? {#sensor-transport}

Dense sensor data (depth, thermal, disparity, lidar point clouds) can travel
either **packed into a video track** or as **stream-class binary frames**
([ADR-0018](adr/0018-stream-channel-class.md)). Neither is universally
better; the choice is made **per sensor, per capability** with this matrix,
confirmed by measurement in the first sensor capability's spike
([open question #16](18-open-questions.md)).

| Concern | Video track | Stream class |
|---|---|---|
| Bandwidth | Temporal + spatial compression: 1080p30 dense depth in a few Mbps | Raw size unless we compress ourselves (640×480×16-bit ≈ 147 Mbps at 30 fps — must decimate/voxelize or add Draco/zstd) |
| Fidelity | Lossy: 8-bit 4:2:0 codecs; 16-bit metric data needs a packing profile (split planes, or 10/12-bit HEVC/AV1 where the browser decodes them in hardware); edge ringing → "flying pixels" | Bit-exact: 16-bit depth, float32 XYZ, intensities, per-point attributes |
| Data shape | Dense 2-D grids only | Anything: sparse/variable point counts, structs, protobuf/CBOR |
| Metadata & sync | Out-of-band (frame seq via time-sync + telemetry) — a real sync problem | Inline: timestamps, intrinsics, frame id in the header |
| Rate control | Free: GCC/TWCC adaptive bitrate, FEC/NACK, jitter buffer, demand tiers | Ours: drop frames on `bufferedAmount` growth; no FEC; whole frame or nothing |
| CPU / HW | HW encode (VA-API) and HW decode in the browser | Agent: cheap unless compressing; browser: WASM decode if compressed |
| Consumer access | `<video>` → WebGL texture is cheap; **numeric readback is expensive** (canvas `getImageData` / WebCodecs `VideoFrame` copy) | `ArrayBuffer` straight into a three.js/WebGL buffer; trivial numeric access |
| Latency | Encoder pipeline adds ~1–2 frames | Serialization only |
| Fits when | dense, high-rate (≥ 15 fps), visualization-grade, resolution matters | sparse or exact, ≤ ~10 Hz after decimation, ≤ docs/16 stream budget, metadata-rich |

Rules of thumb: a depth camera meant for *seeing* obstacles → video
(with a documented packing profile); a lidar cloud meant for a 3-D map
or for *numbers* → stream class after decimation; and **hybrid** is
legitimate — a depth video for the overview plus a decimated cloud on the
stream class for the 3-D view, both stamped with `time-sync`. The
measurements that decide each case: end-to-end latency, bandwidth, agent
and browser CPU, and fidelity error (RMS depth error after the round trip).

## Conventions on existing capabilities (planned additions)

- `fjarr.camera/snapshot` — request → result carrying one full-resolution
  still (JPEG/PNG) via the bulk channel, for tickets and reports without
  screen-grabbing a compressed video frame.
- `fjarr.telemetry/alert` — a conventional event shape
  `{severity, code, message, data?}` so hosts route robot-originated
  notifications to their own toast/notification system uniformly (the
  fleet dashboard's `/rtc/toastMessage`, generalized).
- `fjarr.files/list` — directory listing within the configured allow-lists,
  a prerequisite for any file-browser UI.
- `fjarr.core` — reserved namespace for session-level messages that belong
  to no capability: `ping`/`pong` (docs/08 heartbeat) and `time-sync`
  (operator↔agent clock offset + RTT, needed for stamping teleop commands
  and for the docs/15 latency harness).
- **Multi-operator presence** (planned, M5): the server emits
  `session-peers` (docs/08) to every party on a robot — who is connected,
  with their ownership role from the docs/10 leases (`owner` of input,
  `viewer`) — so UIs can show "Anna is driving, Björn is watching" and
  request/hand over control explicitly. Web: `useSessionPeers(session)`.
- **Haptic feedback** (planned): any capability may emit
  `haptic {intensity, duration_ms, pattern?}` events on the realtime class
  (collision proximity, end-stop, terrain); the web library maps them to
  the Gamepad API's `vibrationActuator` via `useGamepadHaptics(session, cap)`
  and ignores them where unsupported. Lossy by design — a missed rumble is
  fine, a late one is worse.

## Later / explicitly deferred

Session recording/replay, mobile operator apps —
[open questions](18-open-questions.md).

## Stress test: `com.example.arm-teach` (never to be built) {#stress-test}

Fictional third-party capability used to pressure-test the
[extension model](05-extension-model.md): a robot-arm teach pendant needing
(a) bidirectional low-latency joint-state streaming (lossy DC, 100 Hz), (b) a
per-robot persistent waypoint file read/written through the file capability's
API as a dependency, (c) a custom dashboard panel with its own state, (d) a
privileged actuator interlock requiring an explicit integrator grant. Every
extension-API change must keep arm-teach buildable **without core patches**.
