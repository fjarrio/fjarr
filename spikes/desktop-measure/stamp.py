#!/usr/bin/env python3
# Phase 2 latency oracle for Wayland (docs/07): a fullscreen window that paints the wall clock
# (ms mod 2^24) as 24 black/white blocks across the top on every frame, and flips the lower half
# between black and white on each key press or click. A capture decodes the clock to measure
# paint→capture latency and watches the lower half to measure input→photon. Throwaway spike code.
import os, sys, time
import gi
gi.require_version("Gtk", "4.0")
from gi.repository import Gtk

BITS, BLOCK, STRIP = 24, 60, 40
state = {"lit": False}

def draw(area, cr, w, h, *_):
    cr.set_source_rgb(1, 0, 1); cr.paint()
    stamp = int(time.time() * 1000) & 0xFFFFFF
    for i in range(BITS):
        v = (stamp >> (BITS - 1 - i)) & 1
        cr.set_source_rgb(v, v, v); cr.rectangle(i * BLOCK, 0, BLOCK, STRIP); cr.fill()
    v = 1.0 if state["lit"] else 0.0
    cr.set_source_rgb(v, v, v); cr.rectangle(0, h // 2, w, h - h // 2); cr.fill()

def flip(*_):
    state["lit"] = not state["lit"]; area.queue_draw(); return False

def activate(app):
    global area
    win = Gtk.ApplicationWindow(application=app, title="fjarr-stamp")
    area = Gtk.DrawingArea(); area.set_draw_func(draw); win.set_child(area)
    area.add_tick_callback(lambda w, clock: w.queue_draw() or True)
    keys = Gtk.EventControllerKey(); keys.connect("key-pressed", lambda *a: flip()); win.add_controller(keys)
    click = Gtk.GestureClick(button=0); click.connect("pressed", flip); win.add_controller(click)
    win.fullscreen(); win.present()

app = Gtk.Application(application_id="io.fjarr.Stamp")
app.connect("activate", activate)
sys.exit(app.run([]))
