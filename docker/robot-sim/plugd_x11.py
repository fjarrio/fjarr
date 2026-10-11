#!/usr/bin/env python3
# Monitor hot-plug and the clipboard for the X11 fixture (docs/07#simulating-hot-plug, M3 3.7):
# GET /plug carves a 1280x720 RandR monitor right of the first, GET /unplug removes it. Xvfb's root
# cannot grow, so the fixture starts with a 3840x1080 root and monitors are carved out of it.
# /copy?text=, /paste, /copy-image and /paste-image[?size=1] are the robot's applications copying
# and pasting, through xclip, answered exactly as the GNOME fixture's plugd answers them (3.7c).
# /copy-stall and /copy-big are X11-only failure modes: an owner that never answers, text over the limit.
# /play, /speakers?use= and /listening are the kiosk's sound (M3.5 3.5.2, ADR-0034), answered as the
# GNOME fixture's plugd answers them, in the kiosk user's own PipeWire.
import hashlib
import json
import math
import random
import struct
import subprocess
import threading
import time
import urllib.parse
import zlib
from http.server import BaseHTTPRequestHandler, HTTPServer

NAME = "RIGHT"
# The kiosk user's session, where its PipeWire runs (start.sh): sound is played and inspected as it.
KIOSK = ["setpriv", "--reuid=kiosk", "--regid=kiosk", "--init-groups", "env", "XDG_RUNTIME_DIR=/run/kiosk", "HOME=/home/kiosk",
         "DBUS_SESSION_BUS_ADDRESS=unix:path=/run/kiosk/bus"]
player = None  # the pw-play now playing on the kiosk


def kiosk(*args):
    return subprocess.run([*KIOSK, *args], capture_output=True, text=True, timeout=5)


def nodes():
    return json.loads(kiosk("pw-dump").stdout or "[]")


def play(freq, seconds):
    # An application of the kiosk plays a sine to its default output (pw-play, as on GNOME).
    global player
    if seconds == 0:  # stop: a scenario starts from silence, whatever an earlier run left playing
        if player and player.poll() is None:
            player.kill()
            player.wait()
        return 200, "quiet"
    if not 50 <= freq <= 8000 or not 0 < seconds <= 60:
        return 400, "freq 50-8000, seconds up to 60"
    import wave
    path = f"/tmp/tone-{freq}-{seconds}.wav"
    w = wave.open(path, "wb")
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(48000)
    w.writeframes(b"".join(struct.pack("<h", int(12000 * math.sin(2 * math.pi * freq * i / 48000))) for i in range(int(48000 * seconds))))
    w.close()
    if player and player.poll() is None:
        player.kill()
    player = subprocess.Popen([*KIOSK, "pw-play", path], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return 200, f"playing {freq} Hz for {seconds} s"


def speakers(name):
    # The default output changes: a null sink named `name`, made if needed, becomes the default.
    if not name.replace("-", "").isalnum():
        return 400, "a name of letters, digits and dashes"
    find = lambda: [o["id"] for o in nodes() if o.get("info", {}).get("props", {}).get("node.name") == name]
    ids = find()
    if not ids:
        kiosk("pw-cli", "create-node", "adapter", f"{{ factory.name=support.null-audio-sink node.name={name} node.description={name} "
              "media.class=Audio/Sink object.linger=true audio.position=[FL FR] }")
        for _ in range(50):
            ids = find()
            if ids:
                break
            time.sleep(0.1)
        if not ids:
            return 500, "the sink did not appear"
    r = kiosk("wpctl", "set-default", str(ids[0]))
    return (200, name) if r.returncode == 0 else (500, r.stderr)


def encoded(result):
    code, body = result
    return code, body.encode()


def listening():
    # Capture streams open on the kiosk's outputs: the agent's audio reader is one (stream.capture.sink).
    n = sum(1 for o in nodes() if o.get("info", {}).get("props", {}).get("stream.capture.sink") in ("true", True))
    return 200, str(n)


def xrandr(*args):
    return subprocess.run(["xrandr", *args], capture_output=True, text=True, timeout=5)


def test_png():
    # The GNOME fixture's PNG, byte for byte (same seed): ~1.2 MiB of noise, so it is above the 1 MiB
    # text limit, several blob chunks long, and above the 256 KiB INCR threshold both ways.
    w, h = 700, 600
    rnd = random.Random(42)
    raw = b"".join(b"\x00" + rnd.randbytes(w * 3) for _ in range(h))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 1)) + chunk(b"IEND", b"")


def digest(data):
    return f"{len(data)} {hashlib.sha256(data).hexdigest()}"


