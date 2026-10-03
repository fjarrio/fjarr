# Throwaway spike (M3 3.5), run inside the desktop fixture as its user: mutter's RemoteDesktop
# clipboard on an UNLINKED session (no ScreenCast), both directions, against wl-copy/wl-paste.
import os, subprocess, time, dbus, dbus.mainloop.glib
from gi.repository import GLib
RD = "org.gnome.Mutter.RemoteDesktop"
MIME = "text/plain;charset=utf-8"
dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
bus = dbus.SessionBus()
path = dbus.Interface(bus.get_object(RD, "/org/gnome/Mutter/RemoteDesktop"), RD).CreateSession()
rd = dbus.Interface(bus.get_object(RD, path), RD + ".Session")
events = []
bus.add_signal_receiver(lambda opts: events.append(("owner", dict(opts))), "SelectionOwnerChanged", RD + ".Session", path=path)
bus.add_signal_receiver(lambda mime, serial: events.append(("transfer", str(mime), int(serial))), "SelectionTransfer", RD + ".Session", path=path)
rd.Start()
rd.EnableClipboard({})
# A headless seat has no keyboard until a virtual one exists; the session makes one on its first key.
rd.NotifyKeyboardKeycode(dbus.UInt32(42), True); rd.NotifyKeyboardKeycode(dbus.UInt32(42), False)  # KEY_LEFTSHIFT
time.sleep(0.5)
print("clipboard enabled on an unlinked session", flush=True)
loop = GLib.MainLoop()
def pump(seconds, until=lambda: False):
    end = time.time() + seconds
    def tick():
        if until() or time.time() > end: loop.quit(); return False
        return True
    GLib.timeout_add(50, tick); loop.run()

# 1. robot -> operator
subprocess.Popen(["wl-copy", "robot says åäö"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL, start_new_session=True)
pump(3, lambda: any(e[0] == "owner" for e in events))
owner = [e for e in events if e[0] == "owner"]
print("SelectionOwnerChanged:", owner[-1][1] if owner else "NONE", flush=True)
if owner:
    fd = rd.SelectionRead(MIME).take(); os.set_blocking(fd, True)  # mutter hands it over non-blocking
    data = b""
    while True:
        chunk = os.read(fd, 65536)
        if not chunk: break
        data += chunk
    os.close(fd)
    print("SelectionRead ->", repr(data.decode()), flush=True)

# 2. operator -> robot: offer text under every name a Wayland or X11 app may ask for, and answer
# every transfer request (one per paste, per MIME the pasting app picked).
events.clear()
TEXT_TYPES = ["text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "STRING", "TEXT"]
rd.SetSelection({"mime-types": dbus.Array(TEXT_TYPES, signature="s")})
answered = []
def answer(mime, serial):
    fd = rd.SelectionWrite(dbus.UInt32(serial)).take(); os.set_blocking(fd, True)
    os.write(fd, "operator says ÅÄÖ".encode()); os.close(fd)
    rd.SelectionWriteDone(dbus.UInt32(serial), True); answered.append((str(mime), int(serial)))
bus.add_signal_receiver(answer, "SelectionTransfer", RD + ".Session", path=path)
paste = subprocess.Popen(["wl-paste", "--no-newline", "--type", "text/plain"], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
pump(4, lambda: paste.poll() is not None)
print("transfers answered:", answered, flush=True)
try:
    out, err = paste.communicate(timeout=3)
    print("wl-paste on the robot ->", repr(out.decode()), err.decode().strip()[:200], flush=True)
except subprocess.TimeoutExpired:
    paste.kill(); print("wl-paste timed out", flush=True)
rd.Stop()
