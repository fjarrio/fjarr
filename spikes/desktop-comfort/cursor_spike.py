# Throwaway spike (M3 3.5), run inside the desktop fixture as its user: a standalone ScreenCast of
# the primary monitor with cursor-mode=2, the cursor reader on its node, and the pointer moved
# through an unlinked RemoteDesktop session (relative motion needs no stream).
import subprocess, time, dbus, dbus.mainloop.glib
from gi.repository import GLib
RD, SC = "org.gnome.Mutter.RemoteDesktop", "org.gnome.Mutter.ScreenCast"
dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
bus = dbus.SessionBus()
sc_obj = bus.get_object(SC, dbus.Interface(bus.get_object(SC, "/org/gnome/Mutter/ScreenCast"), SC).CreateSession({}))
sc = dbus.Interface(sc_obj, SC + ".Session")
stream_path = sc.RecordMonitor("", {"cursor-mode": dbus.UInt32(2)})
loop = GLib.MainLoop(); node = {}
bus.add_signal_receiver(lambda n: (node.setdefault("id", int(n)), loop.quit()), "PipeWireStreamAdded", SC + ".Stream", path=stream_path)
sc.Start(); GLib.timeout_add_seconds(5, loop.quit); loop.run()
print("node", node.get("id"), flush=True)
import os
reader = subprocess.Popen(["/tmp/cursor", str(node["id"]), "6"], env=dict(os.environ, CURSOR_RAW="1"))
time.sleep(1.5)  # the reader has asked for the cursor meta before the pointer (and its sprite) exists
rd = dbus.Interface(bus.get_object(RD, dbus.Interface(bus.get_object(RD, "/org/gnome/Mutter/RemoteDesktop"), RD).CreateSession()), RD + ".Session")
rd.Start()
for i in range(20):
    rd.NotifyPointerMotionRelative(dbus.Double(7.0), dbus.Double(5.0)); time.sleep(0.1)
reader.wait()
print("reader exit", reader.returncode)
