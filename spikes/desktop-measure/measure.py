#!/usr/bin/env python3
# Phase 2 measurements (docs/07 criteria table) for one combination per run, against the stamp
# window (stamp.py / stamp_x11.py):
#   latency  paint→capture: the clock painted into the frame vs the clock when the frame arrives
#   input    input→photon: inject a key, time until a captured frame shows the lower half flipped
#   cpu      1080p30 capture + vah264enc for a while: this process, the compositor/X server and
#            PipeWire, as % of one core, each minus an equal-length idle baseline; plus GPU busy
# Usage: measure.py --combo A|B|C|D|E [--tests latency,input,cpu]. Run as the session user, with
# DISPLAY/XAUTHORITY (A, B) or the session bus (C, D, E); B and D need inputd.py running as root.
# Throwaway spike code.
import argparse, os, random, socket, statistics, subprocess, sys, time
import gi
gi.require_version("Gst", "1.0"); gi.require_version("GstVideo", "1.0")
from gi.repository import GLib, Gst, GstVideo

Gst.init(None)
ap = argparse.ArgumentParser()
ap.add_argument("--combo", required=True, choices="ABCDE")
ap.add_argument("--tests", default="latency,input,cpu")
ap.add_argument("--n", type=int, default=300)
ap.add_argument("--trials", type=int, default=40)
ap.add_argument("--cpu-seconds", type=int, default=20)
ap.add_argument("--token", default=os.path.expanduser("~/.fjarr-portal-token"))
args = ap.parse_args()
combo = args.combo
BITS, BLOCK = 24, 60

def uinput(cmd):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM); s.sendto(cmd, "/run/fjarr-spike-inputd.sock"); s.close()

# ---- per-combination capture source and injector --------------------------------------------
def setup_x11():
    from Xlib import X, XK, display
    from Xlib.ext import xtest
    dpy = display.Display(); space = dpy.keysym_to_keycode(XK.string_to_keysym("space"))
    def key():
        xtest.fake_input(dpy, X.KeyPress, space); xtest.fake_input(dpy, X.KeyRelease, space); dpy.sync()
    def click():
        xtest.fake_input(dpy, X.MotionNotify, x=960, y=700)
        xtest.fake_input(dpy, X.ButtonPress, 1); xtest.fake_input(dpy, X.ButtonRelease, 1); dpy.sync()
    return "ximagesrc", key, click, "Xorg"

def setup_mutter():
    import dbus, dbus.mainloop.glib
    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.SessionBus(); loop = GLib.MainLoop()
    RD, SC = "org.gnome.Mutter.RemoteDesktop", "org.gnome.Mutter.ScreenCast"
    rd_path = dbus.Interface(bus.get_object(RD, "/org/gnome/Mutter/RemoteDesktop"), RD).CreateSession()
    rd_obj = bus.get_object(RD, rd_path); rd = dbus.Interface(rd_obj, RD + ".Session")
    sid = rd_obj.Get(RD + ".Session", "SessionId", dbus_interface=dbus.PROPERTIES_IFACE)
    sc_path = dbus.Interface(bus.get_object(SC, "/org/gnome/Mutter/ScreenCast"), SC).CreateSession({"remote-desktop-session-id": sid})
    stream = dbus.Interface(bus.get_object(SC, sc_path), SC + ".Session").RecordMonitor("", {"cursor-mode": dbus.UInt32(1)})
    node = {}
    bus.add_signal_receiver(lambda n: (node.update(id=int(n)), loop.quit()), "PipeWireStreamAdded", SC + ".Stream", path=stream)
    rd.Start(); GLib.timeout_add_seconds(10, loop.quit); loop.run()
    def key():
        rd.NotifyKeyboardKeysym(dbus.UInt32(0x20), True); rd.NotifyKeyboardKeysym(dbus.UInt32(0x20), False)
    def click():
        rd.NotifyPointerMotionAbsolute(stream, 960.0, 700.0)
        rd.NotifyPointerButton(dbus.Int32(0x110), True); rd.NotifyPointerButton(dbus.Int32(0x110), False)
    return f"pipewiresrc path={node['id']}", key, click, "gnome-shell"

