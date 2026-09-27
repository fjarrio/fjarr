#!/usr/bin/env python3
# Monitor hot-plug as candidate E sees it (docs/07 criteria): an E session recording the primary
# monitor, logging frames per second and every signal from mutter's session, stream and
# DisplayConfig, for as long as asked. Something else unplugs and replugs the monitor meanwhile
# (the connector's sysfs status, as root). Usage: hotplug_e.py SECONDS. Throwaway spike code.
import sys, time, threading
import dbus, dbus.mainloop.glib
import gi
gi.require_version("Gst", "1.0")
from gi.repository import GLib, Gst

dbus.mainloop.glib.DBusGMainLoop(set_as_default=True); Gst.init(None)
bus = dbus.SessionBus(); loop = GLib.MainLoop(); T0 = time.time()
log = lambda m: print(f"{time.time()-T0:6.1f}s {m}", flush=True)
RD, SC = "org.gnome.Mutter.RemoteDesktop", "org.gnome.Mutter.ScreenCast"
rd_path = dbus.Interface(bus.get_object(RD, "/org/gnome/Mutter/RemoteDesktop"), RD).CreateSession()
rd_obj = bus.get_object(RD, rd_path); rd = dbus.Interface(rd_obj, RD + ".Session")
sid = rd_obj.Get(RD + ".Session", "SessionId", dbus_interface=dbus.PROPERTIES_IFACE)
sc_path = dbus.Interface(bus.get_object(SC, "/org/gnome/Mutter/ScreenCast"), SC).CreateSession({"remote-desktop-session-id": sid})
stream = dbus.Interface(bus.get_object(SC, sc_path), SC + ".Session").RecordMonitor("", {"cursor-mode": dbus.UInt32(1)})
for name, iface, path in [("rd", RD + ".Session", rd_path), ("sc", SC + ".Session", sc_path), ("stream", SC + ".Stream", stream),
                          ("displayconfig", "org.gnome.Mutter.DisplayConfig", "/org/gnome/Mutter/DisplayConfig")]:
    bus.add_signal_receiver(lambda *a, n=name, **k: log(f"signal {n}.{k['member']} {[str(x) for x in a][:2]}"),
                            dbus_interface=iface, path=path, member_keyword="member")
frames = [0]; pipe = [None]
def on_node(node_id):
    log(f"PipeWireStreamAdded node={int(node_id)}")
    GLib.timeout_add(500, lambda: start(node_id) and False)  # the node reaches PipeWire just after the signal
def start(node_id):
    if pipe[0]: pipe[0].set_state(Gst.State.NULL)
    p = Gst.parse_launch(f"pipewiresrc path={int(node_id)} ! video/x-raw ! fakesink name=fs sync=false")
    p.get_by_name("fs").get_static_pad("sink").add_probe(Gst.PadProbeType.BUFFER, lambda *a: frames.__setitem__(0, frames[0] + 1) or Gst.PadProbeReturn.OK)
    b = p.get_bus(); b.add_signal_watch(); b.connect("message::error", lambda _b, m: log(f"gst error: {m.parse_error()[0].message}"))
    b.connect("message::eos", lambda *a: log("gst eos")); p.set_state(Gst.State.PLAYING); pipe[0] = p
bus.add_signal_receiver(on_node, "PipeWireStreamAdded", SC + ".Stream", path=stream)
def tick():
    log(f"fps={frames[0]}"); frames[0] = 0; return True
GLib.timeout_add_seconds(1, tick); GLib.timeout_add_seconds(int(sys.argv[1]), loop.quit)
rd.Start(); log("started"); loop.run()
