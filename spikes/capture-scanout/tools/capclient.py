#!/usr/bin/env python3
"""capclient.py MODE SECONDS OUTLOG [CONNECTOR]  (desktop user, in the session: see as-desktop.sh)

An independent mutter ScreenCast consumer, separate from the agent. It records CONNECTOR (mutter's
connector name, default HDMI-1) in a ScreenCast session of its own with cursor-mode MODE
(hidden|embedded|metadata), reads the PipeWire stream through pipewiresrc into an appsink, and
logs every buffer: arrival time, size, and the mean colour of the 16x16 centre block, classified
as magenta, green or other (the colours of blinkwin.py). Nothing is re-sent: a silent stream
logs nothing, which is the point.

By default the buffers are shared memory (always-copy, plain video/x-raw caps), as the agent reads
them today. CAP_DMABUF=1 negotiates DMA-BUF instead and downloads each frame through GL
(glupload ! glcolorconvert ! gldownload) to read the colour.

Environment:
  CAP_DMABUF=1          DMA-BUF instead of SHM
  CAP_DUMP=path.ppm     keep the newest frame as a PPM, a quarter size (ppm2png.py converts it)
  CAP_CROP=x,y,w,h      ... or only this rectangle, full size

Ends after SECONDS, or when a file named "stop" appears next to OUTLOG.
Example: ./as-desktop.sh python3 capclient.py metadata 60 runs/r1/cap.log HDMI-1
"""
import os
import sys
import time

import gi

gi.require_version("Gst", "1.0")
from gi.repository import Gio, GLib, Gst  # noqa: E402

MODES = {"hidden": 0, "embedded": 1, "metadata": 2}
mode_name = sys.argv[1]
seconds = float(sys.argv[2])
out = open(sys.argv[3], "a", buffering=1)
connector = sys.argv[4] if len(sys.argv) > 4 else "HDMI-1"
STOP = os.path.join(os.path.dirname(os.path.abspath(sys.argv[3])), "stop")  # the run's stop file ends it early


def log(line):
    out.write(f"{time.time():.3f} {line}\n")


Gst.init(None)
bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
SC = "org.gnome.Mutter.ScreenCast"


def call(path, iface, method, params, rtype):
    return bus.call_sync(SC, path, iface, method, params, GLib.VariantType(rtype) if rtype else None,
                         Gio.DBusCallFlags.NONE, 5000, None)


session = call("/org/gnome/Mutter/ScreenCast", SC, "CreateSession", GLib.Variant("(a{sv})", ({},)), "(o)")[0]
stream = call(session, SC + ".Session", "RecordMonitor",
              GLib.Variant("(sa{sv})", (connector, {"cursor-mode": GLib.Variant("u", MODES[mode_name])})), "(o)")[0]
loop = GLib.MainLoop()
state = {"node": None, "n": 0, "empty": 0, "last": None, "pipe": None}


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
log(f"start mode={mode_name} connector={connector} session={session} stream={stream} node={state['node']}")


def on_sample(sink):
    sample = sink.emit("pull-sample")
    t = time.time()
    if sample is None:
        return Gst.FlowReturn.OK
    buf = sample.get_buffer()
    st = sample.get_caps().get_structure(0)
    w, h, fmt = st.get_value("width"), st.get_value("height"), st.get_value("format")
    ok, info = buf.map(Gst.MapFlags.READ)
    if not ok:
        log("buffer unmappable")
        return Gst.FlowReturn.OK
    try:
        size = info.size
        state["n"] += 1
        if not w or not h or size < w * h * 4:
            state["empty"] += 1
            log(f"buf n={state['n']} EMPTY size={size} {w}x{h} {fmt} flags={int(buf.get_flags())}")
            return Gst.FlowReturn.OK
        stride = size // h
        data = info.data
        rs = gs = bs = 0
        cx, cy = w // 2 - 8, h // 2 - 8
        for y in range(cy, cy + 16):
            row = y * stride
            for x in range(cx, cx + 16):
                p = row + x * 4
                if fmt in ("BGRx", "BGRA"):
                    bs += data[p]; gs += data[p + 1]; rs += data[p + 2]
                else:
                    rs += data[p]; gs += data[p + 1]; bs += data[p + 2]
        r, g, b = rs // 256, gs // 256, bs // 256
        colour = "magenta" if r > 200 and b > 200 and g < 60 else "green" if g > 200 and r < 60 and b < 60 else "other"
        if state["last"] is None:
            log(f"caps {w}x{h} {fmt} stride={stride}")
        log(f"buf n={state['n']} size={size} centre=({r},{g},{b}) {colour}")
        state["last"] = colour
        dump = os.environ.get("CAP_DUMP")  # CAP_DUMP=path.ppm: keep the newest frame, a quarter size
        if dump:
            rows = []
            crop = [int(v) for v in os.environ.get("CAP_CROP", f"0,0,{w},{h}").split(",")]  # x,y,w,h
            step = 1 if "CAP_CROP" in os.environ else 4
            cw, ch = crop[2], crop[3]
            for y in range(crop[1], crop[1] + ch, step):
                row = bytearray()
                base = y * stride
                for x in range(crop[0], crop[0] + cw, step):
                    p = base + x * 4
                    if fmt in ("BGRx", "BGRA"):
                        row += bytes((data[p + 2], data[p + 1], data[p]))
                    else:
                        row += bytes((data[p], data[p + 1], data[p + 2]))
                rows.append(bytes(row))
            with open(dump, "wb") as f:
                f.write(f"P6\n{len(rows[0]) // 3} {len(rows)}\n255\n".encode())
                f.write(b"".join(rows))
    finally:
        buf.unmap(info)
    return Gst.FlowReturn.OK


# CAP_DMABUF=1: let mutter hand DMA-BUFs (fix option (a)); GL downloads them to system memory here.
if os.environ.get("CAP_DMABUF"):
    desc = (f"pipewiresrc path={state['node']} ! video/x-raw(memory:DMABuf) ! glupload ! glcolorconvert ! gldownload ! "
            "video/x-raw,format=BGRx ! appsink name=sink emit-signals=true sync=false max-buffers=8")
else:
    desc = f"pipewiresrc path={state['node']} always-copy=true ! appsink name=sink emit-signals=true sync=false max-buffers=8 caps=video/x-raw"
log(f"pipeline {desc}")
pipe = Gst.parse_launch(desc)
pipe.get_by_name("sink").connect("new-sample", on_sample)


def on_msg(_b, msg):
    if msg.type == Gst.MessageType.ERROR:
        err, dbg = msg.parse_error()
        log(f"ERROR gst {err.message} {dbg}")
        loop.quit()
    elif msg.type == Gst.MessageType.STATE_CHANGED and msg.src == pipe:
        _old, new, _pend = msg.parse_state_changed()
        log(f"pipeline {Gst.Element.state_get_name(new)}")


gbus = pipe.get_bus()
gbus.add_signal_watch()
gbus.connect("message", on_msg)
pipe.set_state(Gst.State.PLAYING)
loop2 = GLib.MainLoop()
loop = loop2
GLib.timeout_add(int(seconds * 1000), loop2.quit)
GLib.timeout_add(200, lambda: (loop2.quit() if os.path.exists(STOP) else None) or True)
loop2.run()
pipe.set_state(Gst.State.NULL)
try:
    call(session, SC + ".Session", "Stop", None, None)
except GLib.Error as e:
    log(f"stop: {e.message}")
log(f"end buffers={state['n']} empty={state['empty']}")
