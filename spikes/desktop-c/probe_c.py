#!/usr/bin/env python3
# Candidate C (docs/07): the RemoteDesktop portal with a linked ScreenCast, persist_mode=2 so a
# grant yields a restore token. Usage: probe_c.py [token-file]. With a token file that exists, the
# token is offered back — the unattended question is whether Start then answers without a dialog.
# The new token is written back to the file. Capture checked for the oracle's colour; input via the
# portal's Notify* methods (the consent model is the same for ConnectToEIS, which comes after
# Start). Throwaway spike code.
import os, sys, time, random
import dbus, dbus.mainloop.glib
import gi
gi.require_version("Gst", "1.0")
from gi.repository import GLib, Gst

dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
Gst.init(None)
bus = dbus.SessionBus()
loop = GLib.MainLoop()
PORTAL = "org.freedesktop.portal.Desktop"
obj = bus.get_object(PORTAL, "/org/freedesktop/portal/desktop")
rd = dbus.Interface(obj, "org.freedesktop.portal.RemoteDesktop")
sc = dbus.Interface(obj, "org.freedesktop.portal.ScreenCast")
sender = bus.get_unique_name()[1:].replace(".", "_")
token_file = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser("~/.fjarr-portal-token")
TIMEOUT = int(os.environ.get("PROBE_TIMEOUT", "30"))

def request(call, *args, options=None, timeout=TIMEOUT):
    tok = f"fjarr{random.randint(0, 1 << 30)}"
    path = f"/org/freedesktop/portal/desktop/request/{sender}/{tok}"
    out = {}
    def on_response(code, results):
        out["code"], out["results"] = int(code), results
        loop.quit()
    rcv = bus.add_signal_receiver(on_response, "Response", "org.freedesktop.portal.Request", path=path)
    opts = dict(options or {}); opts["handle_token"] = tok
    call(*args, opts)
    t = GLib.timeout_add_seconds(timeout, loop.quit)
    loop.run()
    GLib.source_remove(t) if "code" in out else None
    rcv.remove()
    return out.get("code"), out.get("results")

code, res = request(rd.CreateSession, options={"session_handle_token": "fjarrsess"})
session = res["session_handle"]
print(f"session {session} (create code={code})")
code, _ = request(rd.SelectDevices, session, options={"types": dbus.UInt32(3), "persist_mode": dbus.UInt32(2),
    **({"restore_token": open(token_file).read().strip()} if os.path.exists(token_file) else {})})
print(f"select-devices code={code} restore_token_offered={os.path.exists(token_file)}")
code, _ = request(sc.SelectSources, session, options={"types": dbus.UInt32(1), "multiple": False, "cursor_mode": dbus.UInt32(2)})
print(f"select-sources code={code}")
t0 = time.time()
code, res = request(rd.Start, session, "")
waited = time.time() - t0
if code is None:
    print(f"START no: no answer in {TIMEOUT} s — a dialog is waiting for a human"); sys.exit(2)
if code != 0:
    print(f"START no: response code {code} after {waited:.1f} s (1 = user cancelled, 2 = other)"); sys.exit(2)
print(f"START yes after {waited:.1f} s, devices={int(res.get('devices', 0))} persist_mode={int(res.get('persist_mode', 0))}")
if "restore_token" in res:
    with open(token_file, "w") as f: f.write(str(res["restore_token"]))
    print(f"restore token written to {token_file}")
node = int(res["streams"][0][0])
fd = sc.OpenPipeWireRemote(session, dbus.Dictionary({}, signature="sv")).take()

pipe = Gst.parse_launch(f"pipewiresrc fd={fd} path={node} always-copy=true ! videoconvert ! "
    "video/x-raw,format=RGB ! appsink name=sink max-buffers=1 drop=true sync=false")
pipe.set_state(Gst.State.PLAYING)
sample = pipe.get_by_name("sink").emit("try-pull-sample", 10 * Gst.SECOND)
if not sample:
    print("CAPTURE no: no frame within 10 s")
else:
    st = sample.get_caps().get_structure(0); w, h = st.get_value("width"), st.get_value("height")
    _, info = sample.get_buffer().map(Gst.MapFlags.READ); d = info.data; stride = len(d) // h
    px = [(d[y*stride+3*x], d[y*stride+3*x+1], d[y*stride+3*x+2]) for y in range(0, h, 16) for x in range(0, w, 16)]
    m = sum(1 for r, g, b in px if r > 200 and g < 60 and b > 200)
    print(f"CAPTURE {'yes' if m > len(px)//2 else 'frames-but-no-oracle'}: {w}x{h}, {100*m//len(px)}% magenta")
pipe.set_state(Gst.State.NULL)

for ks in (0x63, 0x6b):  # 'c' 'k'
    rd.NotifyKeyboardKeysym(session, {}, ks, dbus.UInt32(1))
    rd.NotifyKeyboardKeysym(session, {}, ks, dbus.UInt32(0))
rd.NotifyPointerMotionAbsolute(session, {}, dbus.UInt32(node), 432.0, 345.0)
rd.NotifyPointerButton(session, {}, 0x110, dbus.UInt32(1))
rd.NotifyPointerButton(session, {}, 0x110, dbus.UInt32(0))
time.sleep(1)
print("INJECT sent: key c, key k, click at 432,345 — read the oracle log for the verdict")
