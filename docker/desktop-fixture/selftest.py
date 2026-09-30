#!/usr/bin/env python3
# The fixture proves itself: backend E's two halves on headless mutter, as the agent will use them.
#   capture — a RemoteDesktop session with a linked ScreenCast of the primary monitor; one frame
#             through pipewiresrc must be the test window's colour;
#   input   — ConnectToEIS, then libei: a click to focus the window (nothing has focus on a headless
#             desktop until something clicks), keys f and j, and a click at 321,234. The window's
#             log is the verdict.
# One PASS/FAIL line per check; exit status 1 on any failure.
# spec: docs/15-testing-strategy.md#the-desktop-test-lab · ADR-0033 (and its 2026-09-30 addendum)
import ctypes
import os
import select
import sys
import time

import dbus
import dbus.mainloop.glib
import gi

gi.require_version("Gst", "1.0")
from gi.repository import GLib, Gst  # noqa: E402

RD, SC = "org.gnome.Mutter.RemoteDesktop", "org.gnome.Mutter.ScreenCast"
LOG = os.environ.get("FJARR_FIXTURE_LOG", "/run/desktop/testwin.log")
failed = False


def check(name, ok, detail):
    global failed
    failed |= not ok
    print(f"{'PASS' if ok else 'FAIL'} {name}: {detail}", flush=True)


def session():
    """A RemoteDesktop session with a linked ScreenCast stream of the primary monitor."""
    bus = dbus.SessionBus()
    rd_obj = bus.get_object(RD, dbus.Interface(bus.get_object(RD, "/org/gnome/Mutter/RemoteDesktop"), RD).CreateSession())
    rd = dbus.Interface(rd_obj, RD + ".Session")
    sid = rd_obj.Get(RD + ".Session", "SessionId", dbus_interface=dbus.PROPERTIES_IFACE)
    sc_path = dbus.Interface(bus.get_object(SC, "/org/gnome/Mutter/ScreenCast"), SC).CreateSession({"remote-desktop-session-id": sid})
    sc = dbus.Interface(bus.get_object(SC, sc_path), SC + ".Session")
    stream = sc.RecordMonitor("", {"cursor-mode": dbus.UInt32(1)})  # "": the primary monitor
    return bus, rd, stream


def capture(bus, rd, stream):
    loop = GLib.MainLoop()
    node = {}

    def added(node_id):
        node["id"] = int(node_id)
        loop.quit()

    bus.add_signal_receiver(added, "PipeWireStreamAdded", SC + ".Stream", path=stream)
    rd.Start()
    GLib.timeout_add_seconds(10, loop.quit)
    loop.run()
    if "id" not in node:
        check("capture", False, "PipeWireStreamAdded never arrived")
        return
    pipe = Gst.parse_launch(
        f"pipewiresrc path={node['id']} always-copy=true ! videoconvert ! video/x-raw,format=RGB ! "
        "appsink name=sink max-buffers=1 drop=true sync=false")
    pipe.set_state(Gst.State.PLAYING)
    sample = pipe.get_by_name("sink").emit("try-pull-sample", 10 * Gst.SECOND)
    pipe.set_state(Gst.State.NULL)
    if not sample:
        check("capture", False, f"PipeWire node {node['id']}: no frame within 10 s")
        return
    st = sample.get_caps().get_structure(0)
    w, h = st.get_value("width"), st.get_value("height")
    ok, info = sample.get_buffer().map(Gst.MapFlags.READ)
    data, stride = info.data, len(info.data) // h
    hits = total = 0
    for y in range(0, h, 16):
        for x in range(0, w, 16):
            r, g, b = data[y * stride + 3 * x: y * stride + 3 * x + 3]
            total += 1
            hits += r > 200 and g < 60 and b > 200
    check("capture", hits * 10 >= total * 9, f"{w}x{h} from PipeWire node {node['id']}, {100 * hits // total}% the window's colour")


