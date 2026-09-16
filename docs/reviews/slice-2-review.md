---
title: "M1 Review: Slice 2 (web core + React)"
description: Retrospective code review of @fjarr/core and @fjarr/react as landed in slice 2 — findings, what was fixed on main, what was deferred, and what the review changed in the specs.
---

> Retrospective review per [docs/20](../20-agentic-development.md) (work
> lands on `main`, review follows): four parallel reviewers (session state
> machine; tracks/channels/router; React package; stats/protocol/tests)
> over commits `d040c79`..`374a113`, verified finding by finding against
> the code and the specs, fixed in the follow-up commit, every fix pinned by
> a regression test in `web/packages/core/test/review.test.ts`,
> `channels.test.ts`, `primitives.test.ts`, `stats.test.ts` and
> `web/packages/react/test/react.test.tsx`.

## Findings and resolutions

Severity as assessed after verification. "Fixed" means code + test landed;
"spec" means the spec was the thing to change.

### Critical / major — fixed

| # | Area | Finding | Resolution |
|---|---|---|---|
| 1 | tracks | `acquire()` spread `...options` last, so an explicit `tier: undefined` (what `useVideoTrack` passes from an optional prop) overrode the `"active"` default: every default tile streamed at thumbnail quality; `visible: undefined` disabled demand entirely | defaults applied after the spread; `update()` ignores `undefined` keys |
| 2 | tracks | `manifest_version` survived a new signaling round: after two hot-plugs (v3) any Wi-Fi blip made a restarted agent's v1 offer "stale" → never answered → connect-timeout loop → `failed`. The mock hid it (its counter was global) | reset on `detachMedia()`; docs/08 now says the sequence is per session and equal versions re-apply; mock resets per session |
| 3 | tracks | a failed/timed-out `select-tracks` dropped the demand silently (dirty cleared before the request, nothing re-sent until the next demand change) | re-marked dirty and retried with growing delay up to `MAX_FLUSH_ATTEMPTS`, then left for the next change / reconnect |
| 4 | session | rung 2 of the ladder could loop forever: each ICE failure re-sent `ice-restart`, the re-offer cleared the timer, `round` never advanced, `failed` unreachable (TURN TTL expiry on a long relay session is the trigger) | one restart per disconnection; the timer runs until ICE is actually connected; a second failure while pending climbs to rung 3 (`ice-restart-failed`); control-channel loss during `reconnecting` is no longer ignored |
| 5 | session | a synchronously throwing socket factory (bad `serverUrl`, mixed content) left the session in `connecting` forever with an unhandled rejection | `transport-failed` fatal error; both `startRound()` call sites are caught |
| 6 | session | the grant fetch ran before the connect timer was armed: a hanging host backend meant a permanent spinner | the timer now bounds the whole round, grant included |
| 7 | session | the idle policy only armed after a consumer had come and gone; a session opened with `idle` and never subscribed to stayed up forever | `touchIdle()` from `open()` |
| 8 | channels | `sendFrames()` could hang forever: a drain wait had no way to learn the channel was reset (peer gone), an already-aborted signal was never checked, and abort listeners accumulated per backpressure pause | `onBulkClose` from `ChannelSet`, aborted-signal pre-check, listeners removed on every path |
| 9 | channels | fixed-phase deadman `setInterval` allowed a silent gap approaching 2 × `intervalMs` — enough to trip an agent deadman set to the documented interval | re-armed from every send: the gap is bounded by `intervalMs` (tested with an off-phase publish) |
| 10 | react | `usePushToTalk`: `stop()` or unmount while the permission prompt was up did nothing, then the resolved `getUserMedia` attached the mic to the robot with no UI able to stop it; double `start()` leaked a mic track | generation guard after every `await`, pending-promise dedupe, tracks stopped on every unwind path |
| 11 | react | `useVideoTrack.attach` was recreated on every stream change: React detached/re-attached the element, releasing demand synchronously and re-creating the IntersectionObserver — a disable/enable flap on the wire at first-frame time and a one-frame placeholder flash | stable ref callback reading through refs; detach goes through the visibility grace; observer created once per element |
| 12 | react | `useTelemetry` cache was keyed on `(version, selector)` only: switching the `session` prop with a stable selector could serve the previous robot's value while the version counters matched | the store is part of the cache key |
| 13 | focus | core requirement #8 half-implemented: window `blur` released the keyboard but window `focus` never claimed it — after Alt-Tab into a presentation window keys went nowhere until a click on a fullscreen video | window `focus` restores that window's most recently focused view; re-registering an id keeps ownership; `focus()` on an unregistered id is ignored |
| 14 | stats | the freeze rule compared *cumulative* freeze time to the 500 ms threshold, so any session with 500 ms of history rated every micro-freeze `degraded`; a track's first sample rated `poor` ("no frames"); an enabled track with no `inbound-rtp` report at all never affected health; loss was averaged per track (a 5 pkt/s audio track dominated) | windowed freeze/decode/jitter-buffer metrics with `null` on the first sample; "no media stats" for a bound, enabled track without a report; loss pooled by packets; stats cleared on `stop()` |

