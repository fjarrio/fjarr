---
title: "Slice 3b Review"
description: Retrospective review of the agent core (docs/23) — what fjarr-opsim and the browser lab found in the first real C++ agent, what two reviewer passes found in the core, media plane and introspection code, what was fixed, and what is deferred.
---

> Retrospective review of slice 3b per docs/13 and docs/20, run before the
> slice was committed. The slice is the first `libfjarr` that streams to a
> real browser: the core loop, sessions, the DataChannel router, the media
> plane (producers, FrameHub, consumers), the introspection endpoint and
> `fjarr-opsim`. Two reviewer passes by area (core loop, sessions, router,
> daemon; media plane and introspection), each finding verified against the
> code and fixed with a regression test where one applies, plus the
> defects the operator simulator and the lab found while the slice was
> built.

## What the harnesses found

`fjarr-opsim` (a `webrtcbin` answerer that scripts an operator, docs/23)
and the browser lab (docs/25) ran against the demo robot throughout the
slice. Everything below is fixed and covered by the scenario or test that
found it:

| Found by | Defect | Fix |
|---|---|---|
| lab hot-plug | a track added by renegotiation lost a GOP on the *untouched* track: the first offer carried no `a=ssrc` and the re-offer a random one, so Chromium recreated its receiver | the payloader's SSRC goes into the transceiver's `codec-preferences` with the payload type; unchanged demand is a no-op; PLI/FIR are relayed from the consumer's `appsrc` to the producer |
| lab silent pings | an SCTP write error when the client tore its peer down was reported as `media-error` | bus errors from the SCTP/DTLS/ICE elements close with `ice-failed`; only the rest of the pipeline is `media-error` |
| opsim smoke | `pong.t0` echoed a 32-bit truncation of the operator's millisecond clock | `t0` is read as `int64` |
| opsim silent-operator | `deadman{expired}` from `release_all_input` never reached the operator: the consumer pipeline went to NULL in the same loop callback | closing has a 150 ms flush window: detach, `session-close` and valves at once, pipeline teardown deferred, inbound dropped meanwhile ([docs/23 state table](../23-agent-core-architecture.md#agent-side-session-state-machine)) |
| opsim no-answer | the negotiation timeout named `ice-gathering-complete`, a local step, instead of the offer the peer never answered | only peer-facing milestones re-arm and name the watchdog |
| opsim no-answer (after the flush window landed) | a `drive` event arriving inside the window re-armed a deadman on a context the deferred close then destroyed (SIGSEGV, reproduced under gdb) | every inbound envelope is dropped from `closing` on; the hub is unsubscribed again at the end of the window |
| opsim ice-restart (after the flush window landed) | the reconnecting operator registered the same track id before the old session's deferred close unregistered it, and the new session's video stalled after two frames | track registrations on the media plane are counted |
| opsim ice-restart | `session-close{retry:true}` waited out the flush window | signaling is sent at close time; only the media teardown is deferred |
| ASan/LSan | nine leaked promises per session: webrtcbin only *refs* the promise it is handed | the change function releases the caller's reference; the closure is freed on `EXPIRED` too |

## Reviewer findings: core loop, sessions, router, daemon

| # | Severity | Finding | Resolution |
|---|---|---|---|
| 1 | high | the deferred close left capabilities reachable after `session_detached` (the crash above) and `close_all("media-restart")` rebuilt the plane while consumers still held their sinks | inbound dropped from `closing`; `close_all` always completes the close synchronously; test waits for `closed` |
| 2 | high | `SessionManager` erased sessions through an unguarded post that could run after shutdown freed the manager | guarded by an alive token |
| 3 | high | exceptions thrown inside the router escaped the GLib callback and killed the daemon: `{"tracks":[1]}`, a non-numeric `t0`, a 17 KiB `track_id` echoed into an error reply | `select-tracks` validates shape before reading; the router, the signaling hooks and the RAII kit's trampolines catch `std::exception`; inbound size capped at 16 KiB; error text clamped; `send_control` never rethrows |
| 4 | high | the `on-message-string` handler dereferenced the raw `Session*` on the SCTP thread (a data race on the generation, a use-after-free window against `disconnect`) | handlers box `{weak session, generation, loop, label}` and only post (docs/23 callback context) |
| 5 | high | the bulk `on_drain` callback ran on webrtcbin's thread with a raw sender pointer | posted to the loop, sender looked up by label |
| 6 | high | SIGTERM ran the whole shutdown inside the signal handler (malloc, GStreamer, libsoup in async-signal context; a deadlock when the signal landed on a streaming thread) and the 3 s `_Exit` could skip `release_all_input` | `Agent::stop_on_signal()` installs a `g_unix_signal` source on the core loop; the deadline thread starts from the loop; both daemons use it ([docs/09](../09-interfaces.md#embedding)) |
| 7 | medium | a renegotiation queued while an offer was out was checked before the answer's promise settled and never re-offered | the queued re-offer is driven by the `remote-description-set` milestone |
| 8 | medium | `session-close{agent-shutdown}` was queued and then the socket dropped; operators saw `peer-gone` | the close handshake is pumped for up to 300 ms before the socket goes |
| 9 | medium | a media-plane rebuild closed sessions without `retry:true` | `close_all(reason, retry)`; `media-restart` is a reconnection rung (docs/08) |
| 10 | medium | the input lease outlived its owner's last session by up to 30 s; read-only sessions were never promoted | the lease clears with the owner's last session; promotion stays a reconnect (docs/23 note) |
| 11 | medium | a deferred snapshot timer could fire after the store was destroyed | the store's rings own their timers |
| 12 | medium | `gst_object_ref` on plain `GObject`s outside the kit; the gate did not cover refs | `glib::ref_object`; the gate now refuses `g_object_ref`/`gst_object_ref` outside the kit |
| 13 | medium | spec drift: the daemon only printed a notice outside systemd, `watchdog_secs` was parsed and unused | the daemon refuses to start outside systemd unless `allow_unsupervised`; `watchdog_secs` overrides the unit's interval (docs/23 configuration) |
| — | low | shutdown could run twice; queued thread-pool jobs leaked at exit; `SignalConnection` counted failed connects; a build failure sent `session-close` before any `session-accept`; unavailable tracks stayed registered; non-docs/08 error codes; unchecked `atoi` for integer env vars, unvalidated `log_level`/port; docs/09 and docs/23 signatures drifted from the headers; an unused `GenerationToken`; a test read a counter off the loop | all fixed; `session-reject` for a failed build; `FJARR_*` integers must parse; docs updated to the headers |

Checked and found consistent with the specs: every `// spec:` backlink,
the RAII kit's ownership symmetry, the loop's start/stop handshake and
`call_sync`, the signaling client's context affinity and backoff, the
protocol codec and its limits, config precedence, the session state
machine and channel parameters per docs/08, the deadman and
`release_all_input` order per docs/15, the lease per docs/10, exit codes
per ADR-0019, `fjarr.test` per docs/06, and that `demo-robot` uses public
API only.

## Reviewer findings: media plane and introspection

| # | Severity | Finding | Resolution |
|---|---|---|---|
| 1 | high | starting a second tier on a PLAYING producer synced the branch upstream-first; the queue pushed into the not-yet-READY encoder bin, took `FLUSHING` and parked the tee for good with no bus error (reproduced with a probe pipeline) | branch synced sink → encoder → queue, then linked to the tee; regression test starts the thumbnail tier on a running producer and asserts the active tier keeps delivering |
| 2 | high | the producer-restart path destroyed the `Producer` from inside its own bus-watch dispatch and then used a `track_id` reference that lived in the destroyed closure | the error hook posts (guarded) to the loop |
| 3 | medium | a late joiner got the retained keyframe and then *current* deltas whose references it never saw (corrupt decode until the requested keyframe) | the whole ring (keyframe first) is delivered to the joiner only, deduplicated by sequence number; the hub test pins the order |
| 4 | medium | delivery stats were updated by index after the lock was dropped; a concurrent (un)subscribe could mark the wrong subscriber | matched by sink identity |
| 5 | medium | `gst_object_set_name` on parented elements is a no-op, so pooled transceivers kept the old track's names and relayed PLI with the old id | a removed track's branch is torn down (the transceiver stays pooled); a re-added track rebuilds its branch under its own name with a fresh probe |
| 6 | medium | `SnapshotStore::retire()` had no caller: closed sessions' rings grew for the life of the process | retired from the session-ended event (docs/24: last 8 for 10 min) |
| 7 | medium | `stop()` reset the sink's `appsrc` while the hub thread could be inside `push()` | the sink keeps its reference; a push into a NULL-state appsrc returns `FLUSHING` |
| 8 | medium | CI maps `/dev/dri` into the demo robot; a hosted runner without it fails `compose up` before any test | the e2e job creates an empty `/dev/dri` (maps nothing, keeps the software encoder path) |
| 9 | medium | the restart timer only restarted tiers that had been running; a tier whose start failed left its subscribers waiting forever | every tier with demand is started after a restart |
| 10 | medium | hub hooks posted a raw `MediaPlane*` that shutdown could free first | guarded by an alive token |
| — | low | signal handlers copied `alive_` on webrtcbin's thread while `stop()` reassigned it; the promise closure leaked on `EXPIRED`; a no-op idle probe in `stop_tier`; the thumbnail profile ignored the source size before PLAYING; partial bins leaked on recoverable parse errors; no NULL check after `gst_buffer_make_writable`; the docs/24 naming grammar did not match the one-pipeline-per-track layout and the encoder bin was unnamed; the consumer queue was a hard-coded 2 s instead of one GOP; the PTS base was reset at every resync; the endpoint split a dotted pipeline id on the last `.`; UBSan findings did not fail the ASan preset; a per-gap warning from the streaming thread | all fixed: boxed contexts, `EXPIRED` frees the closure, probe removed, declared caps size the thumbnail, bins sunk, NULL checked, docs/24 grammar and `producer:<track>:<tier>/encode`, queue = `gop_seconds`, sticky PTS base, known suffixes only, `-fno-sanitize-recover=undefined`, gap logged at debug |

Checked and found fine: kit refcounts with `gst_bin_add`, `adopt_pad`
only on transfer-full returns, metadata-only buffer copies, appsink
callback lifetimes, bus-watch ownership, the promise flow (refcount traced
1→2→3→2→1→0 on 1.28), ICE candidate queuing per re-offer, the valve-first
removal order per ADR-0022, the encoder policy with no silent fallback,
snapshot coalescing, the libsoup server's context affinity and token
policy, the stamp layout against docs/25, and the CI package lists.

## Deferred

- A producer bus-error → restart test under ASan (`identity error-after`
  as a `gst` source) — with the `soak` scenario in slice 3c.
- Promotion of an open read-only session when the lease frees.
- The duplicate `deadman{expired}` a capability emits before the core's own
  expiry (harmless, one wire event).
- `CoreLoop::quit()` before `run()` has entered the loop is lost (no caller
  does this; noted for the embedding API).
- TSan as a gate (blocked in the container by seccomp, docs/12).
