---
title: "Slice 4 Review"
description: Retrospective review of fjarr.camera on real sources — the SourceFactory seam, the v4l2 and rtsp types, hot-plug by renegotiation, per-track failure that never reaches the plane rebuild, what the reviewer and the lab found, what was fixed, and what is deferred.
---

> Retrospective review of slice 4 per docs/13 and docs/20, run before the
> slice was committed. The slice puts the first real capability on real
> sources: `fjarr.camera` reads its tracks from config, resolves each
> `source = …` through the agent's registry (built-in `gst`/`test`/`v4l2`/
> `rtsp` and types the embedding application registered), and the demo
> robot streams a pattern, the lab's RTSP simulator and — with an opt-in
> override — the host webcam. One reviewer pass on the C++ and the harness,
> plus what the lab and the unit tests found while it was built.

## What the harness found

| Found by | Defect | Fix |
|---|---|---|
| lab `rtsp` test, second run | the second start of an rtsp producer streamed 3 fps: after the tier grace set the pipeline to NULL, decodebin rebuilt its pads and the ghost pad kept its stale target | a shared late-ghost helper for decodebin-based bins re-targets when the current target's pad is gone ([docs/23](../23-agent-core-architecture.md#video-sources-one-contract-three-ways-to-provide-one)) |
| lab `/sources` test | configured sources were invisible until a session registered their tracks, so the docs/26 "missing device, with the reason" behaviour had nothing to show a doctor or a dashboard | `Capability::configured_sources()` and `VideoSource::unavailable_reason()` (docs/09); `/sources` lists configured sources first, the plane adds caps and tiers once a track is registered |
| lab, three viewers | presented-frame counts of 3–16 per two seconds with two VA-API producers on this host's iGPU (the 3c throttle state); identical code streams 30 fps to every viewer on the software encoder | not a code defect: two hardware encodes at the floor clock; the software path (CI's) is the gate, the GPU runner measures VA-API |

## Reviewer findings

| # | Severity | Finding | Resolution |
|---|---|---|---|
| 1 | high | only `format = "mjpeg"` v4l2 sources could stream: `yuyv` and `auto`+size ended the description in a bare caps string (does not parse), and `auto` without a size let the parser ghost decodebin's *internal* typefind pad | every form emits an explicit `capsfilter`; `auto` without a size decodes through the same late-ghost helper as rtsp; a test builds a bin with a `src` pad for every form |
| 2 | high | unplugging a camera did not remove its track (the hot-plug callback re-offered the full set and the core skips ids already offered), while the producer's bus error walked the restart ladder into a plane rebuild that closed every session | the callback re-offers only what is available (the core's diff removes the rest, valve first); a producer error from inside the source bin is the track's failure: slow retry (1 s → 30 s), reported as the track's reason, never a plane rebuild ([docs/23](../23-agent-core-architecture.md#media-plane-recovery)); loop test with `identity error-after` inside the source bin |
| 3 | high | an unreachable RTSP camera was offered, failed asynchronously and took the agent down the same ladder (every session closed, exit 2 within minutes) | same classification: its track degrades with the bus error as reason |
| 4 | high | the camera lab tests read stamps that are painted only for `fjarr.test` tracks | presented frames (readable or not) are what the camera tests count; watchers start once the video element exists |
| 5 | medium | an RTSP camera announcing audio first would give decodebin's single sink to the audio stream | `rtspsrc::select-stream` accepts video only (documented: video only in slice 4) |
| 6 | medium | `--check` skipped the schema validation the agent runs and could abort on a track without `source` | `fjarr::validate_json_schema` (public, the same the agent uses), every failure printed, never an abort |
| 7 | medium | `configure()` cleared state before it could throw | built aside, swapped in at the end |
| 8 | medium | the hot-plug trampoline could let an exception unwind through GLib | caught and logged, as the kit's trampolines do |
| 9 | medium | the demo's webcam default and the override could not exercise real hot-plug (a container's `devices:` are static) and the gate text implied they could | docs/12 and the docs/23 gate say bare metal; `FJARR_DEMO_WEBCAM` documented |
| 10 | medium | docs/06 put `output` inside the source table; the code reads it beside `source` | docs/06 corrected and extended with `required`, `v4l2` and `rtsp` forms |
| 11 | medium | the 500 ms re-enable measurement included Playwright's polling backoff | polled at 20 ms on the freshly mounted video |
| — | low | an empty `device` indexed a string; the rtsp `pad-added` connection is per source, not per bin (latent, the plane serializes producers); GIO polls a missing by-id directory (~4 s) | `minLength: 1` and a guard; comments; docs/23 note |

Checked and found fine: the RAII gate over the new code; the targetless
ghost linked before its target exists (frames flow once `pad-added` sets
it); GIO monitors created with the core context as thread-default (the
unit test asserts the callback runs on the loop); `session_detached`
ordering against the hot-plug callback; the plane's registration
refcounts through `update_tracks`; docs/09 against the headers; the
dev-only packages in docs/14; `rtsp-sim` started by CI through
`depends_on`.

## The gate (docs/23, slice 4)

1. Three lab pages watch `pattern` and `rtsp` of the demo robot at once
   (one producer per track, three hub subscribers each); a toggle takes
   effect without renegotiation and re-enabling shows a frame well under
   500 ms (the hub's ring catch-up).
2. The RTSP track streams from the simulator in CI; the `v4l2` track with
   no device is `missing` with `no such device: …` in `/sources` and
   absent from the manifest; arrival and departure re-offer every session
   (unit test on a by-id directory, with a recording session context); a
   source that errors degrades its own track only (loop test).
3. `--probe-source` reports caps, memory type and fps for `test`, a
   description, the RTSP simulator (26.5 fps decoded) and a missing
   `v4l2` device with its reason; `--check` prints a row per configured
   source and fails on a `required` one that is missing.
4. Every 3b/3c gate green on the software path, the soak included.

## Deferred

- Elementary-stream passthrough for cameras that encode on board (an
  `rtsp` source decodes today), loss recovery and adaptive bitrate:
  slice 6.
- Real hot-plug through the compose override (Docker cannot hot-plug a
  `devices:` node): bare metal and the GPU runner.
- Audio outputs of a source (`fjarr.audio`).
