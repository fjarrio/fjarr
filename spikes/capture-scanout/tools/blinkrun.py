#!/usr/bin/env python3
"""blinkrun.py NAME [options]   (root, on the machine under test, with a desktop session logged in)

One run of the capture-scanout blink experiments. Starts the KMS and CRC monitors (kmsmon.py,
crcmon.py), then the blink window (blinkwin.py) as the desktop user, then each capture client at a set time AFTER a given blink of the window, so a
client starts while the screen is still (between blinks) and, with a fullscreen window, while it
is scanned out. Every process it starts is a direct child; at the end it touches the run's stop
file (every instrument polls it) and terminates any child still running by its own pid; it never
kills by name. Everything lands in RUNS/NAME: orch.log (what started when, and the primary plane's
state at that moment), testwin.log, kms.log, crc.log, one log per client, and the desktop user's
and fjarr-agent's journal for the run. blinktable.py and cputable.py read it.

Before a run: the machine must show no notification banner, menu or overview (any of them stops
direct scanout while it is up), and an idle session keeps a banner up until there is input, so
move the pointer briefly first (pointer.py nudge). Check kms.log: a run where the primary plane
never shows a client XR24/AR24 framebuffer did not test scanout.

  --runs DIR              where run directories go (default ./runs)
  --user NAME             the desktop session's user (default $DESKTOP_USER or desktop)
  --connector NAME        mutter connector the clients record (default HDMI-1)
  --plane NAME            the CRTC's primary plane in debugfs (default plane-3)

  --blink-ms N            window toggles magenta/green every N ms (default 3000)
  --cursor NAME           FJARR_TESTWIN_CURSOR (none hides the pointer over the window)
  --cursor-cycle A,B,..   FJARR_TESTWIN_CURSOR_CYCLE: the window cycles its cursor through these names
  --cursor-cycle-ms N     ... every N ms
  --mode M                fullscreen (default) | maximize
  --client L:KIND@B+S     start client L of KIND S seconds after blink B (repeatable)
        KIND: capdma (capclient metadata, DMA-BUF, GL download) | capshm-meta | capshm-emb
              probe-dma | probe-dma-sync | probe-lin (pwprobe on a castsession of its own, metadata)
              probe-shm (pwprobe, SHM) | probe-lin-copy | probe-shm-copy (pwprobe --copy)
              gst-MODE (gstcpu.py MODE on a castsession of its own; gst-none is the baseline)
        L:KIND@B+S/D runs the client for D seconds (default 300)
  --nudge @B+S:SECONDS:STEP:PERIOD_MS   move the pointer (pointer.py square) from S s after blink B
  --pwdump @B+S           take a pw-dump snapshot then
  --end-blink K           stop 1 s after blink K (default 12)
  --kms-ms N              KMS sampling interval (default 20)
  --blink-count N         the window stops blinking after N toggles (then holds still)
  --end-after S           stop S s after the start instead of after --end-blink
  --gst-warmup S, --gst-window S   gstcpu.py's warm-up and measurement window (default 5, 20)

pwprobe must be built next to this script (see pwprobe.c). Example, a DMA-BUF client started 1.5 s
after blink 3 while the pointer is hidden, and an SHM control after blink 6:
  sudo python3 blinkrun.py q1 --cursor none --client A:capdma@3+1.5 --client C:capshm-emb@6+1.5
"""
import argparse
import os
import pwd
import signal
import subprocess
import sys
import time

TOOLS = os.path.dirname(os.path.abspath(__file__))
MODS = "0x0200000000401a01,0x0200000000401901,0x0200000000401601,0x0200000000000a01,0x0200000000000901,0x0"

