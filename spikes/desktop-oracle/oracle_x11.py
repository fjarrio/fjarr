#!/usr/bin/env python3
# The injection oracle for X11 sessions. The GTK4 oracle neither painted nor reliably took focus
# under Openbox on the spike machine's Xorg, which made the instrument the variable. This one is a
# bare Xlib window whose magenta background the X server paints itself, fullscreened through the
# window manager, logging keys and clicks exactly as oracle.py does. Throwaway spike code.
import os, time
from Xlib import X, XK, display

LOG = os.path.expanduser(os.environ.get("FJARR_ORACLE_LOG", "~/fjarr-oracle.log"))
def log(line):
    with open(LOG, "a") as f: f.write(f"{time.time():.3f} {line}\n")

d = display.Display(); s = d.screen()
magenta = s.default_colormap.alloc_color(0xffff, 0, 0xffff).pixel
w = s.root.create_window(0, 0, s.width_in_pixels, s.height_in_pixels, 0, s.root_depth,
                         background_pixel=magenta,
                         event_mask=X.KeyPressMask | X.ButtonPressMask | X.FocusChangeMask | X.ExposureMask)
w.set_wm_name("fjarr-oracle")
w.change_property(d.intern_atom("_NET_WM_STATE"), d.intern_atom("ATOM"), 32,
                  [d.intern_atom("_NET_WM_STATE_FULLSCREEN")])
w.map(); d.sync()
log(f"start pid={os.getpid()} x11={os.environ.get('DISPLAY')} xlib")
while True:
    e = d.next_event()
    if e.type == X.KeyPress:
        log(f"key {XK.keysym_to_string(d.keycode_to_keysym(e.detail, 0))}")
    elif e.type == X.ButtonPress:
        log(f"click button={e.detail} x={e.event_x} y={e.event_y}")
    elif e.type in (X.FocusIn, X.FocusOut):
        log(f"focus active={e.type == X.FocusIn}")
