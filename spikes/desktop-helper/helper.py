#!/usr/bin/env python3
# The session-helper half of the ADR-0028 handover spike. Runs as the desktop user, inside the
# session: creates mutter's RemoteDesktop session with a linked ScreenCast of the primary monitor
# (as spikes/desktop-e/probe_e.py does), opens the two descriptors that carry the heavy traffic —
# a fresh connection to the user's PipeWire socket and mutter's EIS socket (ConnectToEIS) — and
# hands both to the agent over /run/fjarr/desktop.sock with SCM_RIGHTS. It then holds the sessions
# open until the agent hangs up. Nothing it does touches a frame or an input event.
# Throwaway spike code.
import json, os, socket, sys
import dbus, dbus.mainloop.glib
from gi.repository import GLib

AGENT_SOCK = os.environ.get("FJARR_DESKTOP_SOCK", "/run/fjarr/desktop.sock")
PW_SOCK = os.path.join(os.environ.get("XDG_RUNTIME_DIR", f"/run/user/{os.getuid()}"), "pipewire-0")

dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
bus = dbus.SessionBus()
loop = GLib.MainLoop()
MUTTER_RD, MUTTER_SC = "org.gnome.Mutter.RemoteDesktop", "org.gnome.Mutter.ScreenCast"

rd = dbus.Interface(bus.get_object(MUTTER_RD, "/org/gnome/Mutter/RemoteDesktop"), MUTTER_RD)
rd_path = rd.CreateSession()
rd_obj = bus.get_object(MUTTER_RD, rd_path)
rd_sess = dbus.Interface(rd_obj, MUTTER_RD + ".Session")
session_id = rd_obj.Get(MUTTER_RD + ".Session", "SessionId", dbus_interface=dbus.PROPERTIES_IFACE)
sc = dbus.Interface(bus.get_object(MUTTER_SC, "/org/gnome/Mutter/ScreenCast"), MUTTER_SC)
sc_path = sc.CreateSession({"remote-desktop-session-id": session_id})
sc_sess = dbus.Interface(bus.get_object(MUTTER_SC, sc_path), MUTTER_SC + ".Session")
stream_path = sc_sess.RecordMonitor("", {"cursor-mode": dbus.UInt32(1)})
stream_params = bus.get_object(MUTTER_SC, stream_path).Get(
    MUTTER_SC + ".Stream", "Parameters", dbus_interface=dbus.PROPERTIES_IFACE)

node = {}
def on_stream_added(node_id):
    node["id"] = int(node_id)
    loop.quit()
bus.add_signal_receiver(on_stream_added, "PipeWireStreamAdded", MUTTER_SC + ".Stream", path=stream_path)
rd_sess.Start()
GLib.timeout_add_seconds(10, loop.quit)
loop.run()
if "id" not in node:
    print("helper: PipeWireStreamAdded never arrived"); sys.exit(1)

# EIS: mutter creates the socket pair and returns our end. device-types: keyboard | pointer.
eis_fd = rd_sess.ConnectToEIS({"device-types": dbus.UInt32(3)}).take()
# PipeWire: connect the socket ourselves and hand it over before the protocol's hello. The server
# reads SO_PEERCRED when it accepts, so the connection carries the desktop user's identity no
# matter which process speaks on it afterwards. (The portal's OpenPipeWireRemote narrows the
# client's permissions to the granted nodes first; this spike does not — see README.)
pw = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
pw.connect(PW_SOCK)

meta = {
    "node_id": node["id"],
    "stream": str(stream_path),
    "position": [int(v) for v in stream_params.get("position", (0, 0))],
    "size": [int(v) for v in stream_params.get("size", (0, 0))],
    "mapping_id": str(stream_params.get("mapping-id", "")),
}
agent = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
agent.connect(AGENT_SOCK)
socket.send_fds(agent, [json.dumps(meta).encode()], [pw.fileno(), eis_fd])
print(f"helper: handed pw fd + eis fd to agent, {meta}", flush=True)
pw.close(); os.close(eis_fd)  # the agent holds its own copies now

while agent.recv(4096):  # hold the sessions until the agent hangs up
    pass
rd_sess.Stop()
print("helper: agent hung up, sessions stopped", flush=True)
