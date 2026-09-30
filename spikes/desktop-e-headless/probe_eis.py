#!/usr/bin/env python3
# Throwaway spike (M3 slice 3.0): input through EIS on headless mutter. A RemoteDesktop session with
# a linked screencast (EIS needs one to map absolute coordinates), ConnectToEIS, then the libei
# client from spikes/desktop-helper/agent.py: a click to focus the oracle, keys f and j, a click at
# 321,234. The oracle's log is the verdict. Throwaway spike code.
import ctypes, os, select, sys, time
import dbus, dbus.mainloop.glib
from gi.repository import GLib

dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
bus = dbus.SessionBus()
loop = GLib.MainLoop()
MUTTER_RD, MUTTER_SC = "org.gnome.Mutter.RemoteDesktop", "org.gnome.Mutter.ScreenCast"
def say(msg): print(f"eis: {msg}", flush=True)

rd = dbus.Interface(bus.get_object(MUTTER_RD, "/org/gnome/Mutter/RemoteDesktop"), MUTTER_RD)
rd_path = rd.CreateSession()
rd_obj = bus.get_object(MUTTER_RD, rd_path)
rd_sess = dbus.Interface(rd_obj, MUTTER_RD + ".Session")
sid = rd_obj.Get(MUTTER_RD + ".Session", "SessionId", dbus_interface=dbus.PROPERTIES_IFACE)
sc = dbus.Interface(bus.get_object(MUTTER_SC, "/org/gnome/Mutter/ScreenCast"), MUTTER_SC)
sc_path = sc.CreateSession({"remote-desktop-session-id": sid})
sc_sess = dbus.Interface(bus.get_object(MUTTER_SC, sc_path), MUTTER_SC + ".Session")
stream_path = sc_sess.RecordMonitor("", {"cursor-mode": dbus.UInt32(1)})
rd_sess.Start()
fd = rd_sess.ConnectToEIS({})
eis_fd = fd.take()
say(f"ConnectToEIS gave fd {eis_fd}")
meta = {"position": (0, 0)}

ei_lib = ctypes.CDLL("libei.so.1")
P, U32, U64, I = ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint64, ctypes.c_int
for name, res, args in [
    ("ei_new_sender", P, [P]), ("ei_configure_name", None, [P, ctypes.c_char_p]),
    ("ei_setup_backend_fd", I, [P, I]), ("ei_get_fd", I, [P]), ("ei_dispatch", None, [P]),
    ("ei_get_event", P, [P]), ("ei_event_unref", P, [P]), ("ei_event_get_type", I, [P]),
    ("ei_event_type_to_string", ctypes.c_char_p, [I]), ("ei_event_get_seat", P, [P]),
    ("ei_event_get_device", P, [P]), ("ei_device_ref", P, [P]), ("ei_device_get_name", ctypes.c_char_p, [P]),
    ("ei_device_has_capability", ctypes.c_bool, [P, I]), ("ei_device_start_emulating", None, [P, U32]),
    ("ei_device_get_region", P, [P, ctypes.c_size_t]), ("ei_region_get_x", U32, [P]),
    ("ei_region_get_y", U32, [P]), ("ei_region_get_width", U32, [P]), ("ei_region_get_height", U32, [P]),
    ("ei_now", U64, [P]), ("ei_device_frame", None, [P, U64]),
    ("ei_device_keyboard_key", None, [P, U32, ctypes.c_bool]),
    ("ei_device_pointer_motion_absolute", None, [P, ctypes.c_double, ctypes.c_double]),
    ("ei_device_button_button", None, [P, U32, ctypes.c_bool]),
]:
    fn = getattr(ei_lib, name); fn.restype = res; fn.argtypes = args
ei_lib.ei_seat_bind_capabilities.restype = None  # variadic, NULL-terminated
# libei.h enums (libei-dev 1.5.0)
CAP_POINTER_ABSOLUTE, CAP_KEYBOARD, CAP_BUTTON = 1 << 1, 1 << 2, 1 << 5
EV_SEAT_ADDED, EV_DEVICE_ADDED, EV_DEVICE_RESUMED, EV_DISCONNECT = 3, 5, 8, 2

