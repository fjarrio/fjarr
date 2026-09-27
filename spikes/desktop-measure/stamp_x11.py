#!/usr/bin/env python3
# The X11 twin of stamp.py, in bare Xlib for the same reason as oracle_x11.py: the same clock strip
# repainted at 60 Hz and the same lower half flipped on each key or click. Throwaway spike code.
import time
from Xlib import X, display

BITS, BLOCK, STRIP = 24, 60, 40
d = display.Display(); s = d.screen(); cm = s.default_colormap
W, H = s.width_in_pixels, s.height_in_pixels
px = lambda r, g, b: cm.alloc_color(r, g, b).pixel
white, black, magenta = px(0xffff, 0xffff, 0xffff), px(0, 0, 0), px(0xffff, 0, 0xffff)
w = s.root.create_window(0, 0, W, H, 0, s.root_depth, background_pixel=magenta,
                         event_mask=X.KeyPressMask | X.ButtonPressMask | X.ExposureMask)
w.set_wm_name("fjarr-stamp")
w.change_property(d.intern_atom("_NET_WM_STATE"), d.intern_atom("ATOM"), 32, [d.intern_atom("_NET_WM_STATE_FULLSCREEN")])
w.map(); d.sync(); time.sleep(0.5); w.set_input_focus(X.RevertToParent, X.CurrentTime)
gw, gb = w.create_gc(foreground=white), w.create_gc(foreground=black)
lit = False
def lower(): w.fill_rectangle(gw if lit else gb, 0, H // 2, W, H - H // 2)
lower()
while True:
    while d.pending_events():
        e = d.next_event()
        if e.type in (X.KeyPress, X.ButtonPress):
            lit = not lit; lower()
        elif e.type == X.Expose:
            lower()
    stamp = int(time.time() * 1000) & 0xFFFFFF
    for i in range(BITS):
        w.fill_rectangle(gw if (stamp >> (BITS - 1 - i)) & 1 else gb, i * BLOCK, 0, BLOCK, STRIP)
    d.flush()
    time.sleep(1 / 60)
