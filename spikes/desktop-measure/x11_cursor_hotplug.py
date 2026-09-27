#!/usr/bin/env python3
# X11 criteria (docs/07): cursor metadata through XFixes (shape readable, change events), and
# monitor hot-plug as RandR reports it. Usage: x11_cursor_hotplug.py SECONDS — listens that long
# for RandR/XFixes events while something else changes the outputs. Throwaway spike code.
import sys, time
from Xlib import X, display
from Xlib.ext import randr, xfixes

d = display.Display(); root = d.screen().root; T0 = time.time()
log = lambda m: print(f"{time.time()-T0:6.1f}s {m}", flush=True)
d.xfixes_query_version()  # XFixes refuses requests until the version is negotiated
img = d.xfixes_get_cursor_image(root)
log(f"xfixes cursor image: {img.width}x{img.height} hot=({img.xhot},{img.yhot}) serial={img.cursor_serial}")
d.xfixes_select_cursor_input(root, xfixes.XFixesDisplayCursorNotifyMask)
root.xrandr_select_input(randr.RRScreenChangeNotifyMask | randr.RROutputChangeNotifyMask | randr.RRCrtcChangeNotifyMask)
res = root.xrandr_get_screen_resources()
for o in res.outputs:
    info = d.xrandr_get_output_info(o, res.config_timestamp)
    log(f"output {info.name} connection={'connected' if info.connection == 0 else 'disconnected'} crtc={info.crtc}")
end = time.time() + float(sys.argv[1])
while time.time() < end:
    while d.pending_events():
        e = d.next_event()
        log(f"event {type(e).__name__} {getattr(e, 'sub_code', '')} {getattr(e, 'width', '')}x{getattr(e, 'height', '')}")
    time.sleep(0.05)
