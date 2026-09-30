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


def send(conn, body):
    conn.sendall(json.dumps(body).encode())


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

        mons, _ = recv(conn)
        monitors = mons.get("monitors", [])
        primary = next((m for m in monitors if m.get("primary")), None)
        check("monitors", mons.get("type") == "monitors" and primary is not None and primary["width"] > 0,
              f"{len(monitors)} monitor(s): " + ", ".join(f"{m['connector']} {m['width']}x{m['height']}{' primary' if m['primary'] else ''} {m['kind']}" for m in monitors))

        send(conn, {"type": "start-capture", "id": 1, "connector": primary["connector"] if primary else "", "cursor": "embedded"})
        started, fds = recv(conn)
        ok = started.get("type") == "capture-started" and len(fds) == 1
        check("capture-started", ok, f"{json.dumps(started)} with {len(fds)} descriptor(s)")
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

        send(conn, {"type": "open-input"})
        opened, eis_fds = recv(conn)
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
