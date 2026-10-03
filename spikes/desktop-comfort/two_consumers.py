# Throwaway spike (M3 3.5b): can a metadata-only cursor reader share a ScreenCast node with
# pipewiresrc? Both connect to one node recorded with cursor-mode=2; pipewiresrc must keep
# producing frames while the reader sees the pointer move.
import os, subprocess, time, dbus, dbus.mainloop.glib
from gi.repository import GLib
RD, SC = "org.gnome.Mutter.RemoteDesktop", "org.gnome.Mutter.ScreenCast"
dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
bus = dbus.SessionBus()
sc = dbus.Interface(bus.get_object(SC, dbus.Interface(bus.get_object(SC, "/org/gnome/Mutter/ScreenCast"), SC).CreateSession({})), SC + ".Session")
stream_path = sc.RecordMonitor("", {"cursor-mode": dbus.UInt32(2)})
loop = GLib.MainLoop(); node = {}
bus.add_signal_receiver(lambda n: (node.setdefault("id", int(n)), loop.quit()), "PipeWireStreamAdded", SC + ".Stream", path=stream_path)
sc.Start(); GLib.timeout_add_seconds(5, loop.quit); loop.run()
print("node", node.get("id"), flush=True)
frames = subprocess.Popen(["python3", "-c",
    "import gi,sys,time; gi.require_version('Gst','1.0'); from gi.repository import Gst; Gst.init(None);"
    f"p=Gst.parse_launch('pipewiresrc path={node['id']} keepalive-time=100 always-copy=true ! videoconvert ! fakesink name=s signal-handoffs=true sync=false');"
    "n=[0]; p.get_by_name('s').connect('handoff', lambda *a: n.__setitem__(0, n[0]+1));"
    "p.set_state(Gst.State.PLAYING); time.sleep(6); print('PIPEWIRESRC frames', n[0], flush=True); p.set_state(Gst.State.NULL)"])
time.sleep(1.5)
reader = subprocess.Popen(["/tmp/cursor", str(node["id"]), "4"], env=dict(os.environ, CURSOR_RAW=""))
time.sleep(1.0)
rd = dbus.Interface(bus.get_object(RD, dbus.Interface(bus.get_object(RD, "/org/gnome/Mutter/RemoteDesktop"), RD).CreateSession()), RD + ".Session")
rd.Start()
for i in range(15):
    rd.NotifyPointerMotionRelative(dbus.Double(9.0), dbus.Double(6.0)); time.sleep(0.1)
reader.wait(); frames.wait()
print("reader exit", reader.returncode, flush=True)
