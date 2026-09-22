---
title: "Slice 6b Review"
description: Retrospective review of passthrough — a camera's own H.264 parsed and packetized with no encoder on the robot, its substream as the thumbnail tier, what GStreamer's parser and ghost pads cost to get right, and where the design moved from the plan.
---

> Retrospective review of slice 6b per docs/13 and docs/20, run before the
> slice was committed. The slice closes M1's media work: a camera with an
> on-board encoder can now stream through Fjarr without the robot
> encoding anything, and the camera's own substream becomes the track's
> lower tier, so slice 6a's rate control still has somewhere to move a
> viewer. One reviewer pass on the C++ and the fixtures, plus what the
> lab and the leaks gate found.

## What the harness found

| Found by | Defect | Fix |
|---|---|---|
| lab `rtsp` test, first passthrough run | every connection failed with "Delayed linking failed" then "Internal data stream error": `gst_parse_bin_from_description(…, TRUE)` ghosts the depayloader's *sink* pad, unlinked at parse time because rtspsrc's pad is delayed — and a ghosted pad counts as linked, so rtspsrc's delayed link had nowhere to go | the passthrough chain parses with `FALSE` and ghosts the named parser's src pad by hand (the same trap `make_late_ghost_bin` documents for decodebin) |
| the same test, next run | the source bin had no `src` pad at all — nothing is ghosted with `FALSE` | the parser is named per output (`fjarr-rtsp-parse-src`) and ghosted through both bin levels |
| the same test, next run | rtspsrc failed the whole pipeline again: the substream is connected as soon as the producer builds, but the thumbnail tier may never start, and an unlinked ghost pad returns not-linked | one `tee allow-not-linked=true` per passthrough output, as the raw path's tee already had |
| the leaks gate, first full unit run | the outer bin, both chains and their ghost pads were still alive after `RtspSource` tests: wrapping the chains in an outer bin with the RAII sink helper handed the caller a *second* reference, while docs/09 says `create_bin()` returns a floating one | the outer bin stays floating; the caller sinks it, as every other source does |
| unit test over the media plane | `adaptive()` answered `true` for a passthrough track before its producer existed (it asked the producer, which was null) | the track's source shape answers it until the producer does |
| the RAII gate | the outer bin's error path used a raw `gst_object_unref` | the RAII kit owns it while it is built and `release()`s it to the caller |
| the full browser suite | the substream test counted frames from the moment it *asked* for the thumbnail tier; the hub starts a subscriber at a keyframe and the camera's GOP is 2 s, so on a loaded machine only 2 frames fell inside the window | it waits for the browser to report the substream's 640×360 and counts from there — the switch is observed, not assumed |
| the full browser suite | slice 6a's rate-limited test timed out on "not promoted" when its session had actually ended on the impaired link | the helper now fails with "the agent no longer has session …", so a dead session never reads as a rate-control defect |

## Reviewer findings

| # | Severity | Finding | Resolution |
|---|---|---|---|
| 1 | medium | the plan put `passthrough = true` beside `source` in the track config, but it changes how the *source's* pipeline is built (depayload and parse instead of decode) and would have to be forwarded into the source anyway | it is a source param (`rtsp` and `gst` take it; a registered type declares `video/x-h264` caps instead) and the core decides from the declared caps alone, so nothing else in the media plane knows about the setting — docs/06 and docs/23 amended |
| 2 | medium | the planned gate measured "CPU below a quarter of the transcoding path's", which is a noisy number on a shared machine and proves the wrong thing | the gate asserts the *structure* — the producer's pipeline contains no encoder and no decoder, only depayload and parse, and `/stats` reports no encoder target — which is what "costs no encode" means and cannot pass by luck |
| 3 | medium | `--check` refusing passthrough on a raw source was in the plan, but with passthrough as a source param the case cannot arise: `rtsp` and `gst` declare elementary caps when asked, and a description that ends raw is simply not passthrough | dropped from the gate; `--probe-source` reports the codec, profile and level instead, which is the question an integrator actually has |
| 4 | low | a passthrough producer reported `kbps` and logged a bitrate it never applies | `/stats` omits the encoder target for a passthrough tier and reports `passthrough: true`; the log line names the camera's stream instead |

## Measured

| Case | Result |
|---|---|
| The demo's RTSP track | pipeline factories: `rtspsrc rtph264depay h264parse` twice (main and substream), two tees, `h264parse capsfilter appsink` — no encoder, no decoder; 64 frames presented in 2.5 s; `/stats`: `passthrough: true`, no encoder target, 0 keyframe requests |
| `--probe-source` on the simulator | `H.264 profile=constrained-baseline level=3.1`, 27 fps, passthrough; the same source without the flag negotiates `video/x-raw NV12 1280×720` |
| Tier switch | `select-tracks` to `thumbnail` gives the browser 640×360 within seconds and back to 1280 after, with no reconnect |

## Gate (docs/23 slice 6b)

1–4. `tests/stack/passthrough.spec.ts` (two tests) and `test_passthrough.cpp` (the source's outputs and descriptions, the substream-without-passthrough config error, tier possibility and `adaptive` over the media plane) — green.
5. Every earlier gate green; see the commit.

## Deferred

- The substream is connected as soon as the producer builds, even when only the active tier is demanded: one extra camera connection at the camera's substream bitrate. Making it lazy needs the source contract to build one output at a time, which is a docs/09 change M2.5 can carry if a customer's camera objects.
- H.265 passthrough: the core's caps test accepts `video/x-h26*`, but the consumer's payloader and codec preferences are H.264 only, so an H.265 source would negotiate nothing. Browsers are only now taking H.265; it belongs with the codec negotiation work in M3.
- A passthrough track's keyframe interval is the camera's, so a viewer joining a camera with a 10 s GOP waits for its next keyframe. docs/26's install guidance should tell integrators to set 1–2 s on the camera.
- Audio from an RTSP camera is still dropped (`select-stream` takes video only), as in slice 4.