ap = argparse.ArgumentParser()
ap.add_argument("name")
ap.add_argument("--blink-ms", type=int, default=3000)
ap.add_argument("--cursor", default="")
ap.add_argument("--cursor-cycle", default="")
ap.add_argument("--cursor-cycle-ms", type=int, default=0)
ap.add_argument("--mode", default="fullscreen")
ap.add_argument("--client", action="append", default=[])
ap.add_argument("--nudge", action="append", default=[])
ap.add_argument("--pwdump", action="append", default=[])
ap.add_argument("--end-blink", type=int, default=12)
ap.add_argument("--kms-ms", type=int, default=20)
ap.add_argument("--end-after", type=float, default=0.0)
ap.add_argument("--blink-count", type=int, default=0)
ap.add_argument("--gst-warmup", type=float, default=5.0)
ap.add_argument("--gst-window", type=float, default=20.0)
ap.add_argument("--runs", default="runs")
ap.add_argument("--user", default=os.environ.get("DESKTOP_USER", "desktop"))
ap.add_argument("--connector", default="HDMI-1")
ap.add_argument("--plane", default="plane-3")
a = ap.parse_args()

RUNS = os.path.abspath(a.runs)
RUN = f"{RUNS}/{a.name}"
if os.path.exists(RUN):
    sys.exit(f"run {a.name} exists")
os.makedirs(RUN)
os.chmod(RUNS, 0o755)
os.chmod(RUN, 0o777)
STOP = f"{RUN}/stop"
ORCH = open(f"{RUN}/orch.log", "a", buffering=1)


def log(line):
    ORCH.write(f"{time.time():.3f} {line}\n")


pw = pwd.getpwnam(a.user)
UD = pw.pw_uid
os.makedirs(f"{RUNS}/cache", exist_ok=True)
os.chown(f"{RUNS}/cache", pw.pw_uid, pw.pw_gid)
DENV = {  # the user's session; caches go to RUNS/cache, not to the user's home
    "XDG_RUNTIME_DIR": f"/run/user/{UD}", "WAYLAND_DISPLAY": "wayland-0",
    "DBUS_SESSION_BUS_ADDRESS": f"unix:path=/run/user/{UD}/bus", "XDG_CACHE_HOME": f"{RUNS}/cache",
    "HOME": pw.pw_dir, "USER": a.user, "LOGNAME": a.user, "PATH": "/usr/local/bin:/usr/bin:/bin", "LANG": "C.UTF-8",
}
children = []  # (label, Popen)


def spawn_desktop(label, argv, env_extra=None, out=None):
    env = dict(DENV)
    env.update(env_extra or {})
    f = open(out or f"{RUN}/{label}.out", "ab")
    p = subprocess.Popen(argv, env=env, user=pw.pw_uid, group=pw.pw_gid, extra_groups=os.getgrouplist(a.user, pw.pw_gid),
                         stdin=subprocess.DEVNULL, stdout=f, stderr=subprocess.STDOUT, cwd=RUN, start_new_session=True)
    children.append((label, p))
    log(f"spawn {label} pid={p.pid} {' '.join(argv)} env+={env_extra or {}}")
    return p


def spawn_root(label, argv):
    f = open(f"{RUN}/{label}.out", "ab")
    p = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=f, stderr=subprocess.STDOUT, cwd=RUN, start_new_session=True)
    children.append((label, p))
    log(f"spawn {label} pid={p.pid} {' '.join(argv)}")
    return p


def pid_of(comm, uid):
    r = subprocess.run(["ps", "-C", comm, "-o", "pid=,uid="], capture_output=True, text=True)
    for ln in r.stdout.split("\n"):
        f = ln.split()
        if len(f) == 2 and int(f[1]) == uid:
            return f[0]
    return "0"


def last_plane():
    try:
        with open(f"{RUN}/kms.log") as f:
            lines = [ln for ln in f if f" {a.plane} " in ln]
        return lines[-1].strip() if lines else "none"
    except OSError:
        return "none"


def parse_at(spec):  # "B+S" -> (B, S)
    b, s = spec.split("+")
    return int(b), float(s)


