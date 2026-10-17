# Spike: the desktop capture freezes behind a directly scanned-out window

**Question (2026-10-17):** why did the mini-PC's nightly `input-to-photon` get
0/10 samples from 10-05 on, or pass on stale samples, while every frame
counter stayed green? And what fixes it?

**Answer:** since the local cursor (232dbbf, 2026-10-03) module E records GNOME
monitors in mutter's *metadata* cursor mode, which keeps the hardware cursor
plane. With a hardware cursor and nothing of the shell on screen, mutter 50.1
scans a fullscreen window that renders through dma-buf directly out to a
monitor that has a KMS CRTC, and while it does it records **no frame into
shared-memory (SHM) monitor streams**. The agent's reader re-sends its last
frame on a still screen, so the operator watched a frozen picture behind green
counters. DMA-BUF streams on the same monitor record every frame. Decided:
[ADR-0041](../../docs/adr/0041-desktop-capture-dmabuf.md), designed in
[docs/23](../../docs/23-agent-core-architecture.md#desktop-capture-memory).

Mutter fixed the SHM side upstream (merge request !5276, issue #3903, in mutter
50.5 and 51.0); Ubuntu 26.04 ships 50.1 without it
([open question #49](../../docs/18-open-questions.md)). The request to backport
it is drafted in [upstream-report.md](upstream-report.md).

## The machine

The lab mini-PC: Ryzen 7 5700U (amdgpu, radeonsi), Ubuntu 26.04, mutter and
gnome-shell 50.1, GTK 4.22, Mesa 26.0.8, PipeWire 1.6.2, GStreamer 1.28.2. One
monitor: a ghost screen on HDMI-A-1, a real KMS CRTC with a forced EDID
([ADR-0032](../../docs/adr/0032-ghost-screens.md)). Everything ran on the
nightly's own build, by hand, outside the nightly window, and the machine was
restored after each session.

## Method

What was on screen came from the kernel, not from a capture: `kmsmon.py`
samples the KMS state in debugfs every 20 ms and logs which framebuffer the
primary plane scans out (gnome-shell's composited `XR30`, or the window's own
`XR24` buffer during direct scanout) and whether the cursor plane is on;
`crcmon.py` logs the CRTC's frame CRC, i.e. what the connector displays.
Captures were independent of the agent: `capclient.py` (a ScreenCast session
read through `pipewiresrc`, SHM or DMA-BUF, logging each buffer's centre colour)
and `pwprobe.c` (a raw `pw_stream` consumer, the shape of module E's reader).
The window was `blinkwin.py`: fullscreen, GTK 4, changing colour by itself.
`blinkrun.py` starts a capture client a set time after a blink, so it starts
on a still, scanned-out screen; `blinktable.py`, `presstable.py`,
`wltimeline.py` and `cputable.py` merge the logs. mutter's own log came from
`MUTTER_DEBUG=render,screen-cast` (`mutterlog.sh`).

A notification banner, a menu or the overview stops direct scanout while it is
up, and an idle session keeps a banner up until there is input, so every run
moved the pointer first (`pointer.py nudge`) and was checked in `kms.log`.

## Results

| Run | What | Result |
|---|---|---|
| Reproduction | the nightly's `desktop-control`, unchanged | 0/10 four times of four (the decoded centre stays the window's magenta), or the 10-17 false pass when the first scanned-out frame was a release |
| Self-blinking window | fullscreen, default Vulkan renderer, an SHM metadata capture beside it | the CRC changed 24 times in 12.4 s while the plane showed the window's own buffers; the SHM capture got **0 buffers** |
| mutter's log | the reproduction with `MUTTER_DEBUG=render,screen-cast` | 20 of 20 key events: the window committed a buffer, mutter logged "Assigning scanout to stage view", and recorded no full frame into either SHM stream |
| GTK renderer `cairo` | SHM buffers, no scanout candidate | pass 10/10 twice (p50 150, p95 200 ms) |
| GTK renderer `gl` | dma-buf again | fail 0/10 twice: not a Vulkan bug |
| Window maximized | dock and top bar on screen, no scanout | pass 10/10 twice |
| `MUTTER_DEBUG_PAINT=disable-direct-scanout` | scanout off for the session | pass 10/10 twice; a blink run gave 54 frames for 41 changes |
| Software cursor (`MUTTER_DEBUG_DISABLE_HW_CURSORS=1`) | | pass 10/10 with the pointer visible; **fail 0/10** once the window hid the pointer |
| Embedded cursor | module E's own fallback | pass 10/10 only when the pointer moved after the stream started; a capture started during scanout with a still pointer got no frame through three restarts |
| DMA-BUF during scanout | a DMA-BUF capture beside the SHM one | 72 full frames in 36 s of scanout, every blink in the right colour; the SHM stream got none |
| DMA-BUF started during scanout | still screen, pointer hidden, 10 starts | first frame 6–18 ms after the stream started streaming, 10 of 10, including a second concurrent stream and a restart; then 33 of 33 later changes; SHM controls got none |
| Cursor metadata with DMA-BUF | metadata mode, pointer moving | `SPA_META_Cursor` on every buffer as over SHM: 118 motions at the exact position within 15 ms, a bitmap and hotspot after each shape change; a hidden pointer arrives as id 0 |
| Zero-copy into VA | `pipewiresrc ! vapostproc ! vah264enc` | negotiated `DMA_DRM AR24:0x0200000000000901` (the only RGB modifier `vapostproc` lists; mutter's own first choice is `0x0200000000401a01`), every frame imported as a VA surface; 5.6–6.2 % of one core at 1080p30 against 27.5–28.6 % for a stand-in of today's copying chain |

Caveats, from the adversarial checks of both sessions: the CRC is the same for
a flat colour composited or scanned out, so scanout continuity rests on the
20 ms KMS sampling; the 30 Hz torn-frame count has little power to find a
sync race; the comparison chain was a `pipewiresrc` stand-in, not the agent;
module E's own reader, an agent restart, a hot-plug and other GPUs were not
exercised.

## Tools

All in `tools/`, each with a usage header. They need root or the desktop user
(`as-desktop.sh`) on the machine under test, and they read debugfs.
`pwprobe.c` builds with `cc pwprobe.c $(pkg-config --cflags --libs libpipewire-0.3)`.
