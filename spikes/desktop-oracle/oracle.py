#!/usr/bin/env python3
# The injection oracle (docs/07#phase-1-in-detail): a fullscreen window the session autostarts. It
# paints one known colour, so a captured frame can be checked for it, and appends every key and
# click it receives to a file, so injection is proven by reading the file back — never by a call
# returning success. Throwaway spike code.
import os, sys, time
import gi
gi.require_version("Gtk", "4.0")
gi.require_version("Gdk", "4.0")
from gi.repository import Gtk, Gdk

LOG = os.path.expanduser(os.environ.get("FJARR_ORACLE_LOG", "~/fjarr-oracle.log"))
COLOUR = "#ff00ff"  # magenta: nothing else on a stock desktop is this colour

def log(line):
    with open(LOG, "a") as f:
        f.write(f"{time.time():.3f} {line}\n")

def activate(app):
    win = Gtk.ApplicationWindow(application=app, title="fjarr-oracle")
    css = Gtk.CssProvider()
    css.load_from_data(f"window {{ background: {COLOUR}; }}".encode())
    Gtk.StyleContext.add_provider_for_display(Gdk.Display.get_default(), css, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)
    keys = Gtk.EventControllerKey()
    keys.connect("key-pressed", lambda c, kv, code, st: log(f"key {Gdk.keyval_name(kv)}") or False)
    win.add_controller(keys)
    click = Gtk.GestureClick(button=0)
    click.connect("pressed", lambda g, n, x, y: log(f"click button={g.get_current_button()} x={x:.0f} y={y:.0f}"))
    win.add_controller(click)
    win.connect("notify::is-active", lambda w, _: log(f"focus active={w.is_active()}"))
    win.fullscreen()
    win.present()
    log(f"start pid={os.getpid()} wayland={os.environ.get('WAYLAND_DISPLAY')} x11={os.environ.get('DISPLAY')}")

app = Gtk.Application(application_id="io.fjarr.Oracle")
app.connect("activate", activate)
sys.exit(app.run([]))
