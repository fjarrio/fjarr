#!/usr/bin/env python3
# Candidate A (docs/07): X11 capture with ximagesrc and XTest injection. Needs DISPLAY and an
# XAUTHORITY the caller can read — which one, and how it was found, is the finding. With
# --capture-only it is candidate B's capture half (B injects with desktop-d/inject_uinput.py).
# Throwaway spike code.
import sys, time
import gi
gi.require_version("Gst", "1.0")
from gi.repository import Gst

Gst.init(None)
pipe = Gst.parse_launch("ximagesrc use-damage=false num-buffers=5 ! videoconvert ! "
                        "video/x-raw,format=RGB ! appsink name=sink sync=false")
pipe.set_state(Gst.State.PLAYING)
sample = pipe.get_by_name("sink").emit("try-pull-sample", 10 * Gst.SECOND)
if not sample:
    print("CAPTURE no: no frame within 10 s")
else:
    st = sample.get_caps().get_structure(0); w, h = st.get_value("width"), st.get_value("height")
    _, info = sample.get_buffer().map(Gst.MapFlags.READ); d = info.data; stride = len(d) // h
    px = [(d[y*stride+3*x], d[y*stride+3*x+1], d[y*stride+3*x+2]) for y in range(0, h, 16) for x in range(0, w, 16)]
    m = sum(1 for r, g, b in px if r > 200 and g < 60 and b > 200)
    print(f"CAPTURE {'yes' if m > len(px)//2 else 'frames-but-no-oracle'}: {w}x{h}, {100*m//len(px)}% magenta")
pipe.set_state(Gst.State.NULL)
if "--capture-only" in sys.argv:
    sys.exit(0)

from Xlib import X, XK, display
from Xlib.ext import xtest
dpy = display.Display()
for ch in ("a", "x"):
    code = dpy.keysym_to_keycode(XK.string_to_keysym(ch))
    xtest.fake_input(dpy, X.KeyPress, code); xtest.fake_input(dpy, X.KeyRelease, code)
xtest.fake_input(dpy, X.MotionNotify, x=543, y=210)
xtest.fake_input(dpy, X.ButtonPress, 1); xtest.fake_input(dpy, X.ButtonRelease, 1)
dpy.sync(); time.sleep(0.5)
print("INJECT sent via XTest: key a, key x, click at 543,210 — read the oracle log")
