#!/usr/bin/env python3
# The fixture's monitor hot-plug (M3 3.4): mutter virtual monitors plugged and unplugged on request,
# so CI can do to the desktop what a cable does on hardware. GET /plug makes one (RecordVirtual from
# a ScreenCast session of its own, consumed at 1280x720 so it has a size) and answers with its
# connector; GET /unplug removes the newest. mutter reports them like any monitor: MonitorsChanged,
# and gone from the layout when their session stops (measured 2026-10-01, mutter 50).
# spec: docs/15-testing-strategy.md#the-desktop-test-lab
import socket

import dbus
import dbus.mainloop.glib
import gi

gi.require_version("Gst", "1.0")
from gi.repository import GLib, Gst  # noqa: E402

SC, DC = "org.gnome.Mutter.ScreenCast", "org.gnome.Mutter.DisplayConfig"
PORT = 8091

dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
Gst.init(None)
bus = dbus.SessionBus()
plugged = []  # (session, pipeline, connector), oldest first


def connectors():
    state = dbus.Interface(bus.get_object(DC, "/org/gnome/Mutter/DisplayConfig"), DC).GetCurrentState()
    return [str(m[0][0]) for m in state[1]]


def wait(predicate, seconds):
    ctx = GLib.MainContext.default()
    end = GLib.get_monotonic_time() + int(seconds * 1e6)
    while not predicate() and GLib.get_monotonic_time() < end:
        ctx.iteration(False)
        GLib.usleep(20000)
    return predicate()


def plug():
    before = set(connectors())
    path = dbus.Interface(bus.get_object(SC, "/org/gnome/Mutter/ScreenCast"), SC).CreateSession({})
    session = dbus.Interface(bus.get_object(SC, path), SC + ".Session")
    stream = session.RecordVirtual({"cursor-mode": dbus.UInt32(1), "is-platform": True})
    node = {}
    bus.add_signal_receiver(lambda n: node.setdefault("n", int(n)), "PipeWireStreamAdded", SC + ".Stream", path=stream)
    session.Start()
    if not wait(lambda: "n" in node, 5):
        session.Stop()
        return "500 no stream"
    pipe = Gst.parse_launch(f"pipewiresrc path={node['n']} ! video/x-raw,width=1280,height=720,max-framerate=30/1 ! fakesink sync=false")
    pipe.set_state(Gst.State.PLAYING)
    if not wait(lambda: set(connectors()) - before, 5):
        pipe.set_state(Gst.State.NULL)
        session.Stop()
        return "500 no monitor appeared"
    connector = sorted(set(connectors()) - before)[0]
    plugged.append((session, pipe, connector))
    return "200 " + connector


def unplug():
    if not plugged:
        return "404 nothing plugged"
    session, pipe, connector = plugged.pop()
    pipe.set_state(Gst.State.NULL)
    session.Stop()
    wait(lambda: connector not in connectors(), 5)
    return "200 " + connector


def on_request(server, _cond):
    conn, _ = server.accept()
    try:
        line = conn.recv(1024).decode(errors="replace").split("\r\n")[0]
        target = line.split(" ")[1] if len(line.split(" ")) > 1 else ""
        result = plug() if target == "/plug" else unplug() if target == "/unplug" else "404 /plug or /unplug"
        code, body = result.split(" ", 1)
        conn.sendall(f"HTTP/1.0 {code} X\r\nContent-Type: text/plain\r\nContent-Length: {len(body)}\r\n\r\n{body}".encode())
    finally:
        conn.close()
    return True


server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind(("0.0.0.0", PORT))
server.listen(4)
GLib.io_add_watch(server.fileno(), GLib.IO_IN, lambda *_: on_request(server, None))
print(f"plugd: monitor hot-plug on :{PORT} (/plug, /unplug)", flush=True)
GLib.MainLoop().run()
