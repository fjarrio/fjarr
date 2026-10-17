#!/usr/bin/env python3
# The fixture's test window: fullscreen, one known colour (so a captured frame can be checked for
# it), and a log of every focus change, key press and release (name and text) and click (button
# and position), so input is proven by reading the log back. While F9 is held the window turns
# green: the "photon" the input-to-photon harness times (docs/15#latency-harness).
# The scanout check (ADR-0041) runs it changing colour by itself with the pointer hidden, ported
# from spikes/capture-scanout/tools/blinkwin.py: fullscreen, GTK 4 renders through the GPU into
# dma-bufs, so mutter can scan it out directly.
#   FJARR_TESTWIN_BLINK_MS=N   toggle magenta and green every N ms, logging "blink green|magenta"
#   FJARR_TESTWIN_CURSOR=NAME  the pointer over the window; "none" hides it
# spec: docs/15-testing-strategy.md#the-desktop-test-lab
import os, sys, time
import gi
gi.require_version("Gtk", "4.0")
gi.require_version("Gdk", "4.0")
from gi.repository import Gdk, GLib, Gtk

LOG = os.environ.get("FJARR_FIXTURE_LOG", "/run/desktop/testwin.log")
COLOUR = os.environ.get("FIXTURE_COLOUR", "#ff00ff")  # magenta: nothing else on the desktop is
PHOTON = "#00ff00"  # while F9 is held
css = None


def log(line):
    with open(LOG, "a") as f:
        f.write(f"{time.time():.3f} {line}\n")


def paint(colour):
    css.load_from_data(f"window {{ background: {colour}; }}".encode())


def on_key(_c, keyval, _code, _state):
    uni = Gdk.keyval_to_unicode(keyval)
    text = chr(uni) if uni and chr(uni).isprintable() else ""
    log(f"key {Gdk.keyval_name(keyval)} text={text!r}")
    if keyval == Gdk.KEY_F9:
        paint(PHOTON)
    return False


def on_release(_c, keyval, _code, _state):
    log(f"release {Gdk.keyval_name(keyval)}")
    if keyval == Gdk.KEY_F9:
        paint(COLOUR)


def activate(app):
    win = Gtk.ApplicationWindow(application=app, title="fjarr-fixture")
    global css
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
    # A drag's positions, which the window keeps receiving past its own monitor while the button is
    # held (Wayland's implicit grab): the oracle for a drag that crosses monitors (docs/22#drag-across-monitors).
    drag = Gtk.GestureDrag()
    drag.connect("drag-update", lambda g, dx, dy: log("drag x={:.0f} y={:.0f}".format(*(lambda p: (p[1] + dx, p[2] + dy))(g.get_start_point()))))
    win.add_controller(drag)
    win.connect("notify::is-active", lambda w, _p: log(f"focus active={w.is_active()}"))
    win.fullscreen()
    cursor = os.environ.get("FJARR_TESTWIN_CURSOR")
    if cursor:
        win.set_cursor_from_name(cursor)
    win.present()
    log(f"start pid={os.getpid()} colour={COLOUR} cursor={cursor or 'default'}")

    blink = int(os.environ.get("FJARR_TESTWIN_BLINK_MS", "0"))
    if blink > 0:
        state = {"green": False}

        def tick():
            state["green"] = not state["green"]
            paint(PHOTON if state["green"] else COLOUR)
            log("blink " + ("green" if state["green"] else "magenta"))
            return True

        GLib.timeout_add(blink, tick)


app = Gtk.Application(application_id="io.fjarr.FixtureWindow")
app.connect("activate", activate)
sys.exit(app.run([]))
