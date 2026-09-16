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

Control messages (envelopes on `fjarr:control`, docs/08#envelope):

| `type` | kind | payload | semantics |
|---|---|---|---|
| `select-tracks` | request → result | `{"tracks": [{"track_id": "cam-front", "enabled": true, "tier": "active" \| "thumbnail"}]}` | full desired state for the tracks listed (unlisted = unchanged); agent flips valves, applies docs/16 tier params, requests a keyframe on enable; `result.ok` |
| `bandwidth-stats` | event | `{"interval_ms": 1000, "tracks": [{"track_id", "enabled", "tier", "bitrate_bps", "frames", "dropped"}]}` | per second while any track is enabled |

The client folds all consumers' demand into one `select-tracks`
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
  picks; never one huge stitched frame).
- Pointer: absolute normalized coordinates per monitor, lossy channel;
  buttons/wheel reliable. Keyboard: physical `KeyboardEvent.code` →
  Linux keycodes, reliable channel; layout handling per backend
  ([docs/07](07-desktop-backends.md)).
- Clipboard (text first; images/files later): offer/request MIME model.
- **Unattended access**: works after reboot with nobody logged in — the
  defining industrial requirement; backend chosen accordingly
  ([ADR-0006](adr/0006-desktop-backend-selection.md)).
- Privilege separation for injection ([ADR-0009](adr/0009-privilege-separation.md)).

**Accepted when:** operator controls the robot-sim desktop end-to-end
(input-to-photon within [budgets](16-performance-budgets.md)); reboot of the
sim brings the desktop back with no local interaction; a stuck-modifier can
never persist after disconnect (input state reset on session end).

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
([ADR-0016](adr/0016-swupdate-ota.md)); fleet-daemon's `deploy.py` is the
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
- **Opinionated out-of-the-box**: a reference Ubuntu 24.04 partition layout +
  image-build recipe ships with the docs; deviate only via documented hooks.

**Accepted when:** a demo fleet updates via delta with one device
deliberately failing its post-hook — that device rolls back automatically and
the campaign reports it; power-cut mid-write leaves the device bootable on
the old slot.

## Later / explicitly deferred

Audio (two-way), session recording/replay, mobile operator apps —
[open questions](18-open-questions.md).

## Stress test: `com.example.arm-teach` (never to be built) {#stress-test}

Fictional third-party capability used to pressure-test the
[extension model](05-extension-model.md): a robot-arm teach pendant needing
(a) bidirectional low-latency joint-state streaming (lossy DC, 100 Hz), (b) a
per-robot persistent waypoint file read/written through the file capability's
API as a dependency, (c) a custom dashboard panel with its own state, (d) a
privileged actuator interlock requiring an explicit integrator grant. Every
extension-API change must keep arm-teach buildable **without core patches**.