def setup_portal():
    import dbus, dbus.mainloop.glib
    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.SessionBus(); loop = GLib.MainLoop()
    obj = bus.get_object("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop")
    rd = dbus.Interface(obj, "org.freedesktop.portal.RemoteDesktop"); sc = dbus.Interface(obj, "org.freedesktop.portal.ScreenCast")
    sender = bus.get_unique_name()[1:].replace(".", "_")
    def request(call, *a, options=None):
        tok = f"fjarr{random.randint(0, 1 << 30)}"; out = {}
        path = f"/org/freedesktop/portal/desktop/request/{sender}/{tok}"
        rcv = bus.add_signal_receiver(lambda c, r: (out.update(code=int(c), res=r), loop.quit()), "Response",
                                      "org.freedesktop.portal.Request", path=path)
        call(*a, {**(options or {}), "handle_token": tok})
        t = GLib.timeout_add_seconds(20, loop.quit); loop.run()
        if "code" in out: GLib.source_remove(t)
        rcv.remove(); return out.get("code"), out.get("res")
    _, r = request(rd.CreateSession, options={"session_handle_token": "fjarrm"}); session = r["session_handle"]
    request(rd.SelectDevices, session, options={"types": dbus.UInt32(3), "persist_mode": dbus.UInt32(2),
                                                "restore_token": open(args.token).read().strip()})
    request(sc.SelectSources, session, options={"types": dbus.UInt32(1), "cursor_mode": dbus.UInt32(2)})
    code, r = request(rd.Start, session, "")
    if code != 0 or "streams" not in r:
        sys.exit(f"portal start failed: code={code} streams={'streams' in (r or {})}")
    open(args.token, "w").write(str(r["restore_token"]))
    node = int(r["streams"][0][0]); fd = sc.OpenPipeWireRemote(session, dbus.Dictionary({}, signature="sv")).take()
    def key():
        rd.NotifyKeyboardKeysym(session, {}, 0x20, dbus.UInt32(1)); rd.NotifyKeyboardKeysym(session, {}, 0x20, dbus.UInt32(0))
    def click():
        rd.NotifyPointerMotionAbsolute(session, {}, dbus.UInt32(node), 960.0, 700.0)
        rd.NotifyPointerButton(session, {}, 0x110, dbus.UInt32(1)); rd.NotifyPointerButton(session, {}, 0x110, dbus.UInt32(0))
    return f"pipewiresrc fd={fd} path={node}", key, click, "gnome-shell"

if combo in "AB": src, key, click, server = setup_x11()
elif combo == "E": src, key, click, server = setup_mutter()
else: src, key, click, server = setup_portal()
if combo in "BD":  # uinput from the root helper replaces the display server's own injection
    key, click = (lambda: uinput(b"k")), (lambda: uinput(b"c"))

