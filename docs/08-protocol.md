---
title: Protocol
description: Normative wire specification — signaling messages, DataChannel topology, envelopes, blob frames.
---

> **Normative.** This document defines the wire behavior implemented three
> times (C++, Rust, TypeScript). Divergence is a bug in the code, never a
> reinterpretation of the spec. Implementations link back here with
> `// spec: docs/08-protocol.md#<anchor>` comments.

## Design rules

1. **Typed envelope, opaque media payloads.** Routing/metadata fields are
   schema'd; SDP and ICE bodies ride as opaque strings (never modeled — they
   change with browser versions). *(fleet-daemon lesson)*
2. **Correlate, don't RPC.** Every request carries a unique `event_id`;
   responses echo it. Long operations reply `accept` → `feedback`\* →
   `result`, all with the same `event_id`.
3. **Explicit reliability.** Every DataChannel declares a reliability class;
   nothing defaults silently to reliable-ordered. *(teleop-car lesson)*
4. **Versioned from message one.** See [Versioning](#versioning).

## Transport layers

| Layer | Carries |
|---|---|
| WSS to `fjarr-server` | signaling envelopes (session setup, ICE), backend-consumer streams |
| WebRTC media tracks | camera/desktop video (RTP) |
| WebRTC DataChannels | everything else, per the topology below |

### RTP feedback and repair {#rtp-feedback}

Every video m-line the agent offers carries, and the agent honours:

| Feedback | Offered as | The agent's side |
|---|---|---|
| Congestion feedback | `a=rtcp-fb:* transport-cc` + the transport-wide sequence header extension | the per-peer estimator's input ([docs/23](23-agent-core-architecture.md#rate-control-and-tier-switching)); browsers send it by default |
| Retransmission | `a=rtcp-fb:* nack` + an `rtx` payload per video payload | a lost packet is re-sent on request from a per-peer buffer of one GOP; a receiver waits at most one round trip instead of asking for a keyframe |
| Keyframe requests | `a=rtcp-fb:* nack pli`, `ccm fir` | a keyframe from the tier's encoder, rate-limited (≥ 1 s, shared by every viewer of the tier); a no-op with a counted log line on a passthrough track (the camera decides) |
| Forward error correction | **not offered** | it spends bandwidth on every link to help the lossy ones; revisit with a customer on a link where retransmission's round trip is too long |

A track's `bandwidth-stats` shows the effect (below): the estimate the
agent holds for this peer and the tier it is actually sending.

## Signaling messages {#signaling}

JSON text frames on the WSS connection. Common fields on **every** message:

```json
{
  "v": 1,
  "type": "…",
  "event_id": "uuidv7",
  "ts": 1789467600123
}
```

| `type` | Direction | Additional fields | Semantics |
|---|---|---|---|
| `hello` | client→server | `role: "agent"\|"operator"`, `auth`, `agent_info`/`client_info`, `proto_versions` | authenticate + advertise. `auth` for operators: `{scheme:"grant", jwt}`. For agents: `{scheme:"dev-token", robot_id, dev_token}` (the dev registry, until M5) or `{scheme:"device-signature", robot_id, nonce_sig}` (enrollment, M5). `agent_info`: `{fjarr, os, arch, capabilities:[names]}` |
| `hello-ack` | server→client | `session_id?`, `proto_version`, `turn` (urls + ephemeral credential + ttl) | accept; operator gets TURN creds here |
| `session-request` | server→agent | `session_id`, `capabilities: [{name, params}]`, `operator` (display identity), `turn?` | grant-verified request to open a session; `turn` carries the agent's ephemeral TURN credentials for this session (same minting and TTL as the operator's `hello-ack.turn`, docs/10#turn — agents never hold static TURN secrets) |
| `session-accept` / `session-reject` | agent→server→operator | `session_id`, reject: `reason` | agent's answer, relayed to the operator (policy hooks may refuse) |
| `offer` | agent→server→operator | `session_id`, `sdp`, `tracks` ([manifest](#track-manifest)) | **agent always offers** |
| `answer` | operator→server→agent | `session_id`, `sdp` | |
| `ice` | both, trickled | `session_id`, `candidate`, `sdp_mline_index` | trickle ICE is REQUIRED; `candidate: ""` = end of candidates |
| `ice-restart` | operator→server→agent | `session_id` | the operator asks the always-offering agent to re-offer with an ICE restart on the existing session ([reconnection](#reconnection)); the agent answers with a new `offer` |
| `session-close` | any | `session_id`, `reason`, `retry?` (bool, default false) | orderly teardown; `retry: true` means the closer expects the operator to open a **new session immediately** (agent media-plane restart, the ICE-restart fallback below) — the client treats it as a reconnection rung, never as a terminal close |
| `peer-gone` | server→other side | `session_id`, `reason` | server-side last-will: socket death is announced, never inferred *(camera-streamer last-will lesson)* |
| `backend-stream` | agent↔server | `capability`, `payload` (envelope) | backend-consumer envelope transport |
| `session-peers` *(planned, M5)* | server→all parties | `session_id`, `peers: [{operator, role: "owner" \| "viewer"}]` | multi-operator presence from the docs/10 ownership leases; emitted on every change |
| `error` | server→client | `code`, `message`, `caused_by` (the offending message's `event_id`) | see [error codes](#errors) |

### Reconnection {#reconnection}

Both sides reconnect with backoff (base 0.5 s ×2, cap 30 s, ±20 % jitter,
reset only after 30 s stable *(fleet-daemon backoff, copied verbatim)*).
The operator's ladder, cheapest rung first:

1. ICE `disconnected`: wait a short grace (3 s) — ICE usually recovers on
   its own.
2. Still disconnected, or ICE `failed`: send `ice-restart` over signaling
   (the WSS is independent of the media path). An agent whose stack
   supports ICE restart re-offers with new ICE credentials and the same
   manifest; tracks and consumers stay attached. An agent that cannot
   (`webrtcbin` ignores the restart option through 1.28 — the
   [spike](../agent/spikes/webrtcbin-probe/README.md)) MUST answer
   `session-close{reason:"ice-restart", retry:true}` at once, so the
   operator climbs to rung 3 without waiting out the re-offer timeout
   ([open question #21](18-open-questions.md)).
3. No new offer within 10 s, or the signaling socket itself is gone: a
   **new signaling round** with the same grant (a fresh `hello`, new
   session) — re-fetching the grant first if the server said
   `grant-expired`. Consumers keep their subscriptions and demand; the
   client re-flushes demand on the new session.
4. Attempts exhausted (host-configurable, default 5 rounds): `failed`, with
   an explicit retry available.

## DataChannel topology {#datachannel-topology}

Channels are created by the agent at session setup, named
`fjarr:<class>[:<capability>]`:

| Channel | Class | Ordered | Retransmit | Used for |
|---|---|---|---|---|
| `fjarr:control` | control | yes | reliable | envelopes: capability control, telemetry, clipboard metadata, heartbeat |
| `fjarr:realtime` | realtime | **no** | **0** | pointer motion, joint states — newest-wins data only |
| `fjarr:bulk:<cap>` | bulk | yes | reliable | one per bulk-using capability; either [blob frames](#blob-frames) (file chunks, clipboard payloads, snapshot bodies) or a raw byte stream — the capability declares which |
| `fjarr:stream:<cap>` | stream | **no** | **0** | lossy binary in either direction: `framed` framing for chunked frames with frame-level newest-wins (point clouds, depth maps, custom sensor data — [ADR-0018](adr/0018-stream-channel-class.md)), `raw` framing for one self-contained datagram per message ([network packets](#net-packets)) |

Rules:

- `KEY_DOWN`/`KEY_UP`, button events → **control** (loss = stuck key;
  reliability required). Pointer *motion* → **realtime**.
- Bulk channels implement [backpressure](#backpressure); control/realtime
  messages MUST stay ≤ 16 KiB.
- Stream frames under `framed` framing carry a 12-byte header (`u32
  frame_seq`, `u16 chunk_index`, `u16 chunk_count`, `u32 payload_len`) so
  receivers reassemble whole frames and drop incomplete or stale ones; a
  frame larger than `sctp.maxMessageSize` is chunked, never queued behind a
  newer frame. Under `raw` framing there is no header and no reassembly:
  one binary message is one complete datagram, dropped or delivered whole.
- Session-level messages that belong to no capability use the reserved
  `cap` `fjarr.core` ([below](#fjarr-core)).
- Heartbeat: `ping`/`pong` envelope on control every 5 s, 3 missed → the
  peer is considered dead: the operator tears the media path down and climbs
  the [reconnection ladder](#reconnection) (its signaling socket death, or a
  `session-close(reason="heartbeat")` if the socket is still up, tells the
  server); the agent ends the session with `session-close(reason="heartbeat")`.
- A bulk **or stream** channel's **framing is declared** in the capability
  manifest, never guessed by a peer. Bulk: `raw` (terminal input — every
  binary message is bytes for the capability, no header) or `blob`
  ([blob frames](#blob-frames):
  files, clipboard payloads, introspection snapshots — chunks of a named,
  sized blob that an envelope refers to). Stream: `framed` (the 12-byte
  header above, the default) or `raw`. Control and realtime carry envelopes
  only.

### Session-level messages (`fjarr.core`) {#fjarr-core}

| `type` | kind | payload | notes |
|---|---|---|---|
| `ping` | request (operator → agent) | `{"t0": ms}` | every 5 s while connected; `t0` = sender's clock at send |
| `pong` | result (echoes the `ping`'s `event_id`) | `{"ok": true, "t0", "t1", "t2"}` | `t1` = agent receive time, `t2` = agent send time, agent clock |
| `time-sync` | request / result (result `type` is `time-sync`; only `ping` is answered as `pong`) | same payloads as `ping`/`pong` | an explicit on-demand probe (latency harness); heartbeats already keep the estimate fresh |
| `ice-restart` | — | — | not an envelope: it is a [signaling message](#signaling), because it must work while the media path is down |

`t0`..`t3` are unix milliseconds (an agent using `g_get_real_time` divides
by 1000). Clock offset and RTT follow NTP: with `t3` = the operator's receive time,
`offset = ((t1 − t0) + (t2 − t3)) / 2`, `rtt = (t3 − t0) − (t2 − t1)`.
Implementations keep the sample with the smallest RTT over a sliding
window (the fleet-daemon lesson: a single skewed sample must not jump the
clock); consumers stamp outgoing commands as `local + offset`.

### Track control (every track-owning capability) {#track-control}

`select-tracks` and `bandwidth-stats` are **served by the agent core for
every capability that declares tracks**, under that capability's `cap`
(`fjarr.camera/select-tracks`, `fjarr.desktop/select-tracks`,
`fjarr.test/select-tracks`); a capability never implements them. The
client sends one request per track-owning capability
([docs/21](21-web-client-architecture.md#demand-model)).

| `type` | kind | payload | semantics |
|---|---|---|---|
| `select-tracks` | request → result | `{"tracks": [{"track_id", "enabled": bool, "tier": "active" \| "thumbnail", "preference"?: "motion" \| "sharpness"}]}` | full desired state for the tracks listed (unlisted = unchanged); the core flips valves, applies docs/16 tier params, requests a keyframe on enable, maps `preference` to the encoder's degradation preference; `result{ok:true}` once applied. A `track_id` that is not in this capability's manifest makes the whole request fail — `result{ok:false, error:{code:"payload-invalid", message:"unknown track <id>"}}` — and nothing is applied |
| `bandwidth-stats` | event (agent → operator) | `{"interval_ms": 1000, "tracks": [{"track_id", "enabled", "tier", "effective_tier", "estimate_bps", "adaptive", "bitrate_bps", "frames", "dropped", "nacks", "keyframe_requests"}]}` | once per second while any of the capability's tracks is enabled. `tier` = what the client asked for, `effective_tier` = what the agent sends (lower while this peer's link is below the tier's band — [docs/23](23-agent-core-architecture.md#rate-control-and-tier-switching)); `estimate_bps` = the agent's estimate of this peer's available bandwidth for the track; `adaptive` = false on a passthrough track without a lower stream (nothing the agent can do about congestion); `nacks` and `keyframe_requests` = repair requests served in the interval. `bitrate_bps` = `outbound-rtp` bytes sent over the interval × 8; `frames` = encoded frames pushed to this peer in the interval; `dropped` = frames skipped for this peer in the interval (FrameHub ring overrun + leaky-queue drops). Informational for the UI; the client's health score uses its own `getStats` (docs/21) |

Codec strings in the manifest are RTP encoding names, uppercase: `H264`,
`H265`, `VP8`, `VP9`, `OPUS`. `mid` values are opaque strings; a
GStreamer agent produces `video0`, `application1`, `video2`… (the spike).

## The envelope {#envelope}

All control/backend messages share one JSON shape:

```json
{
  "v": 1,
  "cap": "fjarr.camera",
  "type": "select-tracks",
  "event_id": "uuidv7",
  "kind": "request",
  "payload": { }
}
```

`kind` ∈ `request` | `accept` | `feedback` | `result` | `event` (unsolicited).
`accept/feedback/result` echo the request's `event_id`. `result.payload`
always carries `ok: bool` and, on failure, `error: {code, message}`. A
request to a capability not attached to the session is answered
`result{ok:false, error:{code:"capability-denied"}}` when the agent has
that capability but the grant did not include it, and
`capability-unknown` when the agent has never heard of it (so a client can
tell "ask for a wider grant" from "wrong robot"); an envelope with a
`v` other than 1 on a DataChannel is dropped and counted, never answered
(the signaling `hello` already negotiated the major).

Capability payload schemas live in `protocol/schemas/` (JSON Schema),
versioned with the capability; TS types and C++/Rust validators are
generated from them (single source — *fleet-daemon schema-drift lesson*).

**Blob references.** A payload field may be a *blob reference* instead of
the value itself: `{"blob": "<blob_id>", "len": <bytes>, "type":
"<media type>"}` (the `blob-ref` fragment of `envelope.schema.json`). The
bytes travel as [blob frames](#blob-frames) on the capability's bulk
channel. A payload whose serialized form would exceed 8 KiB MUST use a
reference — the 16 KiB control ceiling leaves headroom for the envelope
around it, and the control channel never waits behind a large body.

## Track manifest {#track-manifest}

Sent inside `offer.tracks`, before any media flows:

```json
[
  {"track_id": "cam-front", "cap": "fjarr.camera", "kind": "video",
   "label": "Front", "codec": "H264", "pt": 96, "mid": "0", "monitor": null},
  {"track_id": "desk-HDMI-1", "cap": "fjarr.desktop", "kind": "video",
   "label": "HDMI-1 (Dell U2720Q)", "codec": "H264", "pt": 97, "mid": "1",
   "monitor": {"id": "HDMI-1", "index": 0, "primary": true,
               "x": 0, "y": 0, "w": 1920, "h": 1080, "scale": 1.0,
               "name": "Dell U2720Q"}}
]
```

`monitor.id` is the **stable identity** (the connector name, e.g. `HDMI-1`,
`eDP-1`; virtual outputs use the backend's stable name) and desktop
`track_id`s derive from it (`desk-<id>`), so a monitor keeps its identity
across unplug/replug and across sessions. `index`, `primary` and the `x`/`y`
placement are informational and change freely; `name` is the EDID model when
known. Never key anything on `index`.

`kind` is `"video"` or `"audio"` (audio tracks: docs/06 `fjarr.audio`).
`track_id` is stable across renegotiations. `mid` is the SDP media
identifier of the transceiver carrying the track (the agent knows it at
offer time); receivers map incoming `RTCTrackEvent.transceiver.mid` →
`track_id` directly — no SDP parsing, no payload-type guessing
([docs/21](21-web-client-architecture.md#track-registry)). Dashboards MUST
label from the manifest, not from SDP order. *(camera-streamer manifest lesson)*

### Renegotiation and manifest updates {#renegotiation}

The track set can change mid-session (monitor hot-plug, a capability adding
a track). The agent — which always offers — sends a **new `offer`** on the
established session:

- `tracks` is the **complete** new manifest; unchanged tracks keep their
  `track_id` and `mid`; removed tracks are absent from the manifest — the
  agent MAY keep their transceiver alive but muted (`sendonly`, valve
  closed) for reuse, so receivers key on the manifest, never on transceiver
  liveness; new tracks get new transceivers. `mid` values are read from the
  offer SDP (`a=mid`) by both sides.
- `manifest_version` (monotonic `u32`, new offer field) orders manifests
  **within a session**: a new `session_id` starts a new sequence, and
  receivers reset their applied version with it. Receivers ignore an offer
  *older* than one already applied and re-apply an *equal* one (an
  ICE-restart re-offer carries the unchanged manifest and the unchanged
  version; the SDP is what changed).
- The agent serializes renegotiations: at most one un-answered offer per
  session; further changes are coalesced into the next offer.
- **Media on unchanged tracks MUST continue uninterrupted** throughout the
  renegotiation (docs/16 budget) — a hot-plug on one monitor never blips
  another.
- ICE restart travels in the offer's SDP as usual; the same serialization
  applies.

## Input events (fjarr.desktop)

Pointer motion (realtime channel, coalesced client-side to ≤ 60 Hz):

```json
{"cap":"fjarr.desktop","type":"pointer","kind":"event",
 "payload":{"track_id":"desk-0","x":0.7341,"y":0.4288,"seq":58342}}
```

`x`/`y` are normalized [0,1] within that monitor's track — DPI/scaling
agnostic by construction. `seq` is monotonic; receivers drop stale.
Control-channel input messages (all `cap: "fjarr.desktop"`):

| `type` | payload | notes |
|---|---|---|
| `button` | `{"button": "left" \| "middle" \| "right" \| "back" \| "forward", "down": bool}` | |
| `wheel` | `{"dx": px, "dy": px}` | already normalized to pixels by the client |
| `key` | `{"code": "KeyA", "down": bool}` | `code` = `KeyboardEvent.code` (physical key), mapped to Linux keycodes agent-side; clients MUST NOT forward browser auto-repeat — the held key repeats natively |
| `key-combo` | `{"codes": ["ControlLeft", "AltLeft", "Delete"]}` | atomic press-and-release for combos the browser cannot capture |
| `text` | `{"text": "åäö"}` | composed/IME/pasted text the physical-key path cannot express; agent injects as Unicode typing |
| `release-all` | `{}` | client-initiated on focus loss; the agent MUST also release everything on `session-close` |
| `cursor` *(agent → client, realtime class)* | `{"shape_id", "hotspot": {x, y}, "png"?: base64}` | cursor shape changes for local-cursor rendering ([docs/22](22-remote-desktop-client.md#cursor-strategy)); `png` only when a new `shape_id` appears |
| `monitors` *(agent → client, event)* | `{"monitors": [monitor…], "reason": "hotplug" \| "mode-change" \| "initial"}` | emitted immediately on any change with the full current set, *before* the renegotiation completes, so UIs can show placeholders/arrangements at once; the offer's manifest remains the source of truth for tracks |

Full client-side semantics: [docs/22](22-remote-desktop-client.md#input-pipeline).

## Blob frames {#blob-frames}

A bulk channel declared with `blob` framing carries binary messages of one
shape, whatever the capability — the same chunker, reassembler and
backpressure pump serve introspection snapshots (slice 5), clipboard
payloads (M3) and file transfer (M4):

```text
offset  size  field
0       1     version (0x01)
1       16    blob_id (UUIDv7, binary)
17      8     byte_offset (u64 BE)
25      8     blob_len (u64 BE — the whole blob)
33      4     payload_len (u32 BE)
37      …     payload (≤ 256 KiB and ≤ sctp maxMessageSize)
```

Rules:

- A blob is **complete** when every byte of `[0, blob_len)` has arrived.
  Chunks of one blob are sent in offset order (the channel is ordered);
  chunks of different blobs may interleave.
- A chunk whose version is not 1, whose `payload_len` disagrees with the
  message length, or whose `byte_offset + payload_len` exceeds `blob_len`
  is dropped and counted; the blob it belongs to is discarded.
- **Order between channels.** The sender emits the referencing envelope
  first, then the chunks. Control and bulk are different DataChannels, so
  receivers MUST accept either arrival order: chunks of a blob no envelope
  has named yet wait in a bounded pending store (8 MiB or 30 s per
  channel, oldest evicted); a reference whose blob has not completed 30 s
  after the envelope arrived fails locally (the caller of the request or
  the subscriber sees an error; nothing goes back on the wire). An
  unreferenced blob that completes is kept under the same bound until an
  envelope claims it.
- **Backpressure** ([below](#backpressure)) applies to blob senders like
  any bulk sender. A capability that produces blobs faster than the
  channel drains decides what to drop; the core never queues unboundedly
  (`fjarr.introspect`: the newest snapshot per pipeline wins,
  [docs/24](24-pipeline-introspection.md#from-the-dashboard-the-fjarrintrospect-capability)).
- A blob that must **survive reconnect** (a file) is resumed by ranges,
  below; a blob that is a *value* (a snapshot) is simply sent again by the
  new session. Receivers reassemble small blobs in memory (16 MiB cap by
  default) and consume large ones chunk by chunk.

### File transfer (fjarr.files) {#file-frames}

Manifest/resume/complete are envelopes on control; the data is one blob
per transfer with `blob_id = transfer_id`: `file-offer` (name, size,
sha256, direction — the offer *is* the blob reference) → `accept` →
chunks → `file-complete(result)`. Resume: receiver sends
`file-resume {transfer_id, ranges:[[start,end],…]}` after reconnect; sender
fills gaps only. Integrity: whole-file SHA-256 verified before `result.ok`.

### Terminal (fjarr.terminal) {#terminal}

A PTY on the robot. Bytes ride `fjarr:bulk:fjarr.terminal` with `raw`
framing in **both** directions — every binary message is exactly the bytes
for the pty, keystrokes one way and output the other, with no header of
ours. Reliable and ordered, because a terminal that loses a character is
worse than one that is slow.

**One PTY per session at M2.** That is why nothing here carries a terminal
id: a second concurrent shell would need the channel multiplexed, which is a
framing change and therefore a protocol decision to take when something
actually needs it, not now.

| `type` | kind | payload | semantics |
|---|---|---|---|
| `open` | request → result | `{"cols": u16, "rows": u16, "term"?: string}` | start the pty. `result{ok:true}` once the shell is running. `error{code:"unavailable", message}` when the robot has no terminal configured ([docs/06](06-capabilities.md)) — a deployment choice, not a fault; `error{code:"busy"}` when a pty is already open on this session. A grant without `fjarr.terminal` never reaches the capability at all: the core answers `capability-denied` first ([errors](#errors)), and a client must treat that as "you were not given a shell" rather than as a failure |
| `resize` | request → result | `{"cols": u16, "rows": u16}` | `TIOCSWINSZ` + `SIGWINCH`. Sent on every client resize, coalesced by the client |
| `close` | request → result | `{}` | `SIGHUP`, then reap. Idempotent |
| `exit` | event (agent → operator) | `{"code"?: int, "signal"?: string}` | the shell ended on its own; exactly one of `code` or `signal` |

The pty dies with the session, always: `release_all_input` on session end
closes it ([docs/15](15-testing-strategy.md#safety-behaviors)), so a dropped
connection cannot leave an orphan shell holding the robot's resources. The
capability is **input-bearing**, so it takes the ownership lease like any
other ([docs/10](10-security.md#session-ownership)).

### Network packets (fjarr.net) {#net-packets}

The [network tunnel](27-network-tunnel.md) uses `fjarr:stream:fjarr.net`
with `raw` framing: **one binary message is exactly one IP packet**, with no
header of ours at all. The channel is unordered with no retransmission
because the traffic inside carries its own recovery, and an outer
retransmission would fight TCP's and lose on a poor link.

- A packet larger than the negotiated MTU is **dropped, never fragmented**;
  the interface's MTU makes this unreachable in practice and the counter
  exists to catch misconfiguration.
- Senders bound their queue and **drop the tail** when the channel's
  buffered amount is high. Queueing is forbidden: a queue delivers a burst
  of stale packets after congestion, which damages the round-trip estimate
  of every connection inside the tunnel more than the loss would have.
- Both ends drop any packet whose destination is not their own tunnel
  address, or whose source is not the expected peer
  ([docs/27](27-network-tunnel.md#isolation)). These checks are normative:
  they are what makes one robot unreachable from another.

Control messages for the capability travel on `fjarr:control` as ordinary
envelopes:

| `type` | kind | payload | semantics |
|---|---|---|---|
| `open` | request → result | `{}` | brings the link up. `result{ok:true, address, peer_address, mtu, policy:{forwarding:false, allow_ports}}` — `address` is the robot's tunnel address, `peer_address` the operator's. Refused with `error{code:"forbidden"}` when the session grant carries no `net` claim, and `error{code:"unavailable"}` when the interface is absent or the agent is not attached to it |
| `close` | request → result | `{}` | stops the flow; the interface itself stays, since it is persistent by design |
| `link-stats` | event (agent → operator) | `{"interval_ms": 1000, "tx_packets", "rx_packets", "tx_bytes", "rx_bytes", "dropped_no_peer", "dropped_policy", "dropped_queue", "dropped_mtu"}` | once per second while the link is open; the four drop counters are what a support engineer reads first |

### Backpressure {#backpressure}

Senders pump only while `bufferedAmount < HIGH_WATER` (4 MiB) and resume on
`bufferedamountlow` (LOW_WATER 1 MiB); GStreamer side mirrors with
`buffered-amount`/`on-buffered-amount-low`. Unbounded sends are a spec
violation, not a style issue.

## Errors {#errors}

`error.code` is a stable string: `auth-failed`, `grant-expired`,
`capability-unknown`, `capability-denied`, `session-unknown`, `robot-offline`, `rate-limited`,
`payload-invalid`, `internal`. Codes are append-only.

## Versioning {#versioning}

- `v` on every signaling message / envelope = **protocol major**. A peer
  receiving a higher major replies `error(code="payload-invalid")` and the
  connection renegotiates to the common `proto_versions` from `hello`.
- Within a major: fields are add-only; unknown fields MUST be ignored;
  semantics of existing fields never change (add a new type instead).
- Capability payloads version independently via capability semver
  ([docs/05](05-extension-model.md#compatibility-rules)).
- The schemas in `protocol/schemas/` are the machine-readable source; this
  doc explains semantics. CI (M0.5) fails if code and schemas drift.