def start_client(label, kind, dur="300"):
    scan = last_plane()
    log(f"client {label} kind={kind} duration={dur} starting; last {a.plane}: {scan}")
    if kind == "capdma":
        spawn_desktop(f"cap-{label}", ["python3", f"{TOOLS}/capclient.py", "metadata", dur, f"{RUN}/cap-{label}.log", a.connector],
                      {"CAP_DMABUF": "1"})
    elif kind == "capshm-meta":
        spawn_desktop(f"cap-{label}", ["python3", f"{TOOLS}/capclient.py", "metadata", dur, f"{RUN}/cap-{label}.log", a.connector])
    elif kind == "capshm-emb":
        spawn_desktop(f"cap-{label}", ["python3", f"{TOOLS}/capclient.py", "embedded", dur, f"{RUN}/cap-{label}.log", a.connector])
    elif kind.startswith("gst-"):
        gmode = kind[4:]
        node = "0"
        if gmode != "none":
            cast = f"{RUN}/cast-{label}.log"
            spawn_desktop(f"cast-{label}", ["python3", f"{TOOLS}/castsession.py", "metadata", dur, cast, a.connector])
            for _ in range(100):
                if os.path.exists(cast + ".node") and os.path.getsize(cast + ".node") > 0:
                    break
                time.sleep(0.02)
            node = open(cast + ".node").read().strip()
        genv = {"GS_PID": pid_of("gnome-shell", UD), "PW_PID": pid_of("pipewire", UD), "WIN_PID": str(win.pid)}
        if os.environ.get("GSTCPU_DEBUG"):
            genv["GSTCPU_DEBUG"] = os.environ["GSTCPU_DEBUG"]
        spawn_desktop(f"gst-{label}", ["python3", f"{TOOLS}/gstcpu.py", gmode, node, str(a.gst_warmup), str(a.gst_window),
                                       f"{RUN}/gst-{label}"], genv)
    elif kind.startswith("probe-"):
        cast = f"{RUN}/cast-{label}.log"
        spawn_desktop(f"cast-{label}", ["python3", f"{TOOLS}/castsession.py", "metadata", dur, cast, a.connector])
        for _ in range(100):
            if os.path.exists(cast + ".node") and os.path.getsize(cast + ".node") > 0:
                break
            time.sleep(0.02)
        node = open(cast + ".node").read().strip()
        extra = {"probe-dma": ["--dmabuf", MODS], "probe-dma-sync": ["--dmabuf", MODS, "--sync"],
                 "probe-lin": ["--dmabuf", "0x0", "--sample"], "probe-shm": ["--sample"],
                 "probe-lin-copy": ["--dmabuf", "0x0", "--copy"], "probe-shm-copy": ["--copy"]}[kind]
        bdir = f"{RUN}/bitmaps-{label}"
        os.makedirs(bdir, exist_ok=True)
        os.chmod(bdir, 0o777)
        spawn_desktop(f"probe-{label}", [f"{TOOLS}/pwprobe", node, dur, f"{RUN}/probe-{label}.log"] + extra + ["--bitmaps", bdir],
                      {"PWPROBE_STOP": STOP})
    else:
        log(f"unknown kind {kind}")


T0 = time.time()
open(f"{RUN}/t0", "w").write(f"{T0:.3f}\n")
open(f"{RUN}/args", "w").write(" ".join(sys.argv) + "\n")
log(f"start {' '.join(sys.argv)}")

spawn_root("kmsmon", ["python3", f"{TOOLS}/kmsmon.py", str(a.kms_ms), "600", f"{RUN}/kms.log"])
spawn_root("crcmon", ["python3", f"{TOOLS}/crcmon.py", "600", f"{RUN}/crc.log"])
time.sleep(1.0)

wenv = {"GDK_BACKEND": "wayland", "FJARR_FIXTURE_LOG": f"{RUN}/testwin.log", "FJARR_TESTWIN_BLINK_MS": str(a.blink_ms),
        "FJARR_TESTWIN_MODE": a.mode}
if a.cursor:
    wenv["FJARR_TESTWIN_CURSOR"] = a.cursor
if a.blink_count:
    wenv["FJARR_TESTWIN_BLINK_COUNT"] = str(a.blink_count)
if a.cursor_cycle:
    wenv["FJARR_TESTWIN_CURSOR_CYCLE"] = a.cursor_cycle
    wenv["FJARR_TESTWIN_CURSOR_CYCLE_MS"] = str(a.cursor_cycle_ms)
