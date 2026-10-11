#!/usr/bin/env python3
# The session helper's handover, end to end, with this script playing backend module E's side of
# fjarr-desktop-1 (docs/23#desktop-helper-protocol): it listens where the agent would, checks the
# helper's uid (SO_PEERCRED), answers hello, and then captures a frame through `pipewiresrc fd=` on
# the descriptor the helper handed it and injects through the EIS socket it handed it — exactly what
# the agent does with them. One PASS/FAIL line per check; exit status 1 on any failure.
#   fixture-helper-check <path to fjarr-desktop-session>
import json
import os
import socket
import struct
import subprocess
import sys
import time

import gi

gi.require_version("Gst", "1.0")
from gi.repository import Gst  # noqa: E402

SOCK = os.environ.get("FJARR_DESKTOP_SOCK", "/run/desktop/agent.sock")
LOG = os.environ.get("FJARR_FIXTURE_LOG", "/run/desktop/testwin.log")
failed = False


def check(name, ok, detail):
    global failed
    failed |= not ok
    print(f"{'PASS' if ok else 'FAIL'} {name}: {detail}", flush=True)


def recv(conn, timeout=10):
    conn.settimeout(timeout)
    data, fds, _flags, _addr = socket.recv_fds(conn, 65536, 4)
    if not data:
        raise EOFError("the helper hung up")
    return json.loads(data), fds


seen_types = {}  # messages the helper sent unasked while a check waited for another, latest of each type


def recv_type(conn, want, timeout=10):
    """The next message of type `want`; others (the helper's own reports, such as `speakers`) are kept
    in seen_types and their descriptors closed."""
    deadline = time.time() + timeout
    while True:
        msg, fds = recv(conn, max(0.1, deadline - time.time()))
        if msg.get("type") == want:
            return msg, fds
        seen_types[msg.get("type")] = msg
        for fd in fds:
            os.close(fd)


def send(conn, body):
    conn.sendall(json.dumps(body).encode())


# A microphone that really produces sound: a live Audio/Source node in the desktop user's PipeWire.
MIC = ("import gi; gi.require_version('Gst','1.0'); from gi.repository import Gst, GLib; Gst.init(None);"
       "p=Gst.parse_launch('audiotestsrc is-live=true ! audio/x-raw,rate=48000,channels=2 ! pipewiresink mode=provide "
       "stream-properties=\"props,media.class=Audio/Source,node.name=fixture-mic\"'); p.set_state(Gst.State.PLAYING); GLib.MainLoop().run()")
# Capture fixture-mic through the PipeWire connection on fd $PW_FD; exit 0 when audio arrives.
GRAB_MIC = ("import gi,sys; gi.require_version('Gst','1.0'); from gi.repository import Gst; Gst.init(None);"
            "p=Gst.parse_launch('pipewiresrc fd=' + os.environ['PW_FD'] + ' target-object=fixture-mic ! audioconvert ! audio/x-raw ! appsink name=s max-buffers=1 drop=true sync=false');"
            "p.set_state(Gst.State.PLAYING); s=p.get_by_name('s').emit('try-pull-sample', 4*Gst.SECOND); print('audio' if s else 'nothing', flush=True); os._exit(0 if s else 1)")


# Capture the speakers' monitor (what the desktop plays) through fd $PW_FD; exit 0 when sound, not silence, arrives.
GRAB_SPEAKERS = ("import gi,sys,struct; gi.require_version('Gst','1.0'); from gi.repository import Gst; Gst.init(None);"
                 "p=Gst.parse_launch('pipewiresrc fd=' + os.environ['PW_FD'] + ' target-object=fixture-speakers stream-properties=props,stream.capture.sink=true ! "
                 "audioconvert ! audio/x-raw,format=S16LE,channels=1 ! appsink name=s sync=false');"
                 "p.set_state(Gst.State.PLAYING); loud=False\n"
                 "for _ in range(40):\n"
                 "  smp=p.get_by_name('s').emit('try-pull-sample', Gst.SECOND//4)\n"
                 "  if smp:\n"
                 "    b=smp.get_buffer().extract_dup(0, smp.get_buffer().get_size()); v=struct.unpack('<%dh' % (len(b)//2), b[:len(b)//2*2])\n"
                 "    if v and max(abs(x) for x in v) > 3000: loud=True; break\n"
                 "print('sound' if loud else 'nothing', flush=True); os._exit(0 if loud else 1)")


last_grab = ""  # what the last grab printed, for a failure's detail