def drive_eis(eis_fd):
    """As backend E will over an EIS socket: a click to focus the window, keys f and j, a click at
    321,234. Returns an error string, or "" once the events are sent. Also used by fixture-helper-check."""
    lib = ctypes.CDLL("libei.so.1")
    P, U32, U64, I = ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint64, ctypes.c_int
    for name, res, args in [
        ("ei_new_sender", P, [P]), ("ei_configure_name", None, [P, ctypes.c_char_p]),
        ("ei_setup_backend_fd", I, [P, I]), ("ei_get_fd", I, [P]), ("ei_dispatch", None, [P]),
        ("ei_get_event", P, [P]), ("ei_event_unref", P, [P]), ("ei_event_get_type", I, [P]),
        ("ei_event_get_seat", P, [P]), ("ei_event_get_device", P, [P]), ("ei_device_ref", P, [P]),
        ("ei_device_has_capability", ctypes.c_bool, [P, I]), ("ei_device_start_emulating", None, [P, U32]),
        ("ei_now", U64, [P]), ("ei_device_frame", None, [P, U64]),
        ("ei_device_keyboard_key", None, [P, U32, ctypes.c_bool]),
        ("ei_device_pointer_motion_absolute", None, [P, ctypes.c_double, ctypes.c_double]),
        ("ei_device_button_button", None, [P, U32, ctypes.c_bool]),
    ]:
        fn = getattr(lib, name)
        fn.restype, fn.argtypes = res, args
    lib.ei_seat_bind_capabilities.restype = None  # variadic, NULL-terminated
    CAP_ABS, CAP_KBD, CAP_BTN = 1 << 1, 1 << 2, 1 << 5  # libei.h
    EV_SEAT_ADDED, EV_DEVICE_ADDED, EV_DEVICE_RESUMED, EV_DISCONNECT = 3, 5, 8, 2

    ei = lib.ei_new_sender(None)
    lib.ei_configure_name(ei, b"fjarr-fixture-selftest")
    if lib.ei_setup_backend_fd(ei, eis_fd) != 0:
        return "ei_setup_backend_fd failed"
    devices, resumed, seq = [], set(), [1]

    def pump(timeout):
        select.select([lib.ei_get_fd(ei)], [], [], timeout)
        lib.ei_dispatch(ei)
        while ev := lib.ei_get_event(ei):
            t = lib.ei_event_get_type(ev)
            if t == EV_SEAT_ADDED:
                lib.ei_seat_bind_capabilities(P(lib.ei_event_get_seat(ev)), I(CAP_ABS), I(CAP_KBD), I(CAP_BTN), P(None))
            elif t == EV_DEVICE_ADDED:
                devices.append(lib.ei_device_ref(lib.ei_event_get_device(ev)))
            elif t == EV_DEVICE_RESUMED:
                d = lib.ei_event_get_device(ev)
                lib.ei_device_start_emulating(d, seq[0])
                seq[0] += 1
                resumed.add(d)
            elif t == EV_DISCONNECT:
                raise RuntimeError("EIS disconnected")
            lib.ei_event_unref(ev)

    def find(cap):
        return next((d for d in devices if d in resumed and lib.ei_device_has_capability(d, cap)), None)

    deadline = time.time() + 5
    while time.time() < deadline and not (find(CAP_KBD) and find(CAP_ABS)):
        pump(0.2)
    kbd, ptr = find(CAP_KBD), find(CAP_ABS)
    if not (kbd and ptr):
        return f"EIS devices after 5 s: keyboard={bool(kbd)} absolute pointer={bool(ptr)}"
    btn = ptr if lib.ei_device_has_capability(ptr, CAP_BTN) else find(CAP_BTN)

    def click(x, y):
        lib.ei_device_pointer_motion_absolute(ptr, x, y)
        lib.ei_device_frame(ptr, lib.ei_now(ei))
        for down in (True, False):
            lib.ei_device_button_button(btn, 0x110, down)  # BTN_LEFT
            lib.ei_device_frame(btn, lib.ei_now(ei))
        pump(0.3)

    click(640, 360)  # focus first: nothing has focus on a headless desktop until something clicks
    time.sleep(0.3)
    for key in (33, 36):  # evdev KEY_F, KEY_J
        for down in (True, False):
            lib.ei_device_keyboard_key(kbd, key, down)
            lib.ei_device_frame(kbd, lib.ei_now(ei))
        pump(0.2)
    click(321, 234)
    time.sleep(0.8)
    return ""


def inject(rd):
    """libei over mutter's own EIS socket (ConnectToEIS), checked in the window's log."""
    open(LOG, "a").close()
    start = os.path.getsize(LOG)
    err = drive_eis(rd.ConnectToEIS({}).take())
    if err:
        check("input", False, err)
        return
    with open(LOG) as f:
        f.seek(start)
        seen = f.read()
    for name, needle in [("input: focus", "focus active=True"), ("input: key f", "key f text='f'"),
                         ("input: key j", "key j text='j'"), ("input: click at 321,234", "click button=1 x=321 y=234")]:
        check(name, needle in seen or (name == "input: focus" and "focus active=True" in open(LOG).read()),
              "in the window's log" if needle in seen else f"not in the window's log: {seen.strip()!r}")


def main():
    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    Gst.init(None)
    if not os.path.exists("/run/desktop/ready"):
        check("fixture", False, "the session is not ready (/run/desktop/ready missing)")
        sys.exit(1)
    bus, rd, stream = session()
    capture(bus, rd, stream)
    try:
        inject(rd)
    finally:
        rd.Stop()
    print(f"SUMMARY desktop-fixture: {'failed' if failed else 'passed'}", flush=True)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
