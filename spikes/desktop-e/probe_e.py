#!/usr/bin/env python3
# Candidate E (docs/07): mutter's own ScreenCast + RemoteDesktop D-Bus interfaces, no portal — the
# path gnome-remote-desktop takes. Runs with the session user's credentials (its session bus and
# PipeWire), creates a remote-desktop session with a linked screencast of the primary monitor,
# checks a captured frame for the oracle's magenta, then injects a known sequence the oracle must
# log. Prints one verdict line per half. Throwaway spike code.
import sys, time
import dbus, dbus.mainloop.glib
import gi
gi.require_version("Gst", "1.0")
from gi.repository import GLib, Gst

dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
Gst.init(None)
bus = dbus.SessionBus()
loop = GLib.MainLoop()
MUTTER_RD, MUTTER_SC = "org.gnome.Mutter.RemoteDesktop", "org.gnome.Mutter.ScreenCast"

rd = dbus.Interface(bus.get_object(MUTTER_RD, "/org/gnome/Mutter/RemoteDesktop"), MUTTER_RD)
rd_path = rd.CreateSession()
rd_obj = bus.get_object(MUTTER_RD, rd_path)
rd_sess = dbus.Interface(rd_obj, MUTTER_RD + ".Session")
session_id = rd_obj.Get(MUTTER_RD + ".Session", "SessionId", dbus_interface=dbus.PROPERTIES_IFACE)
print(f"rd session {rd_path} id={session_id}")

sc = dbus.Interface(bus.get_object(MUTTER_SC, "/org/gnome/Mutter/ScreenCast"), MUTTER_SC)
sc_path = sc.CreateSession({"remote-desktop-session-id": session_id})
sc_sess = dbus.Interface(bus.get_object(MUTTER_SC, sc_path), MUTTER_SC + ".Session")
connector = sys.argv[1] if len(sys.argv) > 1 else ""
if connector == "virtual":  # no monitor at all: mutter creates one that lives as long as the stream
    stream_path = sc_sess.RecordVirtual({"cursor-mode": dbus.UInt32(1)})
else:
    stream_path = sc_sess.RecordMonitor(connector, {"cursor-mode": dbus.UInt32(1)})
print(f"screencast stream {stream_path} connector={connector!r}")

node = {}
def on_stream_added(node_id):
    node["id"] = int(node_id)
    loop.quit()
bus.add_signal_receiver(on_stream_added, "PipeWireStreamAdded", MUTTER_SC + ".Stream", path=stream_path)
rd_sess.Start()
GLib.timeout_add_seconds(10, loop.quit)
loop.run()
if "id" not in node:
    print("CAPTURE no: PipeWireStreamAdded never arrived"); sys.exit(1)
print(f"pipewire node {node['id']}")

pipe = Gst.parse_launch(
    f"pipewiresrc path={node["id"]} always-copy=true ! "
    # a virtual monitor takes its size from what the consumer negotiates; unasked, it is 1x1
    + ("video/x-raw,width=1920,height=1080 ! " if connector == "virtual" else "") + "videoconvert ! "
    "video/x-raw,format=RGB ! appsink name=sink max-buffers=1 drop=true sync=false")
pipe.set_state(Gst.State.PLAYING)
sink = pipe.get_by_name("sink")
sample = None
deadline = time.time() + 10
while time.time() < deadline:
    s = sink.emit("try-pull-sample", Gst.SECOND)
    if s: sample = s
    if sample and time.time() > deadline - 7: break
if not sample:
    print("CAPTURE no: no frame within 10 s"); sys.exit(1)
st = sample.get_caps().get_structure(0)
w, h = st.get_value("width"), st.get_value("height")
ok, info = sample.get_buffer().map(Gst.MapFlags.READ)
data = info.data
stride = len(data) // h
magenta = total = 0
for y in range(0, h, 16):
    row = y * stride
    for x in range(0, w, 16):
        r, g, b = data[row + 3*x], data[row + 3*x + 1], data[row + 3*x + 2]
        total += 1
        magenta += (r > 200 and g < 60 and b > 200)
with open("/tmp/fjarr-probe-frame.ppm", "wb") as f:  # what the capture saw, for a human to look at
    f.write(f"P6 {w} {h} 255\n".encode())
    for y in range(h): f.write(bytes(data[y*stride : y*stride + 3*w]))
print(f"CAPTURE {'yes' if magenta > total // 2 else 'frames-but-no-oracle'}: {w}x{h}, {100*magenta//total}% magenta")

# Input: the D-Bus Notify* methods, the pre-EIS half of the interface. The oracle must log each.
KEY_F, KEY_J = 0x66, 0x6a  # keysyms 'f' 'j'
for ks in (KEY_F, KEY_J):
    rd_sess.NotifyKeyboardKeysym(dbus.UInt32(ks), True)
    rd_sess.NotifyKeyboardKeysym(dbus.UInt32(ks), False)
rd_sess.NotifyPointerMotionAbsolute(stream_path, 321.0, 234.0)
BTN_LEFT = 0x110
rd_sess.NotifyPointerButton(dbus.Int32(BTN_LEFT), True)
rd_sess.NotifyPointerButton(dbus.Int32(BTN_LEFT), False)
time.sleep(1)
pipe.set_state(Gst.State.NULL)  # after injecting: a virtual monitor exists only while its stream is consumed
rd_sess.Stop()
print("INJECT sent: key f, key j, click at 321,234 — read the oracle log for the verdict")
