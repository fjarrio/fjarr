---
title: "M1 Gate Review"
description: The M1 gate checked promise by promise against evidence — what held, what was claimed but had never actually run, and the two product defects the checking found.
---

> The milestone gate review per docs/13 and docs/20, run after slice 7b closed
> M1's last slice. Each slice already has its own retrospective review, so this
> one does not re-read the milestone's code. It does something the slice reviews
> structurally cannot: take each promise in the [M1 gate](../17-roadmap.md) and
> ask what evidence exists that it is true. Two of them turned out not to be.

## The gate, promise by promise

| Promise | Verdict | Evidence |
|---|---|---|
| demo-robot streams **2 tracks to 3 browsers** through the sidecar | met, on CI | `tests/stack/camera.spec.ts` — three pages, both camera tracks, one producer each. Getting here took two corrections of this review's own making ([below](#what-this-machine-cannot-check)) |
| the demo backend **minting grants** per ADR-0015 | met since slice 3b | every `stack` test connects with a grant the demo backend minted; role-scoped grants in `introspect.spec.ts` |
| the demo backend **receiving webhooks** per ADR-0015 | **was not met** — now met | finding 1: nothing had ever delivered one. `tests/stack/contract.spec.ts` now asserts signed `session.started`/`session.ended` arrive and that forged ones are refused |
| **reconnect + ICE restart** under fault injection | met | `agent.spec.ts` (ice-restart → `session-close{retry:true}` against the real agent), `ladder.spec.ts` (server restart, silent agent, peer-gone), `faults.spec.ts` (killed agent, server restart under the **real** agent) |
| **latency harness** reporting against budgets | met | slice 7b: `tests/stack/latency.spec.ts`, `make latency`, `web/e2e/latency.csv` |
| TS types conform to the **golden fixtures** | met, slice 2 | open question #15, closed 2026-09-16 |
| written **API-fit review** for the remaining capabilities | met, M1 planning | [m1-api-fit-review.md](m1-api-fit-review.md) |

## What the checking found

| # | Severity | Finding | Resolution |
|---|---|---|---|
| 1 | high | **The webhook half of ADR-0015 had never run.** The sidecar implements HMAC-signed delivery with retries; the demo backend has had a receiver since M0. Nothing pointed one at the other: no `FJARR_WEBHOOK_URL` anywhere in compose. The receiver also did not verify the signature, so it was an open endpoint anyone on the network could post session events to. The gate has claimed this since M1 was written | compose and `make demo-up` wire the sidecar to the demo backend; the receiver verifies `x-fjarr-signature` in constant time, refuses with 401, and de-duplicates by `event_id` (delivery is at-least-once); `contract.spec.ts` asserts both halves; CI sets the URL |
| 2 | high | **TURN URLs in the standard form were silently ignored by the agent.** `turn:host:port` is RFC 7065 and what every browser and every server config uses; webrtcbin wants `turn://user:pass@host:port`. Both webrtcbin consumers required the `://`, and `continue`d without a log otherwise — so a robot handed a perfectly ordinary TURN URL had **no relay path and said nothing about it**. It only surfaces behind the symmetric NAT that TURN exists for, which is the case docs/02 says to plan capacity for | `protocol::turn_url_with_credentials` accepts both forms, URL-escapes the credentials, and returns empty for anything else so the caller **logs** it instead of dropping it. Unit-tested over both forms, `turns`, a query string, base64 credentials and five rejects. With it, the `relay-only` simulator scenario connects for the first time: `add-turn-server turn:coturn:3478 -> ok`, connected in 134 ms over a relay candidate |
| 3 | medium | The fan-out acceptance assertion read `/stats` **once** and demanded exactly three subscribers. `/stats` is a global instantaneous view: sessions from an earlier run may still be draining (it read 5) or this run's third viewer may not have subscribed yet (it read 2). It could only pass on a freshly started robot, which is CI and nowhere else | it polls until the hub settles at three subscribers per track with one producer each. The property is unchanged; the sampling is no longer a race |
| 4 | medium | `faults.spec.ts` is the only suite that stops the robot. An assertion failing between the kill and the restart would leave it down, so one real defect would be reported as a dozen fake ones in every later test | an `afterEach` restores it whatever happened |
| 5 | high | (slice 7a, already fixed) a tier's framerate was a target rather than a ceiling, so **no camera below 30 fps could stream on any tier** | the rate filter is a range; regression test verified to fail without the fix |
| 6 | low | The `relay-only` simulator scenario asserted that **both** ends use a relay candidate, which is a claim about the robot's configuration rather than about the session's path | it asserts the operator's local candidate is relay — with the operator forced to relay, every packet traverses coturn regardless of the agent's candidate — and reports the remote type as detail |