ei = ei_lib.ei_new_sender(None)
ei_lib.ei_configure_name(ei, b"fjarr-agent-spike")
rc = ei_lib.ei_setup_backend_fd(ei, eis_fd)
if rc != 0:
    say(f"INJECT no: ei_setup_backend_fd={rc}"); sys.exit(1)
devices, resumed, seq = [], set(), 1
def pump(timeout):
    global seq
    r, _, _ = select.select([ei_lib.ei_get_fd(ei)], [], [], timeout)
    ei_lib.ei_dispatch(ei)
    while ev := ei_lib.ei_get_event(ei):
        t = ei_lib.ei_event_get_type(ev)
        say(f"ei event {ei_lib.ei_event_type_to_string(t).decode()}")
        if t == EV_SEAT_ADDED:
            # variadic: no argtypes, so the seat must be wrapped or ctypes truncates it to an int
            ei_lib.ei_seat_bind_capabilities(P(ei_lib.ei_event_get_seat(ev)), I(CAP_POINTER_ABSOLUTE),
                                             I(CAP_KEYBOARD), I(CAP_BUTTON), P(None))
        elif t == EV_DEVICE_ADDED:
            d = ei_lib.ei_device_ref(ei_lib.ei_event_get_device(ev))
            devices.append(d)
            regions = []
            i = 0
            while reg := ei_lib.ei_device_get_region(d, i):
                regions.append((ei_lib.ei_region_get_x(reg), ei_lib.ei_region_get_y(reg),
                                ei_lib.ei_region_get_width(reg), ei_lib.ei_region_get_height(reg)))
                i += 1
            say(f"  device {ei_lib.ei_device_get_name(d).decode()!r} abs={ei_lib.ei_device_has_capability(d, CAP_POINTER_ABSOLUTE)} "
                f"kbd={ei_lib.ei_device_has_capability(d, CAP_KEYBOARD)} btn={ei_lib.ei_device_has_capability(d, CAP_BUTTON)} regions={regions}")
        elif t == EV_DEVICE_RESUMED:
            d = ei_lib.ei_event_get_device(ev)
            ei_lib.ei_device_start_emulating(d, seq); seq += 1
            resumed.add(d)
        elif t == EV_DISCONNECT:
            say("INJECT no: EIS disconnected us"); sys.exit(1)
        ei_lib.ei_event_unref(ev)

def find(cap):
    return next((d for d in devices if d in resumed and ei_lib.ei_device_has_capability(d, cap)), None)

deadline = time.time() + 5
while time.time() < deadline and not (find(CAP_KEYBOARD) and find(CAP_POINTER_ABSOLUTE)):
    pump(0.2)
kbd, ptr = find(CAP_KEYBOARD), find(CAP_POINTER_ABSOLUTE)
if not (kbd and ptr):
    say(f"INJECT no: kbd={bool(kbd)} abs-pointer={bool(ptr)} after 5 s"); sys.exit(1)

KEY_F, KEY_J, BTN_LEFT = 33, 36, 0x110  # evdev codes
def click(x, y):
    ei_lib.ei_device_pointer_motion_absolute(ptr, x, y); ei_lib.ei_device_frame(ptr, ei_lib.ei_now(ei))
    btn = ptr if ei_lib.ei_device_has_capability(ptr, CAP_BUTTON) else find(CAP_BUTTON)
    for down in (True, False):
        ei_lib.ei_device_button_button(btn, BTN_LEFT, down); ei_lib.ei_device_frame(btn, ei_lib.ei_now(ei))
    pump(0.3)
click(640, 360); time.sleep(0.5)          # focus the oracle first: nothing has focus on a headless desktop
for key in (KEY_F, KEY_J):
    for down in (True, False):
        ei_lib.ei_device_keyboard_key(kbd, key, down); ei_lib.ei_device_frame(kbd, ei_lib.ei_now(ei))
    pump(0.2)
click(321, 234)
pump(0.5); time.sleep(1)
say("INJECT sent through EIS: click to focus, key f, key j, click at 321,234")
rd_sess.Stop()