win = spawn_desktop("testwin", ["python3", f"{TOOLS}/blinkwin.py"], wenv)

plan = []  # (blink, offset, action, args)
for c in a.client:
    label, rest = c.split(":", 1)
    kind, at = rest.split("@")
    dur = "300"
    if "/" in at:
        at, dur = at.split("/")
    b, s = parse_at(at)
    plan.append([b, s, "client", (label, kind, dur), False])
for n in a.nudge:
    at, secs, step, period = n.lstrip("@").split(":")
    b, s = parse_at(at)
    plan.append([b, s, "nudge", (secs, step, period), False])
for d in a.pwdump:
    b, s = parse_at(d.lstrip("@"))
    plan.append([b, s, "pwdump", (), False])

blinks = []  # epoch times, index 1..n
seen = 0
end_at = None
deadline = T0 + 30 + max(a.blink_ms / 1000.0 * (a.end_blink + 2), a.end_after)
try:
    while True:
        now = time.time()
        if now > deadline:
            log("deadline reached")
            break
        try:
            with open(f"{RUN}/testwin.log") as f:
                lines = f.readlines()
        except OSError:
            lines = []
        for ln in lines[seen:]:
            parts = ln.split()
            if len(parts) >= 2 and parts[1] == "blink":
                blinks.append(float(parts[0]))
                if a.blink_ms >= 500:
                    log(f"blink {len(blinks)} {parts[2]} at {parts[0]}; last {a.plane}: {last_plane()}")
        seen = len(lines)
        if win.poll() is not None:
            log(f"window exited rc={win.returncode}")
            break
        for item in plan:
            b, s, action, args, done = item
            if done or len(blinks) < b:
                continue
            if now >= blinks[b - 1] + s:
                item[4] = True
                if action == "client":
                    start_client(*args)
                elif action == "nudge":
                    secs, step, period = args
                    spawn_desktop("nudge", ["python3", f"{TOOLS}/pointer.py", "square", secs, step, period, f"{RUN}/nudge.log"])
                elif action == "pwdump":
                    with open(f"{RUN}/pwdump-{b}.json", "wb") as f:
                        subprocess.run(["runuser", "-u", a.user, "--", "env", f"XDG_RUNTIME_DIR=/run/user/{UD}", "pw-dump"], stdout=f,
                                       stderr=subprocess.DEVNULL, timeout=10)
                    log(f"pw-dump taken after blink {b}")
        if end_at is None and a.end_after > 0:
            end_at = T0 + 1.0 + a.end_after
        if end_at is None and len(blinks) >= a.end_blink:
            end_at = blinks[a.end_blink - 1] + 1.0
        if end_at is not None and now >= end_at:
            break
        time.sleep(0.02)
finally:
    log("stopping")
    open(STOP, "w").close()
    time.sleep(1.5)
    for label, p in children:
        if label == "testwin" and p.poll() is None:
            os.kill(p.pid, signal.SIGTERM)
            log(f"SIGTERM testwin pid={p.pid}")
    time.sleep(1.0)
    for label, p in children:
        if p.poll() is None:
            os.kill(p.pid, signal.SIGTERM)
            log(f"SIGTERM {label} pid={p.pid}")
    for label, p in children:
        try:
            p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            os.kill(p.pid, signal.SIGKILL)
            log(f"SIGKILL {label} pid={p.pid}")
            p.wait()
        log(f"exit {label} pid={p.pid} rc={p.returncode}")
    since = f"@{int(T0)}"
    with open(f"{RUN}/desktop-user.journal", "wb") as f:
        subprocess.run(["journalctl", f"_UID={UD}", "--since", since, "-o", "short-precise", "--no-pager"], stdout=f, stderr=subprocess.STDOUT)
    with open(f"{RUN}/agent.journal", "wb") as f:
        subprocess.run(["journalctl", "-u", "fjarr-agent", "--since", since, "-o", "short-precise", "--no-pager"], stdout=f, stderr=subprocess.STDOUT)
    subprocess.run(["chmod", "-R", "a+rX", RUN])
    log("done")
