---
title: "Slice 5a Review"
description: Retrospective review of the protocol half of the demo wiring — blob frames on both tiers, the fjarr.introspect capability, the pipeline feeds and the viewer component, the demo's roles; what the unit tests and the lab found, what was fixed, and what is deferred to 5b.
---

> Retrospective review of slice 5a per docs/13 and docs/20, run before the
> slice was committed. The slice closes the four planning gaps of
> 2026-09-21 on the protocol side: docs/08 blob frames replace the file
> frame and get a receive half on both tiers; `fjarr.introspect` exposes the
> snapshot rings over the session; one `PipelineFeed` interface with a
> session and an HTTP implementation carries the same viewer to the
> dashboard and (in 5b) to the robot's endpoint; the demo backend stops
> granting everything to everyone. One reviewer pass on the C++ and the
> TypeScript, plus what the tests found while it was built.

## What the harness found

| Found by | Defect | Fix |
|---|---|---|
| blob pump unit test | none in the pump — the test's arithmetic assumed three frames fit under a 200-byte fake watermark; two 101-byte frames already exceed it | the test drains until completion and counts 2 + 2 + 1 frames |
| React hook test | `usePipelineFeed` handed two consumers two feeds: the initializer counted references, but React runs every initializer before any effect, so the second consumer saw a zero-referenced entry and replaced it | the initializer only ensures an entry exists; the effect owns the reference (StrictMode double-mounts effects, which this also survives) |
| lab `introspect` spec | producers were expected in the list before anything streamed; the request's error code is on the error object, not in its message | test-side both times — the agent lists only pipelines that have snapshots, and `FjarrError.code` is the contract |
| lab, dashboard test | none: the Diagnostics tab rendered the session graph with d3-graphviz in real Chromium on the first run (SVG nodes counted), and the operator role was refused with `capability-denied` | — |

## Reviewer findings

| # | Severity | Finding | Resolution |
|---|---|---|---|
| 1 | high | the first blob chunk could leave *before* the referencing envelope: `send_blob()` pumped synchronously, and a capability builds the reference before it sends the event | the first pump is posted to the next loop turn (docs/23 notes); the recording context in the unit tests completes blobs synchronously, which also proved the in-flight bookkeeping tolerates that order |
| 2 | high | newest-wins by *cancelling* the in-flight blob would leave the subscriber a dangling reference that times out 30 s later | the capability holds the newest snapshot per pipeline while one is pumping and sends it on completion; the pump never withdraws bytes; the unit test drives the pump by hand and sees seq 1 then seq 4, never 2 or 3 |
| 3 | high | on the web, keeping the blob store on the `BulkSender` would lose chunks that arrive before `session.bulk(cap)` is called, because the session hands out a fresh sender per call | the receiver lives on the `ChannelSet` per capability, created by the first `bulk(cap)`, closed with the channel; a test sends the chunks first and resolves the reference at once |
| 4 | medium | docs/08 answered every unattached capability with `capability-unknown`, while docs/06 promised `capability-denied` to an operator without the grant | the router asks the registry: registered-but-ungranted is denied, unheard-of is unknown (docs/08 amended; the lab asserts both codes) |
| 5 | medium | the docs/23 plan put the pending store in the router; the router has nowhere sensible to hand a completed-but-unclaimed blob to | `BlobAssembler` *is* the bounded pending store, per capability; the router parses, checks and counts (docs/23 amended) |
| 6 | medium | the graph component's default renderer pulls several MB of wasm; on the main entry every host would carry it | `<PipelineGraph>` is the `@fjarr/react/pipelines` entry with d3-graphviz as an optional peer dependency; the hooks stay on the main entry; `renderDot` injects any renderer (the tests use a fake) |
| 7 | low | the demo dashboard read the role once at load; changing it while connected did nothing visible | the picker says "applies to the next connect"; the grant callback reads the current role each time |
| 8 | low | the `SnapshotStore` had one listener slot, taken by the endpoint | a listener list; the endpoint and the capability read the same ring |

## Gate (docs/23 slice 5a)

1. docs/06 acceptance in the lab: a developer-role page watches the session graph's valve follow `select-tracks` and the hot-plug branch appear, with `txt` inline and `json`/`dot` as blob references over `fjarr:bulk:fjarr.introspect`; an operator-role page gets `capability-denied` — `tests/stack/introspect.spec.ts`, three tests green against the demo robot.
2. The feed contract suite passes against both feeds — `web/packages/core/test/pipelines.test.ts`: the session feed over the mock agent, the HTTP feed over a local Node server with the token path, same list, same newest-wins under a burst, same bytes by sequence, recovery after a transport drop.
3. Blob failure modes on both tiers — C++: bad header, gap, oversize, eviction, expiry, a stalled channel resuming on drain, cancel, a session ending mid-blob (`test_blob.cpp`, `test_introspect_capability.cpp`); TypeScript: both arrival orders, bad header, timeout, close mid-blob, bounds, length mismatch, `sendBlob` chunking after the envelope (`blob.test.ts`).
4. `fjarr-lab introspect` runs on the HTTP feed when `E2E_INTROSPECT_HTTP` is set.
5. Every earlier gate green: agent unit tests, ASan, TSan, the RAII and leaks gates, opsim, the 20-cycle soak, the full lab run — see the commit.

## Deferred

- The served viewer, `introspect.viewer_dir`, the demo's token path and `make introspect` opening a browser: slice 5b.
- Agent-side blob *receive* is exercised only by unit tests (no capability receives blobs yet); the remote terminal (M2) and file transfer (M4) are its first users.
- `pipelines/subscribe` with a `pipeline_id` filter is implemented and unit-tested but the dashboard always subscribes to `*`.
- `make fmt`'s C++ step has never been a gate: the repo has no `.clang-format`, so `clang-format-21` re-indents every hand-formatted file to LLVM defaults (found when it touched 100 files in this slice; reverted). Deciding a style and adding the file is a separate change — until then the C++ is formatted by hand, as it has been since M1.
- The dashboard lab spec asserted a single `test-pattern` track since slice 3b; CI skips it (no Vite dashboard in the CI stack) so the slice-4 camera grant went unnoticed locally until this run. Now asserts the operator role's manifest.