# ---- frame reading ---------------------------------------------------------------------------
class Frames:
    def __init__(self):
        # always-copy: the harness maps frames on the CPU; the cpu test below keeps the zero-copy path
        copy = " always-copy=true" if src.startswith("pipewiresrc") else ""
        self.pipe = Gst.parse_launch(f"{src}{copy} ! appsink name=sink caps=video/x-raw max-buffers=1 drop=true sync=false")
        self.sink = self.pipe.get_by_name("sink"); self.pipe.set_state(Gst.State.PLAYING)
        self.caps = None
        if self.pull(10)[1] is None:  # a stream can take seconds to deliver its first frame
            sys.exit("no first frame within 10 s — is the stamp window drawing?")
    def pull(self, timeout=1.0):
        s = self.sink.emit("try-pull-sample", int(timeout * Gst.SECOND))
        if not s: return None, None
        t = time.time()
        info = GstVideo.VideoInfo.new_from_caps(s.get_caps()); self.caps = self.caps or s.get_caps().to_string()
        ok, m = s.get_buffer().map(Gst.MapFlags.READ)
        stride, bpp = info.stride[0], info.finfo.pixel_stride[0]
        g = 2 if info.finfo.name.startswith(("x", "A")) else 1
        pix = lambda x, y: m.data[y * stride + x * bpp + g] > 127
        stamp = 0
        for i in range(BITS): stamp = (stamp << 1) | pix(i * BLOCK + BLOCK // 2, 20)
        lower = pix(info.width // 2, info.height * 3 // 4)
        s.get_buffer().unmap(m)
        return t, (stamp, lower)
    def close(self): self.pipe.set_state(Gst.State.NULL)

def pct(xs, p): xs = sorted(xs); return xs[min(len(xs) - 1, int(p / 100 * len(xs)))]
def report(name, xs, unit="ms"):
    print(f"{combo} {name}: n={len(xs)} p50={pct(xs,50):.1f}{unit} p95={pct(xs,95):.1f}{unit} max={max(xs):.1f}{unit}", flush=True)

tests = args.tests.split(",")
if "decode" in tests:  # debugging the oracle itself: raw clock and lower-half reads
    f = Frames(); print("caps", f.caps)
    for _ in range(8):
        t, v = f.pull()
        print(f"now={int(t*1000)&0xFFFFFF} stamp={v[0]} lower={v[1]} diff={((int(t*1000)&0xFFFFFF)-v[0])&0xFFFFFF}")
    f.close()
if "latency" in tests or "input" in tests:
    f = Frames()
    if "latency" in tests:
        lat = []
        while len(lat) < args.n:
            t, v = f.pull()
            if v is None: sys.exit("latency: no frames")
            d = ((int(t * 1000) & 0xFFFFFF) - v[0]) & 0xFFFFFF
            if d < 5000: lat.append(d)
            elif (bad := locals().get("bad", 0) + 1) > 50: sys.exit(f"latency: 50 implausible clock reads (last {v[0]}) — the strip is not decoding")
        print(f"{combo} caps: {f.caps}")
        report("paint->capture", lat)
    if "input" in tests:
        click(); time.sleep(0.5)  # focus the stamp window; the click flips it too
        lat, missed = [], 0
        for _ in range(args.trials):
            _, v0 = f.pull()  # appsink keeps only the newest frame, so this is the current state
            t0 = time.time(); key(); seen = None
            while time.time() - t0 < 2:
                t, v = f.pull(0.1)
                if v is not None and v[1] != v0[1]: seen = t; break
            if seen: lat.append((seen - t0) * 1000)
            else: missed += 1
            time.sleep(random.uniform(0.2, 0.4))
        report(f"input->photon (missed {missed})", lat) if lat else print(f"{combo} input->photon: all {missed} missed")
    f.close()

if "cpu" in tests:
    def pid_of(name):
        me = str(os.getuid())
        out = subprocess.run(["pgrep", "-x", name] + (["-u", me] if name != "Xorg" else []), capture_output=True, text=True).stdout.split()
        return int(out[0]) if out else None
    procs = {"server": pid_of(server), "pipewire": pid_of("pipewire")}
    tick = os.sysconf("SC_CLK_TCK")
    def cpu(pid):
        f = open(f"/proc/{pid}/stat").read().rsplit(")", 1)[1].split(); return (int(f[11]) + int(f[12])) / tick
    def me(): t = os.times(); return t.user + t.system
    import glob  # the card number moves: with no monitor at boot the GPU is card0, not card1
    gpu_path = glob.glob("/sys/class/drm/card*/device/gpu_busy_percent")[0]
    def window(seconds):
        a = {k: cpu(p) for k, p in procs.items() if p}; a["agent"] = me(); g = []
        end = time.time() + seconds
        while time.time() < end:
            g.append(int(open(gpu_path).read())); time.sleep(0.1)
        b = {k: cpu(p) for k, p in procs.items() if p}; b["agent"] = me()
        return {k: 100 * (b[k] - a[k]) / seconds for k in a}, statistics.mean(g)
    idle, gidle = window(args.cpu_seconds)
    rate = "videorate ! video/x-raw,framerate=30/1 ! " if src.startswith("pipewiresrc") else "video/x-raw,framerate=30/1 ! "
    pipe = Gst.parse_launch(f"{src} ! {rate}vapostproc ! video/x-raw(memory:VAMemory),format=NV12 ! "
                            "vah264enc bitrate=4000 ! fakesink name=fs sync=false")
    frames = [0]
    pipe.get_by_name("fs").get_static_pad("sink").add_probe(Gst.PadProbeType.BUFFER, lambda *a: frames.__setitem__(0, frames[0] + 1) or Gst.PadProbeReturn.OK)
    pipe.set_state(Gst.State.PLAYING); time.sleep(2); frames[0] = 0
    busy, gbusy = window(args.cpu_seconds)
    src_caps = next(e for e in pipe.children if e.get_factory().get_name() in ("pipewiresrc", "ximagesrc")) \
        .get_static_pad("src").get_current_caps()
    pipe.set_state(Gst.State.NULL)
    print(f"{combo} cpu source caps: {src_caps.to_string() if src_caps else None}")
    print(f"{combo} cpu@1080p30: fps={frames[0]/args.cpu_seconds:.1f} " +
          " ".join(f"{k}={busy[k]:.1f}%(+{busy[k]-idle.get(k,0):.1f})" for k in busy) +
          f" gpu={gbusy:.0f}%(+{gbusy-gidle:.0f})", flush=True)