def grab_mic(fd, code=None):
    """True when audio came through `fd` (a PipeWire connection, consumed). A refused capture hangs
    rather than erroring (spikes/pipewire-narrowing), so it runs in its own process with a deadline.
    The descriptor keeps its number: one the helper handed over may well be 3 already."""
    global last_grab
    try:
        r = subprocess.run(["python3", "-c", "import os;" + (code or GRAB_MIC)], pass_fds=(fd,), env=dict(os.environ, PW_FD=str(fd)),
                           capture_output=True, text=True, timeout=14)
        last_grab = (r.stdout + r.stderr).strip()[-400:]
        return r.returncode == 0
    except subprocess.TimeoutExpired:
        last_grab = "hung"
        return False
    finally:
        os.close(fd)


def raw_pipewire():
    """A fresh, unnarrowed connection to this user's PipeWire: what the helper must never hand over."""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(os.path.join(os.environ.get("XDG_RUNTIME_DIR", "/run/desktop"), "pipewire-0"))
    return s.detach()


def main():
    helper_bin = sys.argv[1]
    try:
        os.unlink(SOCK)
    except FileNotFoundError:
        pass
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    srv.bind(SOCK)
    srv.listen(1)
    env = dict(os.environ, FJARR_DESKTOP_SOCK=SOCK)
    helper = subprocess.Popen([helper_bin], env=env, stderr=open("/run/desktop/helper.log", "w"))
    try:
        srv.settimeout(15)
        conn, _ = srv.accept()
        cred = conn.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, struct.calcsize("3i"))
        _pid, uid, _gid = struct.unpack("3i", cred)
        check("peer uid", uid == os.getuid(), f"SO_PEERCRED says uid {uid}")

        hello, _ = recv(conn)
        check("hello", hello.get("type") == "hello" and hello.get("protocol") == "fjarr-desktop-1", json.dumps(hello))
        send(conn, {"type": "welcome", "protocol": "fjarr-desktop-1"})

        mons, _ = recv_type(conn, "monitors")
        monitors = mons.get("monitors", [])
        primary = next((m for m in monitors if m.get("primary")), None)
        check("monitors", mons.get("type") == "monitors" and primary is not None and primary["width"] > 0,
              f"{len(monitors)} monitor(s): " + ", ".join(f"{m['connector']} {m['width']}x{m['height']}{' primary' if m['primary'] else ''} {m['kind']}" for m in monitors))

        send(conn, {"type": "start-capture", "id": 1, "connector": primary["connector"] if primary else "", "cursor": "embedded"})
        started, fds = recv_type(conn, "capture-started")
        ok = started.get("type") == "capture-started" and len(fds) == 1
        check("capture-started", ok, f"{json.dumps(started)} with {len(fds)} descriptor(s)")
        mic_probe = os.dup(fds[0]) if ok else -1  # the same connection, for the narrowing check below
        if ok:
            Gst.init(None)
            pipe = Gst.parse_launch(
                f"pipewiresrc fd={fds[0]} path={started['node']} keepalive-time=500 always-copy=true ! videoconvert ! "
                "video/x-raw,format=RGB ! appsink name=sink max-buffers=1 drop=true sync=false")
            pipe.set_state(Gst.State.PLAYING)
            sample = pipe.get_by_name("sink").emit("try-pull-sample", 10 * Gst.SECOND)
            detail = "no frame within 10 s"
            good = False
            if sample:
                st = sample.get_caps().get_structure(0)
                w, h = st.get_value("width"), st.get_value("height")
                _, info = sample.get_buffer().map(Gst.MapFlags.READ)
                data, stride = info.data, len(info.data) // h
                hits = total = 0
                for y in range(0, h, 16):
                    for x in range(0, w, 16):
                        r, g, b = data[y * stride + 3 * x: y * stride + 3 * x + 3]
                        total += 1
                        hits += r > 200 and g < 60 and b > 200
                good = hits * 10 >= total * 9
                detail = f"{w}x{h} through the handed descriptor, {100 * hits // total}% the window's colour"
            # A still screen sends nothing more; keepalive-time must keep frames coming.
            n = 0
            deadline = time.time() + 2.5
            while time.time() < deadline:
                if pipe.get_by_name("sink").emit("try-pull-sample", Gst.SECOND // 2):
                    n += 1
            check("capture through the handed PipeWire connection", good, detail)
            check("a still screen keeps producing (keepalive)", n >= 3, f"{n} frames in 2.5 s of an unchanging window")
            pipe.set_state(Gst.State.NULL)

            # The account boundary for audio (docs/23#desktop-descriptor-handover): the handed
            # connection reaches the screen and nothing else. The control proves the check can fail.
            mic = subprocess.Popen(["python3", "-c", MIC])
            try:
                time.sleep(2)
                control = grab_mic(raw_pipewire())
                leaked = grab_mic(mic_probe)
                check("the microphone, through an unnarrowed connection (the control)", control, "audio" if control else "no audio: the check below would prove nothing")
                check("the microphone, through the handed connection", control and not leaked,
                      "nothing: the connection is narrowed to the screen" if not leaked else "AUDIO: the handed connection reaches the desktop user's microphone")
            finally:
                mic.kill()

        # The speakers (docs/23#desktop-audio): reported unasked, and handed over narrowed to that sink.
        if "speakers" not in seen_types:
            try:
                seen_types["speakers"], _fds = recv_type(conn, "speakers", timeout=5)
            except TimeoutError:
                pass
        speakers = seen_types.get("speakers", {}).get("sink") or {}
        check("speakers", speakers.get("name") == "fixture-speakers", json.dumps(seen_types.get("speakers")))
        send(conn, {"type": "start-audio", "id": 7})
        sound, sound_fds = recv_type(conn, "audio-started")
        ok = sound.get("id") == 7 and len(sound_fds) == 1
        check("audio-started", ok, f"{json.dumps(sound)} with {len(sound_fds)} descriptor(s)")
        if ok:
            sound_mic_probe = os.dup(sound_fds[0])
            tone = "/run/desktop/check-tone.wav"
            subprocess.run(["python3", "-c", "import wave,struct,math; w=wave.open('%s','wb'); w.setnchannels(1); w.setsampwidth(2); w.setframerate(48000);"
                            "w.writeframes(b''.join(struct.pack('<h', int(12000*math.sin(2*math.pi*440*i/48000))) for i in range(48000*12))); w.close()" % tone], check=True)
            player = subprocess.Popen(["pw-play", tone])
            mic = None
            try:
                time.sleep(1)
                control = grab_mic(raw_pipewire(), GRAB_SPEAKERS)
                check("the desktop's sound, through an unnarrowed connection (the control)", control, "the tone" if control else "nothing: the check below would prove nothing; " + last_grab)
                heard = grab_mic(sound_fds[0], GRAB_SPEAKERS)
                check("the desktop's sound through the handed connection", heard, "the tone the desktop plays" if heard else "nothing within 10 s: " + last_grab)
                mic = subprocess.Popen(["python3", "-c", MIC])
                time.sleep(2)
                leaked = grab_mic(sound_mic_probe)
                check("the microphone, through the sound's connection", not leaked,
                      "nothing: the connection is narrowed to the speakers" if not leaked else "AUDIO: the sound's connection reaches the desktop user's microphone")
            finally:
                player.kill()
                if mic:
                    mic.kill()

        send(conn, {"type": "open-input"})
        opened, eis_fds = recv_type(conn, "input-opened")
        check("input-opened", opened.get("type") == "input-opened" and len(eis_fds) == 1, f"{json.dumps(opened)} with {len(eis_fds)} descriptor(s)")
        if eis_fds:
            sys.path.insert(0, "/usr/local/bin")
            import importlib.util
            spec = importlib.util.spec_from_loader("selftest", importlib.machinery.SourceFileLoader("selftest", "/usr/local/bin/fixture-selftest"))
            selftest = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(selftest)
            start = os.path.getsize(LOG)
            selftest.drive_eis(eis_fds[0])
            with open(LOG) as f:
                f.seek(start)
                seen = f.read()
            for name, needle in [("input through the handed EIS socket: key f", "key f text='f'"),
                                 ("input through the handed EIS socket: click at 321,234", "click button=1 x=321 y=234")]:
                check(name, needle in seen, "in the window's log" if needle in seen else f"not in the window's log: {seen.strip()!r}")

        # The agent going away ends mutter's session; the helper waits to reconnect.
        conn.close()
        time.sleep(1)
        check("the helper outlives the agent", helper.poll() is None, "still running, waiting to reconnect" if helper.poll() is None else f"exited {helper.returncode}")
    except Exception as e:  # noqa: BLE001 — a check that could not finish is a failure with its reason
        check("helper handover", False, f"{type(e).__name__}: {e}")
    finally:
        helper.terminate()
        srv.close()
    print(f"SUMMARY helper-handover: {'failed' if failed else 'passed'}", flush=True)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
