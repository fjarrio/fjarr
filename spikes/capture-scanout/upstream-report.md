# Draft: SRU request for mutter in Ubuntu 26.04

**Status:** draft, written 2026-10-17. Erik files it on Launchpad
(`https://bugs.launchpad.net/ubuntu/+source/mutter/+filebug`), against
`mutter` in Ubuntu 26.04 (resolute), tagged `resolute`. Not filed upstream:
the bug is already fixed in mutter (merge request !5276, issue #3903, released
in mutter 50.5 on 2026-09-15 and in 51.0), so the request is a backport.

Before filing: check that a newer resolute upload has not picked it up
(`apt-get changelog mutter-common`), and that no Launchpad bug already asks
for it (search "direct scanout" and "screencast").

---

**Title:** [SRU] Screencasts without dma-buf freeze while a fullscreen window is directly scanned out

**[Impact]**

In mutter 50.1, while a view is directly scanned out, monitor screencast
streams that use shared memory (MemFd/MemPtr, not dma-buf) receive no frames:
`before_stage_painted` in `src/backends/meta-screen-cast-monitor-stream-src.c`
returns early for non-dma-buf streams, and the stage is not painted during a
scanout, so nothing records into them. A fullscreen application that renders
through dma-buf (GTK 4 with its Vulkan or GL renderer, EGL or Vulkan apps,
fullscreen browsers and video players) gets scanned out whenever the hardware
cursor is in use and nothing of the shell is on screen. Every shared-memory
consumer of the screencast (remote desktop and screen sharing tools reading
through `pipewiresrc` or `pw_stream` with shared memory) then shows a frozen
picture while the screen changes. A stream that starts during the scanout gets
no first frame at all. Notification banners, menus and the overview disable
scanout while they are visible, so the freeze comes and goes.

Upstream fixed it in mutter merge request !5276, "stream-source/monitor: Fix
direct scanout handling without dma-buf" (closes issue #3903): during scanout,
non-dma-buf streams queue the idle record, which paints the stage into the
shared-memory buffer. It is in mutter 50.5 ("Fix direct scanout handling for
captures without dma-buf" in NEWS) and 51.0. Ubuntu 26.04 has 50.1
(50.1-0ubuntu2.4 in resolute-updates on 2026-10-17), without it.

**[Test Plan]**

On Ubuntu 26.04 with a GPU and a real monitor, in a GNOME Wayland session:

1. Start a screencast of the monitor through
   `org.gnome.Mutter.ScreenCast` `RecordMonitor` with `cursor-mode` 2
   (metadata), and read it through
   `pipewiresrc ! video/x-raw ! fakesink` (shared memory), logging buffers.
2. Open a fullscreen GTK 4 window that changes colour every 500 ms (the
   default renderer is enough). Keep the pointer still, with no notification
   on screen.
3. Without the fix, the screen changes but the capture receives no buffers
   (`MUTTER_DEBUG=render,screen-cast` logs "Assigning scanout to stage view"
   and no "Recording full frame"). With the fix, every change arrives.
4. Regression check: the same with a dma-buf consumer
   (`video/x-raw(memory:DMABuf)`) still receives every frame, and a
   composited (non-fullscreen) window still records normally.

**[Where problems could occur]**

The change only affects monitor screencast streams that are not dma-buf, and
only while a view is directly scanned out: such a stream now triggers a paint
of the stage into its buffer. Problems would show as extra GPU work or frame
pacing changes for shared-memory screencasts during scanout, not for normal
desktop rendering or dma-buf consumers.

**[Other Info]**

Found while debugging a remote desktop agent on Ubuntu 26.04 (Ryzen 7 5700U,
amdgpu, mutter 50.1, PipeWire 1.6.2), with the KMS plane state and CRTC CRC
confirming what was on screen while the shared-memory capture received
nothing. The upstream fix was cherry-picked to gnome-50 (817424e8).

A related behaviour, still present on mutter main and not part of this
request: adding a hardware-cursor inhibitor (as embedded cursor mode does)
takes effect only at the next cursor update, so an embedded-cursor stream
started while the pointer is still does not end a scanout by itself.
