---
title: Protocol
description: Normative wire specification — signaling messages, DataChannel topology, envelopes, file frames.
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
| `hello` | client→server | `role: "agent"\|"operator"`, `auth` (device credential or session grant), `agent_info`/`client_info`, `proto_versions` | authenticate + advertise |
| `hello-ack` | server→client | `session_id?`, `proto_version`, `turn` (urls + ephemeral credential + ttl) | accept; operator gets TURN creds here |
| `session-request` | server→agent | `session_id`, `capabilities: [{name, params}]`, `operator` (display identity) | grant-verified request to open a session |
| `session-accept` / `session-reject` | agent→server | `session_id`, reject: `reason` | agent's answer (policy hooks may refuse) |
| `offer` | agent→server→operator | `session_id`, `sdp`, `tracks` ([manifest](#track-manifest)) | **agent always offers** |
| `answer` | operator→server→agent | `session_id`, `sdp` | |
| `ice` | both, trickled | `session_id`, `candidate`, `sdp_mline_index` | trickle ICE is REQUIRED |
| `session-close` | any | `session_id`, `reason` | orderly teardown |
| `peer-gone` | server→other side | `session_id`, `reason` | server-side last-will: socket death is announced, never inferred *(camera-streamer last-will lesson)* |
| `backend-stream` | agent↔server | `capability`, `payload` (envelope) | backend-consumer envelope transport |
| `error` | server→client | `code`, `message`, `caused_by` (the offending message's `event_id`) | see [error codes](#errors) |

Reconnection: both sides reconnect with backoff (base 0.5 s ×2, cap 30 s,
±20 % jitter, reset only after 30 s stable *(fleet-daemon backoff, copied
verbatim)*). An operator reconnect attempts ICE restart on the existing
session before requesting a new one.

## DataChannel topology {#datachannel-topology}

Channels are created by the agent at session setup, named
`fjarr:<class>[:<capability>]`:

| Channel | Class | Ordered | Retransmit | Used for |
|---|---|---|---|---|
| `fjarr:control` | control | yes | reliable | envelopes: capability control, telemetry, clipboard metadata, heartbeat |
| `fjarr:realtime` | realtime | **no** | **0** | pointer motion, joint states — newest-wins data only |
| `fjarr:bulk:<cap>` | bulk | yes | reliable | file frames, clipboard payloads; one per bulk-using capability |

Rules:

- `KEY_DOWN`/`KEY_UP`, button events → **control** (loss = stuck key;
  reliability required). Pointer *motion* → **realtime**.
- Bulk channels implement [backpressure](#backpressure); control/realtime
  messages MUST stay ≤ 16 KiB.
- Heartbeat: `ping`/`pong` envelope on control every 5 s, 3 missed → session
  considered dead → teardown + `session-close(reason="heartbeat")`.

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
always carries `ok: bool` and, on failure, `error: {code, message}`.

Capability payload schemas live in `protocol/schemas/` (JSON Schema),
versioned with the capability; TS types and C++/Rust validators are
generated from them (single source — *fleet-daemon schema-drift lesson*).

## Track manifest {#track-manifest}

Sent inside `offer.tracks`, before any media flows:

```json
[
  {"track_id": "cam-front", "cap": "fjarr.camera", "kind": "video",
   "label": "Front", "codec": "H264", "pt": 96, "monitor": null},
  {"track_id": "desk-0", "cap": "fjarr.desktop", "kind": "video",
   "label": "Monitor 1", "codec": "H264", "pt": 97,
   "monitor": {"index": 0, "w": 1920, "h": 1080, "scale": 1.0}}
]
```

`track_id` is stable across renegotiations. Dashboards MUST label from the
manifest, not from SDP order. *(camera-streamer manifest lesson)*

## Input events (fjarr.desktop)

Pointer motion (realtime channel, coalesced client-side to ≤ 60 Hz):

```json
{"cap":"fjarr.desktop","type":"pointer","kind":"event",
 "payload":{"track_id":"desk-0","x":0.7341,"y":0.4288,"seq":58342}}
```

`x`/`y` are normalized [0,1] within that monitor's track — DPI/scaling
agnostic by construction. `seq` is monotonic; receivers drop stale.
Buttons/wheel/keys (control channel): `{"type":"button","payload":
{"button":"left","down":true}}`, `{"type":"key","payload":{"code":"KeyA",
"down":true}}` — `code` is `KeyboardEvent.code` (layout-independent physical
key), mapped to Linux keycodes agent-side. On `session-close` for any reason
the agent MUST release all held keys/buttons.

## File frames (fjarr.files) {#file-frames}

Manifest/resume/complete are envelopes on control; data rides binary frames
on `fjarr:bulk:fjarr.files`:

```text
offset  size  field
0       1     version (0x01)
1       15    transfer_id (uuid, truncated binary)
16      8     byte_offset (u64 BE)
24      4     payload_len (u32 BE)
28      …     payload (≤ 256 KiB and ≤ sctp maxMessageSize)
```

Envelope flow: `file-offer` (name, size, sha256, direction) → `accept` →
frames → `file-complete(result)`. Resume: receiver sends
`file-resume {transfer_id, ranges:[[start,end],…]}` after reconnect; sender
fills gaps only. Integrity: whole-file SHA-256 verified before `result.ok`.

### Backpressure {#backpressure}

Senders pump only while `bufferedAmount < HIGH_WATER` (4 MiB) and resume on
`bufferedamountlow` (LOW_WATER 1 MiB); GStreamer side mirrors with
`buffered-amount`/`on-buffered-amount-low`. Unbounded sends are a spec
violation, not a style issue.

## Errors {#errors}

`error.code` is a stable string: `auth-failed`, `grant-expired`,
`capability-unknown`, `capability-denied`, `session-unknown`, `rate-limited`,
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
