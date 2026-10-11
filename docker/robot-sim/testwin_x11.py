#!/usr/bin/env python3
# The X11 fixture's test window (docs/15#the-desktop-test-lab, M3 3.7): a bare Xlib window, fullscreen
# on the first monitor, logging what it receives in the format fjarr-opsim reads from the mutter
# fixture's window: "key <keysym> text='<c>'", "release <keysym>", "click button=N x= y=",
# "focus active=True|False". F9 held turns it green, for input-to-photon. A frame stamp across its
# top edge (fjarr-opsim's decode_stamp: 0xA5, a counter, a timestamp, a checksum) counts up at 15 Hz,
# so a lost picture shows as a skipped count (docs/06 "zero dropped frames", M3 gate review). Adapted from
# spikes/desktop-oracle/oracle_x11.py (the GTK oracle neither painted nor took focus under Openbox).
import os, select, time
from Xlib import X, XK, display

LOG = os.environ.get("FIXTURE_LOG", "/run/fixture/oracle/testwin.log")
# Keysym names ("Shift_L", "at", "aring"): python-xlib's keysym_to_string gives a key's TEXT, so the
# names come from its XK_ constants, with the "miscellany" group (modifiers, F-keys) loaded.
XK.load_keysym_group("miscellany")
NAMES = {getattr(XK, n): n[3:] for n in dir(XK) if n.startswith("XK_")}


def log(line):
    with open(LOG, "a") as f:
        f.write(f"{time.time():.3f} {line}\n")


d = display.Display()
s = d.screen()
cmap = s.default_colormap
magenta = cmap.alloc_color(0xffff, 0, 0xffff).pixel
green = cmap.alloc_color(0, 0xffff, 0).pixel
w = s.root.create_window(0, 0, 1920, 1080, 0, s.root_depth, background_pixel=magenta,
                         event_mask=X.KeyPressMask | X.KeyReleaseMask | X.ButtonPressMask | X.FocusChangeMask | X.ExposureMask)
w.set_wm_name("fjarr-testwin")
w.change_property(d.intern_atom("_NET_WM_STATE"), d.intern_atom("ATOM"), 32, [d.intern_atom("_NET_WM_STATE_FULLSCREEN")])
w.map()
d.sync()
log(f"start pid={os.getpid()} x11={os.environ.get('DISPLAY')}")


def text_of(keysym):
    if keysym == 0:
        return ""
    if keysym < 0x100:
        return chr(keysym)
    if 0x01000000 <= keysym <= 0x0110FFFF:
        return chr(keysym - 0x01000000)
    return ""


def handle(e):
    if e.type in (X.KeyPress, X.KeyRelease):
        level = 1 if e.state & X.ShiftMask else 0
        base = d.keycode_to_keysym(e.detail, 0)
        sym = d.keycode_to_keysym(e.detail, level) or base
        name = NAMES.get(sym) or NAMES.get(base) or str(sym)
        if e.type == X.KeyPress:
            text = text_of(sym)
            log(f"key {name}" + (f" text='{text}'" if text else ""))
            if name == "F9":
                w.change_attributes(background_pixel=green)
                w.clear_area()
                stamp(advance=False)
        else:
            log(f"release {name}")
            if name == "F9":
                w.change_attributes(background_pixel=magenta)
                w.clear_area()
                stamp(advance=False)
    elif e.type == X.ButtonPress:
        log(f"click button={e.detail} x={e.event_x} y={e.event_y}")
    elif e.type in (X.FocusIn, X.FocusOut):
        log(f"focus active={e.type == X.FocusIn}")


# The frame stamp: 96 blocks of 1920/128 px in the top 16 rows, white for a set bit (decode_stamp).
gc_white = w.create_gc(foreground=s.white_pixel)
gc_black = w.create_gc(foreground=s.black_pixel)
BLOCK = 1920 // 128
count = 0


def stamp(advance=True):
    # A repaint (F9) redraws the current count: an extra count the capture could miss would read as
    # a lost frame.
    global count
    if advance:
        count += 1
    data = [0xA5] + list(count.to_bytes(4, "big")) + list(int(time.time() * 1000).to_bytes(6, "big"))
    check = 0
    for b in data:
        check ^= b
    data.append(check)
    white, black = [], []
    for i in range(96):
        (white if (data[i >> 3] >> (7 - (i & 7))) & 1 else black).append((i * BLOCK, 0, BLOCK, 16))
    w.poly_fill_rectangle(gc_white, white)
    w.poly_fill_rectangle(gc_black, black)
    d.flush()


PERIOD = 1 / 15
next_at = time.monotonic()
while True:
    while d.pending_events():
        handle(d.next_event())
    now = time.monotonic()
    if now >= next_at:
        stamp()
        next_at = max(next_at + PERIOD, now)
    select.select([d.fileno()], [], [], max(0.0, next_at - time.monotonic()))
