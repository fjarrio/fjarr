#!/usr/bin/env python3
"""blinkwin.py  (desktop user, in the Wayland session)

The capture-scanout test window: a GTK4 window in one flat colour (magenta) that turns green while
F9 is held, and, with FJARR_TESTWIN_BLINK_MS, toggles magenta and green by itself so it keeps
presenting new frames without any input. A fullscreen GTK4 window renders through Vulkan (or GL)
into dma-bufs, so mutter can scan it out directly; that is the case this spike is about. It is a
standalone copy of docker/desktop-fixture/testwin.py with the experiment options added.

Every event goes to the log (FJARR_FIXTURE_LOG, default ./testwin.log) as "<epoch> <event>":
start, focus, key, release, click, cursor, blink. Run it with WAYLAND_DEBUG=1 and stderr to
testwin-app.log to see its commits, dma-buf feedback and presentation feedback (wltimeline.py).

Environment:
  FJARR_TESTWIN_MODE=fullscreen|maximize|window  fullscreen (default) can be scanned out; maximize
                                                 and window leave the shell's panels visible and cannot
  FJARR_TESTWIN_CURSOR=NAME                      cursor over the window; "none" hides the pointer
  FJARR_TESTWIN_CURSOR_CYCLE=a,b,c               switch the cursor through these names ...
  FJARR_TESTWIN_CURSOR_CYCLE_MS=N                ... every N ms (a new bitmap for cursor metadata)
  FJARR_TESTWIN_BLINK_MS=N                       toggle magenta/green every N ms
  FJARR_TESTWIN_BLINK_COUNT=N                    stop blinking after N toggles (even: ends magenta)
  GSK_RENDERER=cairo                             wl_shm buffers instead: never scanned out (lever E2)

Example: FJARR_TESTWIN_BLINK_MS=500 FJARR_TESTWIN_CURSOR=none python3 blinkwin.py
"""
import os
import sys
import time

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Gdk", "4.0")
from gi.repository import Gdk, GLib, Gtk  # noqa: E402

LOG = os.environ.get("FJARR_FIXTURE_LOG", "testwin.log")
COLOUR = "#ff00ff"  # magenta: nothing else on a stock desktop is
PHOTON = "#00ff00"  # green: while F9 is held, and every other blink
css = None


def log(line):
    with open(LOG, "a") as f:
        f.write(f"{time.time():.3f} {line}\n")


def paint(colour):
    css.load_from_data(f"window {{ background: {colour}; }}".encode())


def on_key(_c, keyval, _code, _state):
    log(f"key {Gdk.keyval_name(keyval)}")
    if keyval == Gdk.KEY_F9:
        paint(PHOTON)
    return False


def on_release(_c, keyval, _code, _state):
    log(f"release {Gdk.keyval_name(keyval)}")
    if keyval == Gdk.KEY_F9:
        paint(COLOUR)


def activate(app):
    global css
    win = Gtk.ApplicationWindow(application=app, title="capture-scanout blink window")
    css = Gtk.CssProvider()
    paint(COLOUR)
    Gtk.StyleContext.add_provider_for_display(Gdk.Display.get_default(), css, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)
    keys = Gtk.EventControllerKey()
    keys.connect("key-pressed", on_key)
    keys.connect("key-released", on_release)
    win.add_controller(keys)
    click = Gtk.GestureClick(button=0)
    click.connect("pressed", lambda g, _n, x, y: log(f"click button={g.get_current_button()} x={x:.0f} y={y:.0f}"))
    win.add_controller(click)
    win.connect("notify::is-active", lambda w, _p: log(f"focus active={w.is_active()}"))

    mode = os.environ.get("FJARR_TESTWIN_MODE", "fullscreen")
    if mode == "maximize":
        win.maximize()
    elif mode == "window":
        win.set_default_size(1200, 800)
    else:
        win.fullscreen()
    if os.environ.get("FJARR_TESTWIN_CURSOR"):
        win.set_cursor_from_name(os.environ["FJARR_TESTWIN_CURSOR"])
    win.present()
    log(f"start pid={os.getpid()} colour={COLOUR} mode={mode} cursor={os.environ.get('FJARR_TESTWIN_CURSOR', 'default')}")

    cycle = [c for c in os.environ.get("FJARR_TESTWIN_CURSOR_CYCLE", "").split(",") if c]
    cycle_ms = int(os.environ.get("FJARR_TESTWIN_CURSOR_CYCLE_MS", "0"))
    if cycle and cycle_ms > 0:
        cstate = {"i": 0}

        def next_cursor():
            cstate["i"] = (cstate["i"] + 1) % len(cycle)
            win.set_cursor_from_name(cycle[cstate["i"]])
            log(f"cursor {cycle[cstate['i']]}")
            return True

        win.set_cursor_from_name(cycle[0])
        log(f"cursor {cycle[0]}")
        GLib.timeout_add(cycle_ms, next_cursor)

    blink = int(os.environ.get("FJARR_TESTWIN_BLINK_MS", "0"))
    if blink > 0:
        state = {"green": False, "left": int(os.environ.get("FJARR_TESTWIN_BLINK_COUNT", "0")) or -1}

        def tick():
            state["green"] = not state["green"]
            paint(PHOTON if state["green"] else COLOUR)
            log("blink " + ("green" if state["green"] else "magenta"))
            state["left"] -= 1
            return state["left"] != 0

        GLib.timeout_add(blink, tick)


app = Gtk.Application(application_id="io.fjarr.CaptureScanoutBlink")
app.connect("activate", activate)
sys.exit(app.run([]))
