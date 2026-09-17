---
title: "Slice 3 Planning Review"
description: Two-lens review of the slice-3 planning set (docs/23–26, ADR-0019–0021, the webrtcbin spike) before implementation — what an implementer would have had to invent, what the documents contradicted, what was fixed, and what remains for the maintainer.
---

> Review of the planning documents themselves, per docs/13 ("specs at
> `review` before implementation starts"). Two reviewers: an
> implementer lens ("could two engineers build the same thing from
> these documents?") and a product/delivery lens (scope, gate quality,
> ordering, missing artifacts, risks). Findings were verified and fixed
> on `main` in the same pass; this page records the outcome.

## Verdict before the pass

No. The architecture was complete on structure but the two artifacts
the gate depended on (`fjarr.test`, `fjarr-opsim`) had no specification,
a dozen wire-adjacent details were left to invention, several documents
contradicted each other or the code, and the environment (compose, CI,
Makefile, the server binary) could not run the gate. One finding was a
real slice-1 bug: `fjarr-server` mounted `Config::default()`, which
rejects every agent and operator, so the sidecar image could never have
brokered a session.

## Fixed in this pass

| Area | Gap | Resolution |
|---|---|---|
| Scope | one slice bundling four reviewable deliverables | split into **3a** browser lab, **3b** agent core + `fjarr.test` + `fjarr-opsim` + minimal introspection + minimal demo wiring, **3c** introspection completeness + memory ladder ([docs/23](../23-agent-core-architecture.md#slices-3a-3b-3c-and-their-gates), docs/17, docs/24, docs/25) |
| `fjarr.test` | no spec | [docs/06](../06-capabilities.md#fjarrtest--the-built-in-test-capability-slice-3): tracks, sources, messages (`echo`, `hotplug`, `silence`, `drive`, `deadman`), input-bearing semantics, config, acceptance; shipped in the product package as the install smoke test |
| `fjarr-opsim` | no spec | [docs/23](../23-agent-core-architecture.md#fjarr-opsim-the-operator-simulator): CLI, self-minted HS256 grant, eleven named scenarios with assertions and expected end states, output and exit codes |
| Track control | `select-tracks` ownership undefined; `bandwidth-stats` `frames`/`dropped` had no source; unknown `track_id` behaviour undefined | [docs/08 track control](../08-protocol.md#track-control): served by the core for every track-owning capability under that capability's `cap`; field sources; `payload-invalid` with nothing applied |
| Agent TURN | agents had no way to get TURN credentials without a static secret | `session-request.turn`, minted per session like the operator's (docs/08, schema, fixture, Rust, TS) |
| Dev auth | three different shapes across docs, Rust and fixtures | one shape `{scheme:"dev-token", robot_id, dev_token}`, one env var name `FJARR_DEV_DEVICE_TOKEN`, a fixture for it; `agent_info` fields named |
| Server binary | `Config::default()` (reject all) | `Config::from_env()`; compose passes the dev secrets from `.env` |
| Frame stamp | 80+ bits declared in 32 blocks | 96-bit layout (sync, counter, 48-bit ms timestamp, checksum), block width from frame width, painted by a core pad probe ([docs/25](../25-browser-lab.md#frame-stamp-the-latency-harnesss-oracle)) |
| Encoders | "refuses software" vs "CI runs software" | explicit `encoder = auto \| vaapi \| software`; `software` = `openh264enc` chosen on purpose; never a silent fallback |
| Config | keys named across docs but absent from the schema | complete `fjarr.toml` in docs/23 with `[media]`, `[introspect]`, per-track `required`, env override rule |
| Media numbers | GOP, tier bitrates, queue/appsrc properties, PTS rebase, realtime drop threshold, pt allocation unspecified | constants table in docs/23 |
| Timers | scattered | one table in docs/23 |
| Milestones | two different lists | one list of milestones plus post-connect events |
| Introspection JSON | a test oracle with no schema, no addressing grammar | `protocol/schemas/introspect.schema.json` under the conformance gate, fixtures, element-naming grammar, SSE format, checkpoint token, port-in-use behaviour |
| docs/09 vs headers | `SessionId` type, `DetachReason` vocabulary, `send_binary` return, missing `Stream` class, no `release_all_input` hook or `input_bearing` flag, `SessionEvent` fields | docs/09 updated (headers follow in 3b) |
| ICE-restart fallback | claimed capability state survives; lease carry-over unaddressed | corrected: a new session; the operator-keyed, fail-open lease carries across the gap |
| Fault menu | "SIGSTOP → media plane restarts" impossible in one process | two rows: pipeline error → plane rebuild; process hang → systemd watchdog |
| Wire tap | mentioned, unspecified, and a leak risk | event shape, opt-in only, never enabled by `@fjarr/react` (docs/21, docs/10) |
| Demo topology | which binary runs in `demo-robot` | `demo-robot` embeds `fjarr::TestCapability` through the public API; compose env fixed (`ws://…/ws`, robot id, dev token, `NET_ADMIN`) |
| DataChannel parameters | per-class options, creation order, SCTP limit | stated in docs/23 |
| CI/presets | no ASan test preset; LSan suppressions unwritten | `asan` test preset with `LSAN_OPTIONS`; `agent/tests/lsan.supp` |
| Statuses | docs 23–25 `draft` while implementation was about to start | promoted to `review`; docs/26 stays `draft` (M3 scope) |

## Deferred, with owners

- Makefile targets, compose `browser` service, CI jobs and the nightly
  workflow named in docs/12, 15, 23–25 are created by the slice that
  needs them (3a: lab; 3b: agent tests; 3c: memory ladder) — the docs
  describe the end state.
- Chromium-answerer verification of the spike's Q1/Q3/Q6 is the half-day
  spike inside 3a.
- `pnpm-workspace.yaml` entry for `@fjarr/e2e`, the Playwright image tag,
  the systemd unit file: 3a/3b implementation details.

## Open for the maintainer

1. **M3 load.** M3 now carries the desktop MVP, the presentation-mode
   spike, audio, packaging, the driver catalog and the design-partner
   demo. Proposal: an **M2.5 "Packaging & install"** milestone (apt
   repository, `fjarr-agent` package, `setup`, `--check`, catalog format)
   so M3 receives packaging rather than builds it.
2. **GStreamer baseline.** 1.26 may fix ICE restart and the
   `direction=inactive` stall (open question #21). Bump now (an ADR-0002
   revision, Ubuntu 25.04/26.04) or at the ADR-0007 spike in slice 6?
3. **systemd.** Written as optional (`watchdog_secs = 0` → off, `sd_notify`
   when available). Require it on shipped robots instead?
4. Adopted without a separate decision, flag if you disagree: the
   3a/3b/3c split; the viewer built once in slice 5 and served from the
   agent as a data file; TSan and the `leaks` tracer as trends in 3b and
   gates in 3c; `fjarr.test` in the product package as the install smoke
   test; the introspection endpoint on loopback TCP by default (Unix
   socket optional).