### Minor — fixed

Envelope size cap measured in UTF-8 bytes, not code units · `Emitter` isolates
a throwing handler · monitors store keeps identity unless geometry changed ·
time-sync window reset on every media teardown (rebooted robot, new clock) ·
`grant-expired` is a free round (no backoff, not charged) · `session-unknown`
closes instead of failing · ICE `failed` during the initial connect starts a
new round immediately · `retry()` from `reconnecting` skips the backoff ·
the round budget is renewed only by a 30 s stable connection (docs/08 rule) ·
`fail()` emits an `error` event and exhaustion has its own code · WebSocket
close codes survive an `error` event · candidates trickled while an ICE
restart is pending are queued for the re-offer · end-of-candidates sent once
· no-ICE-servers warning · guard checks `agent_info`/`client_info`/`params`
objects · `monitor` normalized to `null` · v2 frames warn · compile-time
witnesses for the DOM seams · `ConnectButton` on `useSyncExternalStore` ·
autoplay "blocked" only on `NotAllowedError` · mute toggles demand in place ·
`useBeforeUnloadWhileConnected(client)` works without a provider ·
`FloatingVideo` ends drags on `pointercancel` · registered views receive
`{ session, capability }` (docs/05) · public types re-exported from
`@fjarr/react` · demo disposes the client on Vite hot reload.

### Spec changes made by the review

- docs/08: `manifest_version` is monotonic *within a session*; equal
  versions re-apply (ICE-restart re-offers); heartbeat death on the operator
  side climbs the ladder (docs/08 and docs/21 had disagreed).
- docs/21: state-machine rows for `retry()` from `reconnecting`,
  `session-unknown`, the free `grant-expired` round, the round budget and
  connect-timeout rules.
- docs/22 core requirement #2: `preference` goes on the wire,
  `latencyMode` is browser-side only.
- docs/05: registered views receive `{ session, capability }`.

### Second pass: adversarial review of the fix commit

Because the first pass found so much, a second, narrower pass reviewed the
diff of the fix commit (`2a95c2d`) with an adversarial brief: break the
fixes. It found 16 items; the ones worth naming:

| # | Area | Finding | Resolution |
|---|---|---|---|
| 15 | session | a host calling `close()` from its `state` listener during `reconnecting` (or `connected`) left a zombie round: the timer was armed *after* the listener ran, so a socket, peer connection and sampler outlived the closed session | timers armed before the transition is announced; `startRound()` refuses a terminal state; `maybeConnected()` re-checks after announcing |
| 16 | session | the free `grant-expired` round was an unbounded hot loop for a host minting tokens the server rejects | one free refresh per attempt (reset by `hello-ack`); a second consecutive `grant-expired` is a counted, backed-off round |
| 17 | channels | a bulk wait started before any bulk channel existed still hung on peer loss (`onBulkClose` only covers attached channels); `ByteChannel.ready()` had no close path | `ChannelSet.onReset` rejects every waiter |
| 18 | channels | an oversized publish left the rejected payload in `last` and skipped re-arming, so the deadman went silent while held | `last` only ever holds a sent value; the timers are armed before the transmit |
| 19 | react | `usePushToTalk`: an unwinding stale `start()` cleared a *newer* run's pending guard, so a third `start()` could open a second live microphone; two `replaceTrack` calls could land out of order | per-run guard, `mic.current` re-check after every await, all `replaceTrack` calls serialized through one chain |
| 20 | testing | the mock answered on the *latest* peer connection's control channel, so two concurrent sessions would spuriously reconnect after 15 s | replies on the peer connection the request came from; a 25 s two-session test |
| 21 | tracks | after `MAX_FLUSH_ATTEMPTS` every later demand change got a single un-retried shot | a demand change resets the budget |
| 22 | session | a rung-3 round whose fresh ICE failed asked for a restart on a session that never connected; a `failed` from the old ufrag before the re-offer counted as the restart failing | "never connected on this peer" climbs at once; restart failure is judged only after the re-offer is applied |
| 23 | stats | "no media stats" rated a starting track `poor` in browsers that materialise the report on the first packet; `stop()` kept the dead peer's health level | the rule applies only to tracks that have reported once; health resets with stats |
| 24 | react | `useInputFocus` unregistered on every option change, so ownership was lost on the exact window transition docs/22 #8 covers; `<AudioSink muted>` ignored later prop changes; an option change mid-visibility-grace flapped demand | re-register without unregistering, unmount-only unregister; prop effect; a pending grace counts as visible |

Also: `update(null)` clears an acquire option; late trickle candidates for the
old ufrag are dropped at restart; spurious `error` events after a teardown
mid-offer are suppressed. Every item has a regression test.

### Deferred (recorded, not fixed here)

| Finding | Why deferred | Where it goes |
|---|---|---|
| A WSS blip tears down a healthy media path (rung 3 has no session resume) | needs a `hello` with `session_id` resume in the protocol and agent support | slice 3 design note; docs/18 candidate |
| `TrackRegistry.publish()` rebuilds every entry object, so all tiles re-render on any registry change | measured as acceptable for a dozen tiles; identity-preserving publish is a cheap later optimization | note in docs/21 testing section |
| Mock agent brokers hello → offer in one microtask; ordering races are covered by hand-driven tests only | fidelity vs. simplicity; the C++ agent + Playwright e2e (slice 5) cover the real timing | slice 5 |
| `useInputFocus` defaults `window` to the global window; portaled views must pass theirs | correct per docs/22 #8; the M3 `<DesktopView>` is the caller that must do it | M3 |

### Test coverage added

Invalid golden fixtures for `hello` without an `auth` object, a bad
`hello-ack.turn`, a bad `monitor` + negative `manifest_version`, `error`
without `code`, an envelope with an array payload and an empty `type` (all
three tiers now reject them); the state-machine rows that were missing
(session-reject, session-close, rate-limited, auth-failed, unknown codes,
session-unknown, control-channel close, ICE recovered within the grace, ICE
failed during connect, retry from reconnecting, relay policy, channels
opening after `connected`, connect-timeout with a hanging grant and with a
never-acking server); multi-sample health with hysteresis; freeze windowing
and the legacy `track` keying; focus across three views and two windows;
deadman gap bound; bulk hang on reset and abort-listener accounting; UTF-8
cap; and on the React side push-to-talk unwinding, attach stability with a
controllable IntersectionObserver, and the cross-session telemetry cache.

## Conclusion

Slice 2 stands, with 14 real defects fixed in the first pass and 10 more
in the adversarial second pass, before any agent exists to hit them; the most consequential were the two that made a long session
unrecoverable (findings 2 and 4) and the microphone left open (10). The
review also confirmed the design choices that matter: generation guards on
every async boundary, renegotiation diffing by `track_id`, one owner per
session for demand, and the mock agent as the test substrate. The second
pass's yield (10 in ~900 lines of fixes vs. 14 in ~4,000 lines of original
code) is the argument for stopping here: the remaining risk is the
mock-versus-browser gap, which only the slice-5 end-to-end run against a
real agent can close. Slice 3 (agent core, C++) starts from the protocol as
amended here.
