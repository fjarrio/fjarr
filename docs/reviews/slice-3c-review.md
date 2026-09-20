---
title: "Slice 3c Review"
description: Retrospective review of introspection completeness and the memory ladder (docs/23, docs/24) — what the new tooling found in the agent on its first day (an upstream webrtcbin leak among them), what the reviewer found in the tooling, what was fixed, and what is deferred.
---

> Retrospective review of slice 3c per docs/13 and docs/20, run before the
> slice was committed. The slice completes the introspection endpoint
> (`/events`, `/stats`, `/memory`, `/log`, the diagnostics bundle,
> `fjarr-lab introspect`, `make introspect`) and turns the docs/23 memory
> ladder into gates: the leaks-tracer bracketing, the soak, valgrind,
> heaptrack, the netem scenarios, a nightly workflow prepared for a
> self-hosted GPU runner. One reviewer pass on the C++; the two harness
> subagents (opsim soak/netem, lab tests) reported what the tooling found.

## What the new tooling found in the agent

Each row is a defect the ladder or the lab surfaced within hours of
existing; each is fixed and the tool that found it now guards it.

| Found by | Defect | Fix |
|---|---|---|
| lab `/memory` test, then a refcount trace | **webrtcbin 1.28.2 leaks one `GstRtpSession` reference per `get-stats` call** (`_get_data_channel_transport_stats` takes rtpbin's `get-session` and never releases it): an 18 s session left 18 references and ~18 MB behind; the robot went from 44 to 300 MB over an afternoon | the stats sampler reads rtpbin's per-source counters synchronously on the loop; `get-stats` is not used ([docs/23](../23-agent-core-architecture.md#slices-3a-3b-3c-and-their-gates)); reported upstream |
| lab `/memory` test, opsim soak | `census.sources` grew by one per session for ever: the RAII kit only left the census at `cancel()`, so a one-shot timer that fired on its own (the deferred snapshot) stayed counted while its guard sat in a map | the trampoline decrements when a source returns REMOVE; `cancel()` only if it has not |
| opsim soak | closed sessions' snapshot rings never expired: the deferred "closing" snapshot landed after `retire()` and un-retired the ring (76 closed sessions listed, the oldest 20 min old) | a retired ring stays retired; `retire()` takes the pending snapshot immediately |
| heaptrack | one heap allocation per delivered frame in `FrameHub::deliver` (the target list), against the docs/16 budget | the list is a member reused across frames; hub keys interned so `Work` copies no strings |
| opsim soak (200 cycles) | +71 MB with the census flat: snapshot retention was bounded per ring but not in total (200 closed sessions inside the 10-minute window), each snapshot held a parsed JSON tree several times its text plus a 50 KB DOT, and the producer history ramps to 64 entries | snapshots stored as compact text, DOT kept for a ring's 8 most recent entries, at most 8 closed rings, `malloc_trim` at session end, and the soak's baseline taken after a 40-cycle warm-up while the bounded caches fill ([docs/24](../24-pipeline-introspection.md#the-data-model), [docs/23](../23-agent-core-architecture.md#fjarr-opsim-the-operator-simulator)): 200 cycles now end 1.5 MiB *below* the baseline |
| lab `/memory` test | the census went negative by the number of stats samples: the new stats read wrapped two transfer-full element references without the kit's adopting helper | `glib::adopt_element` — and the RAII gate now has a case it did not catch: a kit pointer constructed from a raw pointer (worth a grep in 3c's follow-up) |
| lab `/log` test | six consecutive sessions logged under the same 8-character id: a UUIDv7's first 8 hex digits are a coarse timestamp | `sid8` is the id's last 8 characters everywhere (logs, element names, opsim, the lab, docs/24) |
| valgrind | 146 reports from 4 contexts, all GLib/GIO/duktape-internal | `agent/tests/valgrind/fjarr.supp`, each entry named and library-only |
| opsim netem `lossy`/`bad` | no NACK/RTX/FEC negotiated and a fixed 4 Mbit active tier: at 5 % loss a keyframe rarely survives, under 1.5 Mbit the queue floods | **deferred to slice 6** (adaptive bitrate, loss recovery); the scenarios assert transport-level arrival and record decode health; docs/25's `lossy`/`bad` decode expectations wait for it |

## Reviewer findings: the 3c C++

| # | Severity | Finding | Resolution |
|---|---|---|---|
| 1 | high | the memory census read the tracer's `get-live-objects`, whose result **takes** every live object's reference (it exists for process exit): a `/memory?since=` on a tracer-enabled robot would have freed live elements under a running pipeline | never called; "created and still alive" is created − removed as multisets in a process-wide ledger merged on every read, so reads are idempotent and any number of readers agree |
| 2 | high | `/events` left libsoup's body accumulation on: every frame ever written stayed in memory for the life of the connection | accumulation off; docs/24 says so |
| 3 | high | exceptions from a provider, the tar writer or a malformed JSON field could unwind through libsoup's C handler into the core loop | the handler catches `std::exception` and answers 500 |
| 4 | medium | `since()` reset the tracer window, so a second read (the soak's producer wait, the debug print) lied; several `MemoryCensus` instances re-started tracking | the ledger above; the test runner reads once per test |
| 5 | medium | `FrameHub::targets_` kept the last delivery's sinks (and their `appsrc` refs) alive until the next frame on any key | cleared after delivery |
| 6 | medium | a slow `/events` reader was "ended" but neither dropped nor stopped receiving appends; a libsoup-initiated disconnect never fired `finished` | socket closed, a `closing` flag, `disconnected` handled |
| — | low | `Last-Event-ID` replayed one ring only; per-frame `HubKey` string copies beyond SSO; gzip at level 6 on the loop; the leaks listener ran after gtest's printer; docs drift (`fjarr-agent --memcheck`, `gst.supp`, the bundle's members, the checkpoint token) | all fixed: replay across rings by timestamp, interned keys, level 1, listener before the printer, docs corrected |

Checked and found fine: the endpoint and every provider run on the core
loop; `SseClient` lifetime against libsoup 3.6.6's `finished`/`disconnected`
semantics; SSE framing per docs/24; the ustar header (checksum, octal fields,
prefix split, padding, end marker) and the GZlib loop; the diagnostics fetch
from a loop-less daemon; the log ring's bounds and thread safety; the tracer
signal names and structure fields on 1.28.2; `fjarr.supp` patterns never
match a fjarr frame; the nightly workflow against docs/12 and docs/15.

## The gate

- docs/24 acceptance minus the viewer, through curl, `fjarr-lab introspect`
  and the lab tests: `/pipelines`, `/stats`, `/memory` checkpoint diff,
  `/log`, `/events` (a `select-tracks` shows as a snapshot event with the
  valve in its body), `/diagnostics.tar.gz` parsed member by member.
- docs/15 memory rows: ASan, TSan, the leaks bracketing (32 tests, and the
  deliberate-leak self-check fails as it must), valgrind memcheck clean,
  heaptrack with no fjarr allocator on the frame path.
- The soak: 20 cycles per commit and 200 at the gate — census identical to
  the post-warm-up checkpoint (`sources` unchanged), RSS −1.5 MiB over the
  160 cycles after the 40-cycle warm-up against the 5 MiB budget (before
  the fixes above: +19 sources, +71 MB per 200 cycles and, over an
  afternoon of mixed scenarios, +250 MB).
- Every earlier gate still green: 8 opsim scenarios twice on a quiet robot,
  the full lab suite, netem `lossy` and `bad`.

## Deferred

- Loss recovery (NACK/RTX, FEC) and adaptive bitrate: slice 6; until then
  `lossy` asserts arrival, not decode, and `bad` is out of the nightly (it
  fails on its control channel at 15 % loss without them).
- The `toggle-silent` check saw 7–11 frames decode up to 800 ms after a
  disable in 2 of 3 runs while netem, the soak and the lab shared one robot;
  it did not reproduce on a quiet robot (twice). Watch it in CI; if it
  returns, the suspect is send-side buffering after the valve.
- The final local verification ran on the software encoder: after a day of
  builds and sanitizer runs the host's package power limit had throttled
  the iGPU and its clock stayed at the 800 MHz floor, so VA-API sessions
  streamed 10–15 fps and the lab's hot-plug continuity budget failed —
  identically with the committed 3b agent, which located it in the machine
  and not the slice (on the software encoder both builds measure a gap of
  3–4 on this host today against CI's ≤ 2 for the same committed code, so
  the lab browser's side moved too; CI arbitrates that test). The sim's unthrottled `glxgears` (3.5 cores, always)
  was removed as a contributor; docs/12 has the diagnosis. The VA-API path
  is re-measured when the GPU clocks normally (or on the GPU runner).
- The nightly's VA-API measurement waits for the self-hosted runner
  ([docs/12](../12-development-environment.md#nightly-ci-and-the-self-hosted-gpu-runner)).
- Upstream: the webrtcbin stats leak; the workaround stays until the
  baseline carries a fix.
