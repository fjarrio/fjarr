#!/usr/bin/env python3
# The fixture's monitor hot-plug (M3 3.4): mutter virtual monitors plugged and unplugged on request,
# so CI can do to the desktop what a cable does on hardware. GET /plug makes one (RecordVirtual from
# a ScreenCast session of its own, consumed at 1280x720 so it has a size) and answers with its
# connector; GET /unplug removes the newest. mutter reports them like any monitor: MonitorsChanged,
# and gone from the layout when their session stops (measured 2026-10-01, mutter 50).
# spec: docs/15-testing-strategy.md#the-desktop-test-lab
import subprocess
import urllib.parse
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


def copy(text):
    # The robot copies (M3 3.5): wl-copy stays behind to serve the selection, detached from us.
    subprocess.Popen(["wl-copy", "--", text], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
    return "200 copied"


def paste():
    # The robot pastes: what an application asking for text/plain gets.
    r = subprocess.run(["wl-paste", "--no-newline", "--type", "text/plain"], capture_output=True, timeout=5)
    return ("200 " if r.returncode == 0 else "500 ") + (r.stdout if r.returncode == 0 else r.stderr).decode(errors="replace")


def test_png():
    # A real PNG of ~1.2 MiB: noise does not compress, so it is above the 1 MiB text limit and
    # several blob chunks long (docs/08: images up to 8 MiB). Seeded, so every run is the same.
    import random, struct, zlib
    w, h = 700, 600
    rnd = random.Random(42)
    raw = b"".join(b"\x00" + rnd.randbytes(w * 3) for _ in range(h))
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 1)) + chunk(b"IEND", b"")


def digest(data):
    import hashlib
    return f"{len(data)} {hashlib.sha256(data).hexdigest()}"


def copy_image():
    # The robot copies an image as image/png only, as an image viewer does.
    data = test_png()
    path = "/run/desktop/test.png"
    with open(path, "wb") as f:
        f.write(data)
    subprocess.Popen(["wl-copy", "--type", "image/png"], stdin=open(path, "rb"), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
    return "200 " + digest(data)


def paste_image(size=False):
    # The robot pastes an image: what an application asking for image/png gets, as "<length> <sha256>",
    # or with size=1 as "<width>x<height>" from its PNG header (a browser re-encodes what it pastes).
    import struct
    r = subprocess.run(["wl-paste", "--type", "image/png"], capture_output=True, timeout=10)
    if r.returncode != 0:
        return "500 " + r.stderr.decode(errors="replace")
    if size:
        if r.stdout[:8] != b"\x89PNG\r\n\x1a\n" or len(r.stdout) < 24:
            return "200 not a png"
        w, h = struct.unpack(">II", r.stdout[16:24])
        return f"200 {w}x{h}"
    return "200 " + digest(r.stdout)


def session_end():
    # The desktop session goes, as when GNOME Shell crashed on an unplug (mini-PC, 2026-10-04): to
    # the agent, its helper leaves (docs/08 `desktop: absent`).
    r = subprocess.run(["pkill", "-f", "fjarr-desktop-session"], capture_output=True)
    return "200 ended" if r.returncode == 0 else "404 no helper was running"


def session_start():
    # It comes back: the helper starts again, as the user unit does in a new desktop session.
    import os
    if not os.path.isdir("/run/fjarr"):
        return "404 no agent socket directory shared in"
    log = open("/run/desktop/helper-live.log", "ab")
    env = dict(os.environ, FJARR_DESKTOP_SOCK="/run/fjarr/desktop.sock")
    subprocess.Popen(["fjarr-desktop-session"], env=env, stdin=subprocess.DEVNULL, stdout=log, stderr=log, start_new_session=True)
    return "200 started"


def on_request(server, _cond):
    conn, _ = server.accept()
    try:
        line = conn.recv(4096).decode(errors="replace").split("\r\n")[0]
        target = line.split(" ")[1] if len(line.split(" ")) > 1 else ""
        path, _, query = target.partition("?")
        if path == "/plug":
            result = plug()
        elif path == "/unplug":
            result = unplug()
        elif path == "/copy":
            result = copy(urllib.parse.parse_qs(query).get("text", [""])[0])
        elif path == "/paste":
            result = paste()
        elif path == "/session-end":
            result = session_end()
        elif path == "/session-start":
            result = session_start()
        elif path == "/copy-image":
            result = copy_image()
        elif path == "/paste-image":
            result = paste_image(urllib.parse.parse_qs(query).get("size", ["0"])[0] == "1")
        else:
            result = "404 /plug, /unplug, /copy?text=, /paste, /copy-image, /paste-image, /session-end or /session-start"
        code, body = result.split(" ", 1)
        data = body.encode()
        conn.sendall(f"HTTP/1.0 {code} X\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: {len(data)}\r\n\r\n".encode() + data)
    finally:
        conn.close()
    return True


server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind(("0.0.0.0", PORT))
server.listen(4)
GLib.io_add_watch(server.fileno(), GLib.IO_IN, lambda *_: on_request(server, None))
print(f"plugd: monitor hot-plug and the clipboard on :{PORT} (/plug, /unplug, /copy?text=, /paste, /copy-image, /paste-image)", flush=True)
GLib.MainLoop().run()
