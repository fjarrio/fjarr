---
title: Pipeline Introspection
description: Live, visual and machine-readable introspection of the agent's GStreamer pipelines — for developers, for customers extending the product, and for AI agents building and operating it. A product feature, not a debug flag.
---

> **Status: review** — specified with slice 3 ([docs/23](23-agent-core-architecture.md));
> the core-side walker and local endpoint land in slice 3, the session
> capability and the dashboard viewer in slice 5. Product positioning in
> [docs/03](03-product-strategy.md).

## Why this is a feature

Every WebRTC/GStreamer product ends up debugged by staring at DOT dumps.
The camera streamer made them a first-class debugging tool and it was the
single most useful thing it had ([prior art](11-prior-art.md#camera-streamer)).
Fjarr makes that experience **live, shareable and machine-readable**, and
sells it: a robot company that adds a new camera source
([docs/09](09-interfaces.md#the-video-source-contract)) or a third-party
capability must be able to *see* what the media plane did with it — in a
browser, from the dashboard, on the robot, and through an API an AI coding
agent can read while it builds and deploys the integration.

Three audiences, one data source:

| Audience | Needs | Surface |
|---|---|---|
| **Developer** (us, on the robot or in the devcontainer) | the graph of every pipeline as it changes: caps, states, queue levels, valves, ICE/DTLS, data channels; scrub back to "what did it look like when the offer was created?" | local viewer at `http://127.0.0.1:7381/`, `make introspect`, DOT/JSON files in `dot_dir` |
| **Customer** (integrator extending the product, support engineer) | the same view from the fleet dashboard, per robot and per session, without shell access; an exportable diagnostics bundle for a ticket | `fjarr.introspect` capability + `<PipelineGraph>` in `@fjarr/react`; `fjarr-agent --diagnostics` |
| **AI agent** (building, testing, deploying) | a compact, stable, machine-readable model of the pipelines and their history, fetchable with one HTTP call, small enough for a context window | `GET /pipelines/<id>.json` and `.txt` (the summary form), the events stream, the JSON schema in `protocol/schemas/` |

## The data model

The core walks a `GstBin` and produces one **graph snapshot** per pipeline,
in three renderings from the same walk:

- **DOT** — `gst_debug_bin_to_dot_data(GST_DEBUG_GRAPH_SHOW_ALL)`, exactly
  what GStreamer itself renders, so any GStreamer developer's tooling and
  intuition transfer. Fjarr adds a header comment with the metadata below
  and stable node ids (element names are made stable and meaningful:
  `cam-front.active.encoder`, `s-01.desk-HDMI-1.valve`).
- **JSON** (`introspect.schema.json`) — the same graph as data: elements
  (name, factory, state, selected properties: `drop` on valves,
  `current-level-*` on queues, `bitrate` on encoders, `connection-state` /
  `ice-connection-state` / `signaling-state` on `webrtcbin`, `ready-state`
  and `buffered-amount` on data channels), pads with negotiated caps and
  counters (buffers, bytes, last PTS — from lightweight probes the core
  already has for stats), links, and bins nested. Deterministic key order
  so diffs are meaningful.
- **Addressing.** Elements are named by the core with a stable grammar
  used everywhere (DOT node ids, JSON `name`, test assertions):
  `producer:<track_id>/<role>` for the shared head of a producer (roles:
  `source`, `convert`, `rawcaps`, `tee`) and `producer:<track_id>:<tier>/<role>`
  for each tier branch (roles: `queue`, `encode` — the encoder bin, whose
  children are `encoder`, `parser`, `caps` — and `sink`), and `session:<sid8>/<track_id>/<role>`
  (roles: `appsrc`, `queue`, `valve`, `payloader`) plus
  `session:<sid8>/webrtc`, where `<sid8>` is the first 8 characters of the
  UUIDv7 `session_id` (the full id is in the snapshot metadata). A test
  asserts `session:0192f3a1/test-pattern/valve.drop == false`.
- **Summary text** — a few hundred characters per pipeline in topological
  order: `videotestsrc(PLAYING) → vah264enc[bitrate=4M] → appsink … ;
  consumer s-01: appsrc → queue[3/30] → valve[open] → rtph264pay(pt=96,
  ssrc=…) → webrtcbin[connected, ice=connected, dtls=connected] ; DC
  control=open(0 B) realtime=open`. This is the form an AI agent (or a
  support engineer reading a ticket) wants first.

Every snapshot carries: `pipeline_id` (`producer:<track>` — one pipeline per track, its tiers are branches —,
`session:<session_id>`, or `agent` for the combined view), `session_id`
and `robot_id` when applicable, `generation`, the triggering **milestone**
or event (`state-changed`, `caps-fixed`, `offer-created`, `renegotiation`,
`select-tracks`, `producer-restart`, …), `ts`, and a monotonically
increasing `seq` per pipeline.

**Triggers and rate.** A snapshot is taken on every session milestone
(docs/23), on every producer/consumer state change, on `select-tracks`, on
renegotiation, on producer restart and plane rebuild, and on demand.
Snapshots are coalesced to at most one per pipeline per 250 ms —
trailing-edge, so a burst of triggers yields one snapshot of the *final*
state, never a stale one — and the walk runs on the core loop (it reads
element state without locking the streaming threads). Cost is bounded: a graph is a few KB; a busy robot with
three sessions produces well under 100 KB/s at peak and nothing when idle.

**History (flight recorder).** The core keeps the last 64 snapshots per
pipeline (configurable), so a viewer can scrub back through a
negotiation or a hot-plug, and a diagnostics bundle contains the story,
not just the end state. Closed sessions keep their last 8 snapshots for
10 minutes.

## Surfaces

### On the robot: the local introspection endpoint

`fjarr-agent` (and any embedder that enables it) serves a small HTTP
endpoint on `127.0.0.1:7381` (libsoup-3 server, already a dependency;
Unix socket alternative `introspect.socket = "/run/fjarr/introspect.sock"`):

| Route | Returns |
|---|---|
| `GET /` | the bundled viewer (below) |
| `GET /pipelines` | JSON list: id, kind, state, session, seq, last milestone |
| `GET /pipelines/<id>.dot` / `.json` / `.txt` | latest snapshot in that form; `?seq=<n>` for history |
| `GET /pipelines/<id>/history` | the snapshot sequence (metadata only; bodies via `?seq`) |
| `GET /events` | Server-Sent Events (`event: snapshot`, `id: <pipeline>@<seq>`, `data:` the metadata JSON, plus the body when `?body=json\|dot\|txt`; `retry: 1000`) — this is what "live" means |
| `GET /stats` | the per-session `get-stats` sample, FrameHub counters, producer states |
| `GET /sources` | configured video sources with negotiated caps and availability, and for a missing driver the catalog entry and install command ([docs/26](26-robot-install-and-drivers.md)) |
| `GET /memory[?since=<checkpoint>]` / `POST /memory/checkpoint` (checkpoint = an opaque token, currently the census `seq`) | RSS, live GStreamer/GLib object census by type (elements, pads, samples, promises, sources), FrameHub buffers held, channel bytes buffered, sessions/pipelines alive — and the diff since a checkpoint (the soak-test oracle, [docs/23](23-agent-core-architecture.md#memory-and-lifetime-discipline-and-the-tooling-that-enforces-it)) |
| `POST /snapshot?pipeline=<id>` | force a snapshot now |
| `GET /diagnostics.tar.gz` | the diagnostics bundle |

Security: bound to loopback by default; `introspect.bind = "0.0.0.0"` plus
`introspect.token` is required to expose it on a LAN; it never carries
credentials or grants (docs/10). The only mutations are `POST /snapshot`
and `POST /memory/checkpoint`, both harmless. A port already in use is a
startup error (exit 1) — a silently absent endpoint would break the
tests that rely on it. Served by libsoup-3's server on the core context
(no new dependency).

### The bundled viewer

A single-page viewer served from the endpoint, rendering DOT in the
browser (d3-graphviz on `@hpcc-js/wasm`, both permissively licensed —
docs/14) so the robot needs no Graphviz installation and no network:
pipeline list on the left, the live graph in the middle (nodes that
changed since the previous snapshot highlighted, caps on hover, state
colouring), a timeline scrubber below with milestones marked, and the
summary text and JSON as tabs. Layout updates are animated between
snapshots so a renegotiation reads as "this branch appeared", not as a
new picture. `make introspect` in the devcontainer opens it against the
running demo-robot.

### From the dashboard: the `fjarr.introspect` capability

A built-in, peer-consumer capability ([docs/06](06-capabilities.md#fjarrintrospect--pipeline-introspection-slice-3-core-slice-5-ui))
exposing the same snapshots over the session so a customer sees a robot's
pipelines from the fleet dashboard without shell access:

| `type` | kind | payload | channel |
|---|---|---|---|
| `pipelines/list` | request → result | `{pipelines: [...]}` | control |
| `pipelines/subscribe` | request → result | `{pipeline_id?: "*", forms: ["txt","json","dot"]}` — then `snapshot` events | control |
| `snapshot` | event | metadata + the requested forms; bodies over 12 KiB ride `fjarr:bulk:fjarr.introspect` as a referenced blob | control / bulk |
| `pipelines/history` | request → result | `{pipeline_id, seq_from?}` | control / bulk |
| `stats` | request → result | as `GET /stats` | control |

Gated by a grant param: `{"name":"fjarr.introspect"}` in the grant, which
the customer's backend issues to developer and support roles only — a
pipeline graph reveals device paths, encoder settings and session ids.

`@fjarr/react` ships `usePipelines(session)` / `usePipelineSnapshot(session,
id)` (docs/21 selector mode, newest-wins per pipeline) and `<PipelineGraph
session pipelineId>` — the same viewer as a component, headless-first, so
the demo dashboard's *Diagnostics* tab is ~20 lines. Fjarr Cloud later
shows the same view fleet-wide (M7), which is where the retention and
cross-robot search live — the hosted-value line of docs/03.

### For AI agents

Nothing special is needed beyond the endpoint: `curl
127.0.0.1:7381/pipelines/session:s-01.txt` is a complete, current answer
to "what is the media plane doing", and `/events` is how an agent watches
a change it just deployed take effect. The repo's `/verify` and the slice-5
e2e tests use the same endpoint to assert pipeline shape (e.g. "after
`select-tracks`, `s-01.cam-front.valve.drop == false`"), which keeps the
introspection output honest — it is a test oracle, not decoration. The
JSON schema lives with the protocol schemas and is versioned like them.

### Files and bundles

`dot_dir` (config / `FJARR_DOT_DIR`) writes every snapshot as
`<pipeline>-<seq>-<milestone>.dot` (+ `.json`, `.txt`) for offline work.
`fjarr-agent --diagnostics [out.tar.gz]` writes a bundle: config (secrets
redacted), the doctor report, `/pipelines` history, `/stats`, `/sources`,
the last 10 minutes of the log, versions — what a support engineer asks
for first, produced in one command.

## Implementation notes (docs/23 hooks)

- The walker is one class in the media plane (`PipelineIntrospector`)
  called with a `GstBin*` on the core loop; the DOT string comes from
  GStreamer, the JSON/summary from the same recursive walk over
  `GST_BIN_CHILDREN`, pads and `gst_pad_get_current_caps`. Counters come
  from the pad probes the stats sampler already installs.
- Snapshots are stored in a per-pipeline ring in the `MediaPlane`; the
  endpoint and the capability are two readers of the same ring.
- The endpoint runs on the core context (libsoup async server), so serving
  a snapshot never blocks streaming; bodies are served from the ring
  without re-walking.
- Element naming is a core rule from slice 3 on: every element the core
  creates gets a stable, meaningful name; capability-provided source bins
  are wrapped in a bin named after the track.

## Slice mapping

- **Slice 3b** ✔ (2026-09-20): walker (DOT/JSON/summary), `dot_dir`
  files, the endpoint with `/pipelines`, `/pipelines/<id>.{json,txt,dot}`
  (`?seq=`), `/pipelines/<id>/history`, `/sources`, `POST /snapshot`, and
  `introspect.schema.json` under the conformance gate — the lab validates
  a live session snapshot against it and asserts `…/valve.drop` follows
  `select-tracks` within a second.
- **Slice 3c**: `/events`, the history ring and `?seq`, `/stats`,
  `/memory` + checkpoints, `/diagnostics.tar.gz`, `make introspect`,
  `fjarr-lab introspect`.
- **Slice 5**: `fjarr.introspect` capability, `<PipelineGraph>` + hooks,
  the demo dashboard Diagnostics tab; the same built component is served
  from the endpoint's `GET /` as a data file, so the viewer is built once.
- **M7**: fleet-wide retention and search in Fjarr Cloud.

## Acceptance

Slice 3b/3c, via `curl` and `fjarr-lab`: with demo-robot streaming to one
operator, `/pipelines` lists the producer and the session pipeline;
toggling a track in the dashboard changes `…/valve.drop` in the JSON
within a second; `hotplug` makes the new branch appear and the history
gain a `renegotiation` event; `?seq=` at `offer-created` returns the
pre-renegotiation graph; `/pipelines/session:<id>.txt` is under 2 KB; the
diagnostics bundle contains the whole story; an idle robot produces zero
snapshots per second. Slice 5 adds the same through the viewer and the
dashboard.
