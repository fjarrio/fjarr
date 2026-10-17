#!/usr/bin/env python3
"""castsession.py MODE SECONDS OUTLOG [CONNECTOR]  (desktop user, in the session: see as-desktop.sh)

A mutter ScreenCast session of its own (RecordMonitor CONNECTOR, default
HDMI-1, cursor-mode MODE = hidden|embedded|metadata), started and held open until SECONDS pass or
the run's stop file (OUTLOG's directory/stop) appears. It has no consumer: it logs the PipeWire
node id and writes it to OUTLOG.node, so another process (gst-launch-1.0 pipewiresrc, pwprobe)
links to the node. Stops the session on exit.

Example: ./as-desktop.sh python3 castsession.py metadata 60 runs/r1/cast-A.log &
         ./as-desktop.sh ./pwprobe "$(cat runs/r1/cast-A.log.node)" 30 runs/r1/probe-A.log --dmabuf 0x0200000000000901
"""
import os
import sys
import time

from gi.repository import Gio, GLib

MODES = {"hidden": 0, "embedded": 1, "metadata": 2}
mode_name = sys.argv[1]
seconds = float(sys.argv[2])
outlog = sys.argv[3]
connector = sys.argv[4] if len(sys.argv) > 4 else "HDMI-1"
out = open(outlog, "a", buffering=1)
STOP = os.path.join(os.path.dirname(os.path.abspath(outlog)), "stop")


def log(line):
    out.write(f"{time.time():.3f} {line}\n")


bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
SC = "org.gnome.Mutter.ScreenCast"


def call(path, iface, method, params, rtype):
    return bus.call_sync(SC, path, iface, method, params, GLib.VariantType(rtype) if rtype else None,
                         Gio.DBusCallFlags.NONE, 5000, None)


session = call("/org/gnome/Mutter/ScreenCast", SC, "CreateSession", GLib.Variant("(a{sv})", ({},)), "(o)")[0]
stream = call(session, SC + ".Session", "RecordMonitor",
              GLib.Variant("(sa{sv})", (connector, {"cursor-mode": GLib.Variant("u", MODES[mode_name])})), "(o)")[0]
loop = GLib.MainLoop()
state = {"node": None}


def on_added(_c, _s, _p, _i, _sig, params, *_a):
    state["node"] = params[0]
    loop.quit()


bus.signal_subscribe(SC, SC + ".Stream", "PipeWireStreamAdded", stream, None, Gio.DBusSignalFlags.NONE, on_added)
call(session, SC + ".Session", "Start", None, None)
GLib.timeout_add(5000, loop.quit)
loop.run()
if state["node"] is None:
    log("ERROR no PipeWireStreamAdded within 5 s")
    sys.exit(1)
log(f"start mode={mode_name} connector={connector} session={session} stream={stream} node={state['node']} pid={os.getpid()}")
with open(outlog + ".node", "w") as f:
    f.write(f"{state['node']}\n")

loop2 = GLib.MainLoop()
GLib.timeout_add(int(seconds * 1000), loop2.quit)
GLib.timeout_add(200, lambda: (loop2.quit() if os.path.exists(STOP) else None) or True)
loop2.run()
try:
    call(session, SC + ".Session", "Stop", None, None)
except GLib.Error as e:
    log(f"stop: {e.message}")
log("end")
