#!/usr/bin/env python3
# The agent half of the ADR-0028 handover spike. Runs as the agent's own system account
# (fjarr-agent), never the desktop user, from a system unit with a clean environment. First shows
# it cannot reach the desktop on its own (the user's bus and PipeWire socket both refuse it). Then
# listens on /run/fjarr/desktop.sock (owner fjarr-agent, group fjarr-desktop, 0660), accepts one
# helper whose SO_PEERCRED uid is the configured desktop user, and with the two descriptors the
# helper hands it: captures a frame from PipeWire and checks it for the oracle's magenta, then
# injects f, j and a click through libei, which the oracle must log. Throwaway spike code.
import ctypes, json, os, select, socket, struct, sys, time
import gi
gi.require_version("Gst", "1.0")
from gi.repository import Gst

DESKTOP_UID = int(sys.argv[1]) if len(sys.argv) > 1 else 1001
SOCK = "/run/fjarr/desktop.sock"

def say(msg): print(f"agent: {msg}", flush=True)

say(f"uid={os.getuid()} gid={os.getgid()} groups={os.getgroups()} "
    f"XDG_RUNTIME_DIR={os.environ.get('XDG_RUNTIME_DIR')} DBUS={os.environ.get('DBUS_SESSION_BUS_ADDRESS')}")
for path in (f"/run/user/{DESKTOP_UID}/bus", f"/run/user/{DESKTOP_UID}/pipewire-0"):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        s.connect(path); say(f"CONTROL FAIL: reached {path} directly")
    except OSError as e:
        say(f"control: {path} refuses us ({e.strerror})")
    s.close()

# --- the listening socket, per ADR-0028 "Direction and trust" -----------------------------------
try: os.unlink(SOCK)
except FileNotFoundError: pass
srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
srv.bind(SOCK)
os.chown(SOCK, -1, __import__("grp").getgrnam("fjarr-desktop").gr_gid)
os.chmod(SOCK, 0o660)
srv.listen(4)
say(f"listening on {SOCK}")

while True:
    conn, _ = srv.accept()
    pid, uid, gid = struct.unpack("3i", conn.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))
    if uid != DESKTOP_UID:
        say(f"REJECT peer pid={pid} uid={uid}: not the desktop user"); conn.close(); continue
    say(f"accept peer pid={pid} uid={uid}")
    msg, fds, _, _ = socket.recv_fds(conn, 4096, 2)
    break
meta = json.loads(msg)
pw_fd, eis_fd = fds
say(f"received pw fd={pw_fd} eis fd={eis_fd} meta={meta}")

# --- capture: pipewiresrc on the handed PipeWire connection -------------------------------------
Gst.init(None)
pipe = Gst.parse_launch(
    f"pipewiresrc fd={pw_fd} path={meta['node_id']} always-copy=true ! videoconvert ! "
    "video/x-raw,format=RGB ! appsink name=sink max-buffers=1 drop=true sync=false")
pipe.set_state(Gst.State.PLAYING)
sink = pipe.get_by_name("sink")
sample, frames, deadline = None, 0, time.time() + 10
while time.time() < deadline:
    s = sink.emit("try-pull-sample", Gst.SECOND)
    if s: sample, frames = s, frames + 1
    if sample and time.time() > deadline - 7: break
if not sample:
    say("CAPTURE no: no frame within 10 s"); sys.exit(1)
st = sample.get_caps().get_structure(0)
w, h = st.get_value("width"), st.get_value("height")
ok, info = sample.get_buffer().map(Gst.MapFlags.READ)
data = info.data
stride = len(data) // h
magenta = total = 0
for y in range(0, h, 16):
    for x in range(0, w, 16):
        r, g, b = data[y*stride + 3*x : y*stride + 3*x + 3]
        total += 1
        magenta += (r > 200 and g < 60 and b > 200)
with open("/tmp/fjarr-agent-frame.ppm", "wb") as f:
    f.write(f"P6 {w} {h} 255\n".encode())
    for y in range(h): f.write(bytes(data[y*stride : y*stride + 3*w]))
say(f"CAPTURE {'yes' if magenta > total // 2 else 'frames-but-no-oracle'}: {w}x{h}, "
    f"{100*magenta//total}% magenta, {frames} samples pulled")
pipe.set_state(Gst.State.NULL)

# --- input: libei sender on the handed EIS socket ------------------------------------------------
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
for key in (KEY_F, KEY_J):
    for down in (True, False):
        ei_lib.ei_device_keyboard_key(kbd, key, down); ei_lib.ei_device_frame(kbd, ei_lib.ei_now(ei))
x, y = meta["position"][0] + 321, meta["position"][1] + 234
ei_lib.ei_device_pointer_motion_absolute(ptr, x, y); ei_lib.ei_device_frame(ptr, ei_lib.ei_now(ei))
btn = ptr if ei_lib.ei_device_has_capability(ptr, CAP_BUTTON) else find(CAP_BUTTON)
for down in (True, False):
    ei_lib.ei_device_button_button(btn, BTN_LEFT, down); ei_lib.ei_device_frame(btn, ei_lib.ei_now(ei))
pump(0.5); time.sleep(1)
say(f"INJECT sent through EIS: key f, key j, click at {x},{y} — read the oracle log for the verdict")
conn.close()
