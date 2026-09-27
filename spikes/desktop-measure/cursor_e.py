#!/usr/bin/env python3
# Cursor metadata through GStreamer (docs/07 criteria): an E stream with cursor-mode 2 (metadata,
# cursor not drawn into the frame), the pointer moved by E, and each buffer's metas listed. If
# pipewiresrc surfaced PipeWire's cursor metadata, a meta beyond the video ones would appear.
# Throwaway spike code.
import time
import dbus, dbus.mainloop.glib
import gi
gi.require_version("Gst", "1.0")
from gi.repository import GLib, Gst

dbus.mainloop.glib.DBusGMainLoop(set_as_default=True); Gst.init(None)
bus = dbus.SessionBus(); loop = GLib.MainLoop()
RD, SC = "org.gnome.Mutter.RemoteDesktop", "org.gnome.Mutter.ScreenCast"
rd_path = dbus.Interface(bus.get_object(RD, "/org/gnome/Mutter/RemoteDesktop"), RD).CreateSession()
rd_obj = bus.get_object(RD, rd_path); rd = dbus.Interface(rd_obj, RD + ".Session")
sid = rd_obj.Get(RD + ".Session", "SessionId", dbus_interface=dbus.PROPERTIES_IFACE)
sc_path = dbus.Interface(bus.get_object(SC, "/org/gnome/Mutter/ScreenCast"), SC).CreateSession({"remote-desktop-session-id": sid})
stream = dbus.Interface(bus.get_object(SC, sc_path), SC + ".Session").RecordMonitor("", {"cursor-mode": dbus.UInt32(2)})
node = {}
bus.add_signal_receiver(lambda n: (node.update(id=int(n)), loop.quit()), "PipeWireStreamAdded", SC + ".Stream", path=stream)
rd.Start(); GLib.timeout_add_seconds(10, loop.quit); loop.run()
p = Gst.parse_launch(f"pipewiresrc path={node['id']} always-copy=true ! video/x-raw ! appsink name=sink max-buffers=4 drop=true sync=false")
sink = p.get_by_name("sink"); p.set_state(Gst.State.PLAYING)
apis = set()
for i in range(60):
    rd.NotifyPointerMotionAbsolute(stream, 100.0 + 10 * i, 600.0)
    s = sink.emit("try-pull-sample", Gst.SECOND // 5)
    if not s: continue
    buf = s.get_buffer(); state = None
    while True:
        m, state = (buf.iterate_meta(state) if hasattr(buf, "iterate_meta") else (None, None))
        if not m: break
        apis.add(m.info.api.name if hasattr(m.info.api, "name") else str(m.info.api))
p.set_state(Gst.State.NULL)
print(f"buffer metas seen with cursor-mode=metadata: {sorted(apis) or 'none'}")