def copy(data, target):
    # xclip forks and serves the selection until another client owns it, as a real application does.
    p = subprocess.Popen(["xclip", "-selection", "clipboard", "-target", target, "-in"], stdin=subprocess.PIPE,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
    p.communicate(data, timeout=5)


def paste(target):
    try:
        return subprocess.run(["xclip", "-selection", "clipboard", "-target", target, "-out"], capture_output=True, timeout=10)
    except subprocess.TimeoutExpired as e:
        return subprocess.CompletedProcess(e.cmd, 124, e.stdout or b"", f"xclip -out timed out after 10 s with {len(e.stdout or b'')} bytes".encode())


def stall_owner():
    # A misbehaving application (3.7c's failure mode): it owns CLIPBOARD and offers text through
    # TARGETS, but never hands the text over. It gives up when another client copies, or after 60 s.
    from Xlib import X, Xatom, display
    from Xlib.protocol import event

    d = display.Display()
    w = d.screen().root.create_window(-10, -10, 1, 1, 0, X.CopyFromParent)
    clip, targets, utf8 = (d.intern_atom(n) for n in ("CLIPBOARD", "TARGETS", "UTF8_STRING"))
    w.set_selection_owner(clip, X.CurrentTime)
    d.flush()
    deadline = time.time() + 60
    while time.time() < deadline:
        while d.pending_events():
            e = d.next_event()
            if e.type == X.SelectionClear:
                return
            if e.type == X.SelectionRequest and e.target == targets:
                e.requestor.change_property(e.property, Xatom.ATOM, 32, [targets, utf8])
                e.requestor.send_event(event.SelectionNotify(time=e.time, requestor=e.requestor, selection=e.selection,
                                                             target=e.target, property=e.property))
                d.flush()
        time.sleep(0.02)


def clipboard(path, query):
    if path == "/copy-stall":
        threading.Thread(target=stall_owner, daemon=True).start()
        return 200, b"owning the clipboard without answering"
    if path == "/copy-big":
        copy(b"x" * (1024 * 1024 + 1), "UTF8_STRING")
        return 200, b"copied 1048577 bytes of text"
    if path == "/copy":
        copy(query.get("text", [""])[0].encode(), "UTF8_STRING")
        return 200, b"copied"
    if path == "/paste":
        r = paste("UTF8_STRING")
        return (200, r.stdout) if r.returncode == 0 else (500, r.stderr)
    if path == "/copy-image":
        data = test_png()
        copy(data, "image/png")
        return 200, digest(data).encode()
    if path == "/paste-image":
        r = paste("image/png")
        if r.returncode != 0:
            return 500, r.stderr
        if query.get("size", ["0"])[0] == "1":
            if r.stdout[:8] != b"\x89PNG\r\n\x1a\n" or len(r.stdout) < 24:
                return 200, b"not a png"
            w, h = struct.unpack(">II", r.stdout[16:24])
            return 200, f"{w}x{h}".encode()
        return 200, digest(r.stdout).encode()
    return None


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        path, _, q = self.path.partition("?")
        answered = clipboard(path, urllib.parse.parse_qs(q))
        if answered:
            return self.reply(*answered)
        sound_q = urllib.parse.parse_qs(q)
        if path == "/play":
            return self.reply(*encoded(play(int(sound_q.get("freq", ["440"])[0]), float(sound_q.get("seconds", ["5"])[0]))))
        if path == "/speakers":
            return self.reply(*encoded(speakers(sound_q.get("use", [""])[0])))
        if path == "/listening":
            return self.reply(*encoded(listening()))
        if self.path == "/plug":
            r = xrandr("--setmonitor", NAME, "1280/340x720/190+1920+0", "none")
            code, body = (200, NAME) if r.returncode == 0 else (500, r.stderr)
        elif self.path == "/move":
            # Same size, new place, in one xrandr run: the layout shift a kiosk makes after an
            # unplug (the mini-PC's ghost moved into the gap, 2026-10-05).
            r = xrandr("--delmonitor", NAME, "--setmonitor", NAME, "1280/340x720/190+2560+0", "none")
            code, body = (200, NAME) if r.returncode == 0 else (500, r.stderr)
        elif self.path in ("/mode", "/mode?restore=1"):
            # A mode change in place: LEFT, the test window's monitor, to 1600x900 and back
            # (docs/06: "a mode change keeps input coordinates correct").
            geo = "1920/500x1080/280+0+0" if self.path.endswith("restore=1") else "1600/420x900/236+0+0"
            # Delete and add in one run: Xvfb refuses --setmonitor on a name it already has (BadValue).
            r = xrandr("--delmonitor", "LEFT", "--setmonitor", "LEFT", geo, "screen")
            code, body = (200, geo) if r.returncode == 0 else (500, r.stderr)
        elif self.path == "/unplug":
            r = xrandr("--delmonitor", NAME)
            code, body = (200, NAME) if r.returncode == 0 else (404, r.stderr or "nothing plugged")
        elif self.path == "/":
            code, body = 200, "plugd: /plug, /move, /unplug, /copy?text=, /paste, /copy-image, /paste-image, /play, /speakers?use=, /listening"
        else:
            code, body = 404, "/plug, /unplug, /copy?text=, /paste, /copy-image or /paste-image"
        self.reply(code, body.encode())

    def reply(self, code, data):
        self.send_response(code)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *args):
        pass


HTTPServer(("0.0.0.0", 8091), Handler).serve_forever()