## Measured

| Check | Result |
|---|---|
| Webhooks for one session | `robot.online`, `session.started`, `session.ended`, each signed and verified; unsigned and wrongly signed posts both refused with 401 and not recorded |
| TURN after the fix | `add-turn-server turn:coturn:3478 -> ok`; relay session connected in 134 ms; 34 frames in 1 s over the relay |
| Hub at rest | 0 subscribers on every track and tier with no sessions — the drifting counts in finding 3 were sampling, not a leak |
| Killed agent | operator saw `peer-gone:agent-disconnected` 629 ms after SIGKILL; streaming again 6.9 s later |
| Glass-to-glass | clean 191/210 ms, lossy 198/263 ms, relay 173/205 ms on this laptop — recorded, and correctly refused as a verdict (below) |

## What this machine cannot check {#what-this-machine-cannot-check}

Several media suites pass one at a time and fail together on a developer
laptop: three viewers decoding six streams while the robot software-encodes
them saturates the machine. The frame stamp measures it rather than leaving it
to opinion — 13 frames in 1.5 s with a counter gap of 8, or 1 frame where 15
are expected. The same starvation makes the simulator's hot-plug scenario
intermittent when run after others, while it passes alone.

This is recorded in [docs/25](../25-browser-lab.md), and it is why the latency
harness refuses to hold a CPU-limited run to a budget.

**Two corrections, both of this review's own making.** They are left here
rather than tidied away, because the sequence is the lesson.

Finding 3 replaced a fixed two-second wait with a poll on the hub's
subscriber count. That was right, but the two seconds had been doing a second
job nobody had written down: giving the frame-stamp watchers time to collect
frames before the next assertion read them. On a fast machine the poll returns
in milliseconds, so that assertion began reading empty watchers.

I then mis-read the resulting CI failure as hardware. It looked exactly like
the starvation documented above, so I tagged the suite `@heavy` and moved it
to the nightly — a workaround for a problem I had introduced, resting on a
diagnosis that the evidence did not support. The tell was there and I walked
past it: the suite failed in **3.7 seconds**, far too fast to be a machine
running out of breath, and it had been green on hosted CI before this review
touched it.

The fix is to wait for the frames themselves rather than for a duration, which
depends on no machine's speed. With that, the suite passes on a four-core
runner and on a laptop that genuinely does starve under load. The `@heavy`
mechanism is gone: it existed only to serve the wrong diagnosis.

The starvation described above is real and still applies to the media suites
run back to back. It simply was not this.

## Follow-ups

- Add `relay-only` to the CI simulator set once it has been seen stable on CI
  hardware. It gates the path finding 2 shows was silently broken, but it is
  CPU-limited on this laptop and a gate that cannot be trusted is worse than
  none, so it is not added on a guess.
- The agent's own `FJARR_ICE_POLICY=relay` — a robot forced to relay — is
  still untested; finding 6 explains why the old assertion looked like it
  covered this and did not.
- Input-to-photon lands with `fjarr.desktop` in M3, on the slice-7b rig.
