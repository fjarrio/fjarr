#!/usr/bin/env python3
# The fixture's test window: fullscreen, one known colour (so a captured frame can be checked for
# it), and a log of every focus change, key (name and text) and click (button and position), so
# input is proven by reading the log back. spec: docs/15-testing-strategy.md#the-desktop-test-lab
import os, sys, time
import gi
gi.require_version("Gtk", "4.0")
gi.require_version("Gdk", "4.0")
from gi.repository import Gdk, Gtk

LOG = os.environ.get("FJARR_FIXTURE_LOG", "/run/desktop/testwin.log")
COLOUR = os.environ.get("FIXTURE_COLOUR", "#ff00ff")  # magenta: nothing else on the desktop is


def log(line):
    with open(LOG, "a") as f:
        f.write(f"{time.time():.3f} {line}\n")


def on_key(_c, keyval, _code, _state):
    uni = Gdk.keyval_to_unicode(keyval)
    text = chr(uni) if uni and chr(uni).isprintable() else ""
    log(f"key {Gdk.keyval_name(keyval)} text={text!r}")
    return False


def activate(app):
    win = Gtk.ApplicationWindow(application=app, title="fjarr-fixture")
    css = Gtk.CssProvider()
    css.load_from_data(f"window {{ background: {COLOUR}; }}".encode())
    Gtk.StyleContext.add_provider_for_display(Gdk.Display.get_default(), css, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)
    keys = Gtk.EventControllerKey()
    keys.connect("key-pressed", on_key)
    win.add_controller(keys)
    click = Gtk.GestureClick(button=0)
    click.connect("pressed", lambda g, _n, x, y: log(f"click button={g.get_current_button()} x={x:.0f} y={y:.0f}"))
    win.add_controller(click)
    win.connect("notify::is-active", lambda w, _p: log(f"focus active={w.is_active()}"))
    win.fullscreen()
    win.present()
    log(f"start pid={os.getpid()} colour={COLOUR}")


app = Gtk.Application(application_id="io.fjarr.FixtureWindow")
app.connect("activate", activate)
sys.exit(app.run([]))
