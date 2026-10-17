---
title: "ADR 0041: Module E Captures Monitors as DMA-BUF, Without a Copy Into VA"
description: Backend module E reads mutter's monitor streams as DMA-BUF and hands them to vapostproc and vah264enc without a copy, keeping shared memory as the fallback, because mutter 50.1 records nothing into shared-memory monitor streams while a fullscreen window is scanned out directly, and the operator saw a frozen picture behind green counters.
---

- **Status**: accepted
- **Date**: 2026-10-17
- **Supersedes**: — (replaces [ADR-0006](0006-desktop-backend-selection.md)'s
  note that DMA-BUF with mutter fails with "target not found", which no
  longer reproduces)

## Context

Since 232dbbf (2026-10-03, the local cursor) module E records GNOME monitors in
mutter's **metadata** cursor mode ([docs/22](../22-remote-desktop-client.md#cursor-strategy)):
the cursor travels beside the video, and mutter keeps the hardware cursor
plane. Its stream reader asks for shared memory (SHM: `MemFd` or `MemPtr`) and
copies every frame ([docs/23](../23-agent-core-architecture.md#desktop-helper-protocol)).

The mini-PC's nightly `desktop-control` failed input-to-photon 0/10 from 10-05,
and passed on 10-17 on stale samples. On 2026-10-17 the cause was found on the
machine (Ryzen 7 5700U, amdgpu, mutter 50.1, PipeWire 1.6.2, GStreamer 1.28.2,
Mesa 26.0.8), with the KMS plane state, the CRTC's CRC, mutter's own debug log
and independent capture clients ([spikes/capture-scanout](../../spikes/capture-scanout/README.md)):

- With a hardware cursor and nothing of the shell on screen, mutter 50.1 scans
  a fullscreen window that renders through dma-buf (GTK 4 with Vulkan or GL,
  any EGL or Vulkan app, a fullscreen browser or video player) **directly
  out** to a monitor that has a KMS CRTC, a ghost screen included.
- While a view is scanned out, mutter records **no frame into SHM monitor
  streams**: the stage is not painted, and `before_stage_painted` in
  `meta-screen-cast-monitor-stream-src.c` returns early for streams that are
  not DMA-BUF. A self-blinking fullscreen window changed the screen 24 times in
  12.4 s by the CRC; an SHM capture got 0 buffers. A DMA-BUF stream on the
  same monitor recorded 72 frames in 36 s of scanout, every blink in the right
  colour, while the agent's SHM stream recorded none.
- The reader re-sends its last frame on a still screen, so the operator saw a
  **frozen picture** while the frame counters, `desktop-see` and the
  still-screen check stayed green. Input still reached the robot: the operator
  acted on a stale picture.
- A capture that **starts** during scanout gets no first frame over SHM.
  Module E's fallback to the embedded cursor does not rescue it while the
  pointer is still, because mutter 50.1 applies the hardware-cursor inhibitor
  only at the next cursor update, nor when the application hides the pointer.
  After three restarts the capture is left running without a frame until
  scanout ends.
- Notification banners, menus and the overview stop scanout while they are on
  screen, which made the failure look intermittent. CI's headless mutter has
  no KMS CRTC and cannot show it: only real hardware does.

Upstream mutter has fixed this: merge request !5276 ("Fix direct scanout
handling without dma-buf", closing issue #3903) records SHM streams during
scanout, and shipped in mutter 50.5 (2026-09-15) and 51.0. Ubuntu 26.04 still
ships 50.1 without it (50.1-0ubuntu2.4 in `resolute-updates` on 2026-10-17;
[open question #49](../18-open-questions.md)), and robots run the mutter they
have. The freeze must be fixed in the agent.

## Options considered

**(a) DMA-BUF into VA, without a copy, with SHM as the fallback.** Module E's
reader negotiates DMA-BUF with modifiers the encoder can import, and the
frames go to `vapostproc` and `vah264enc` on the GPU. Measured at mutter level
on the mini-PC, with stand-ins for the reader (a raw `pw_stream` consumer and
`pipewiresrc`):

- A DMA-BUF stream started during an ongoing, still scanout with the pointer
  hidden got its first frame 6 to 18 ms after it started streaming, 10 starts
  of 10, including a second concurrent stream and a stream started alone
  after the first ended. SHM controls in the same runs got none.
- It followed every later change: 33 blinks of 33, 5 to 26 ms after the
  window drew them.
- `SPA_META_Cursor` arrives as with SHM: positions within 15 ms of each of 118
  pointer motions, the bitmap and hotspot after each shape change, and
  cursor-only buffers (an empty chunk flagged corrupted). Metadata mode and the
  local cursor stay.
- `pipewiresrc ! vapostproc ! vah264enc` negotiated
  `drm-format=AR24:0x0200000000000901` (XR24 with the same modifier works too)
  and imported every frame as a VA surface, with no download. That modifier is
  the only RGB one `vapostproc` lists, and mutter's list (`0x…401a01`,
  `401901`, `401601`, `0a01`, `0901`, LINEAR) contains it: offered all of its
  own, mutter fixates `0x…401a01`, which VA does not list.
- At 1920×1080 and 30 fps this cost 5.6 to 6.2 % of one core, against 27.5 to
  28.6 % for a stand-in of today's chain (copy, `videoconvert` to I420,
  `vapostproc`), and 13 to 14 % for SHM uploaded by `vapostproc`. gnome-shell
  rose 0.8 to 1.7 points over its baseline, against 3.7 to 4.5 for today's
  chain.
- A CPU reader cannot take mutter's DMA-BUF instead: tiled buffers refuse
  `mmap` (EPERM), and a LINEAR one cost 48 ms per frame to copy, a whole core
  at about 20 fps. A GL download costs 17 % (NV12) to 30 % (BGRx).

Cons: real work in the reader and the media plane; untested inside the agent
itself; DMA-BUF and modifier support differ per GPU and driver; CI cannot run
the path (no GPU).

**(b) The embedded cursor on monitors with a KMS CRTC** (the behaviour before
232dbbf). A small change; one run with the pointer moved after the stream
started passed 10/10 (p50 150, p95 200 ms). But it gives up the local cursor on
those monitors, does nothing for a capture started during scanout with a still
pointer (0 frames after three restarts, measured) or for a pointer the
application hides (0 frames in 8 s of scanout, measured), and by mutter's
source not when the pointer is on another monitor either (not measured).

**(c) `MUTTER_DEBUG_PAINT=disable-direct-scanout`, set by `setup desktop`** for
the desktop account's gnome-shell. No agent change, covers every monitor and
the hidden pointer, keeps the local cursor; passed 10/10 twice (p50 149/150,
p95 200/201 ms), and a blink run gave 54 frames for 41 changes. But it is a
mutter debug variable, not an interface; it takes direct scanout from the
robot's own fullscreen applications (more GPU work, power and display
latency on the robot); it applies at the next login only; and it is
GNOME-specific setup state to keep and undo.

**(d) A software cursor** (`MUTTER_DEBUG_DISABLE_HW_CURSORS=1`). Passed 10/10
twice with the pointer visible, failed 0/10 as soon as the window hid it, and
is a debug variable too.

**Status quo.** A frozen picture behind green counters, on exactly the
fullscreen HMIs robots run. Not acceptable.

**Upstream fix** (complementary, not an option on its own): already done in
mutter 50.5 and 51.0 (!5276), which records SHM streams during scanout. It
fixes every SHM consumer, but only where that mutter runs: Ubuntu 26.04 needs
a stable release update (SRU) backporting it, requested through Launchpad
(#49), and robots on 26.04 keep 50.1 until it lands. It does not touch the CPU
cost of the SHM path.

## Decision

**(a)**, decided by Erik on 2026-10-17. Module E captures monitors that have a
KMS CRTC as DMA-BUF, with modifiers the encoder can import, and hands them to
`vapostproc` and `vah264enc` without a copy. Today's SHM path stays as the
fallback. Metadata mode and the local cursor stay as they are. It fixes the
freeze, fixes a capture started during scanout without needing pointer motion,
covers the hidden pointer, keeps the robot's own direct scanout, and removes a
copy and a CPU conversion per frame. The Launchpad SRU request for 26.04 goes in beside it.

## Consequences

- **The modifiers come from the encoder.** The media plane gives each capture
  its **import list**, the DMA-BUF formats and modifiers the active encoder
  family imports (`vapostproc`'s sink caps for `vaapi`), through
  `CaptureOptions` ([docs/09](../09-interfaces.md#the-desktop-backend-interface-wayland-first-shape)).
  The reader offers mutter the intersection of that list with what mutter can
  produce, never mutter's whole list, since mutter's first choice may be one VA
  cannot import (it is, on the mini-PC). How it decides and what it logs is in
  [docs/23](../23-agent-core-architecture.md#desktop-capture-memory).
- **The frames stay on the GPU.** The reader never maps a DMA-BUF. The
  producer has a device-memory head: no `videoconvert` and no I420 caps, the
  tiers' filters on device memory, `vapostproc` doing the conversion to NV12
  and the thumbnail tier's scaling. This is what docs/23 already promised for
  device-memory sources; the desktop is the first such source.
- **A buffer is held while GStreamer holds its frame.** The reader returns a
  DMA-BUF buffer to mutter only when nothing in the pipeline references it, and
  holds the newest one, so the **keepalive re-sends a GPU buffer** that mutter
  cannot overwrite. A re-sent frame is marked, so introspection counts **new
  frames and repeats** apart ([docs/24](../24-pipeline-introspection.md#capture-frame-counts)).
- **SHM stays the fallback**, chosen per capture and logged with its reason:
  virtual monitors (mutter's `Meta-` monitors, `RecordVirtual` and headless
  mutter alike, which have no KMS CRTC and cannot be scanned out, so SHM sees
  every frame there; their DMA-BUF path was not measured); no modifier in
  common; mutter offering no DMA-BUF (no GPU, as in CI); an encoder family
  that imports none (`software` today, and `nvcodec` until it has a path); and
  `helper.capture_memory = "shm"`. A physical monitor on SHM is still exposed
  to the freeze, and its log line says so. A GPU download for the software
  encoder (vapostproc to system memory, or GL at 17 to 30 % of a core) is
  left until a GNOME robot without VA asks for it.
- **Explicit sync later.** mutter advertises the `SyncTimeline` meta for
  DMA-BUF streams; `pipewiresrc` 1.6.2 does not support it, and every run
  above used implicit fences. The reader does not announce it in this change:
  on amdgpu the kernel's implicit fences gave the right frame for every blink
  in the 3-second runs and for every fresh buffer, and Intel's drivers keep
  implicit sync for dma-bufs too (not measured). The 30 Hz check (0 torn
  frames in about 2,960 decoded DMA-BUF frames) has little power to find a
  race, so this is a measured "works", not a proof. When adopted (NVIDIA, or a
  torn or stale frame seen), the meta is announced together with a `Buffers`
  param whose `metaType` names it, never alone (alone, it gave 1-block buffers
  and 18 gnome-shell assertion failures); acquire points are waited on only
  for buffers with pixels; and the release point is signalled only when VA is
  done with the surface.
- **CPU.** About 6 % of one core per 1080p30 monitor instead of about 28 %,
  and less work in gnome-shell. A still screen still costs a VA encode per
  keepalive, as today.
- **CI cannot see this.** The headless fixture has no GPU and no KMS CRTC, so it
  keeps covering the SHM path, and unit tests cover the negotiation and the
  fallback decisions. The mini-PC nightly gains a **scanout check**: a
  fullscreen GPU window that changes by itself, with the pointer hidden, which
  the decoded video must follow, while the KMS state proves the window was
  scanned out ([docs/15](../15-testing-strategy.md#the-desktop-test-lab)).
- **No new dependency.** libpipewire, GStreamer's VA plugin and its allocators
  are already the module's and the agent's (docs/14).
- **Revisit** once every supported robot runs mutter 50.5 or later, or a 26.04
  SRU with !5276 (SHM is then correct again; DMA-BUF stays for its CPU); if a GPU gives
  wrong frames under implicit sync (adopt explicit sync); if an encoder family
  other than `vaapi` needs a capture path (`nvcodec`, `nvv4l2`); if a VA driver
  refuses a modifier its caps list; or if virtual monitors are worth moving to
  DMA-BUF for their CPU.
