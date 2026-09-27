#!/usr/bin/env python3
# Candidate D's capture half (docs/07): a ScreenCast-only portal session, persist_mode=2, optional
# restore token from a file. The unattended question is whether Start answers without a human.
# Usage: probe_d_capture.py [token-file]. Throwaway spike code.
import os, sys, time, random
import dbus, dbus.mainloop.glib
from gi.repository import GLib

dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
bus = dbus.SessionBus(); loop = GLib.MainLoop()
sc = dbus.Interface(bus.get_object("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop"),
                    "org.freedesktop.portal.ScreenCast")
sender = bus.get_unique_name()[1:].replace(".", "_")
token_file = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser("~/.fjarr-screencast-token")
TIMEOUT = int(os.environ.get("PROBE_TIMEOUT", "30"))

def request(call, *args, options=None):
    tok = f"fjarr{random.randint(0, 1 << 30)}"; out = {}
    path = f"/org/freedesktop/portal/desktop/request/{sender}/{tok}"
    def on_response(code, results): out.update(code=int(code), results=results); loop.quit()
    rcv = bus.add_signal_receiver(on_response, "Response", "org.freedesktop.portal.Request", path=path)
    call(*args, {**(options or {}), "handle_token": tok})
    t = GLib.timeout_add_seconds(TIMEOUT, loop.quit); loop.run()
    if "code" in out: GLib.source_remove(t)
    rcv.remove(); return out.get("code"), out.get("results")

_, res = request(sc.CreateSession, options={"session_handle_token": "fjarrsc"})
session = res["session_handle"]
has_token = os.path.exists(token_file)
request(sc.SelectSources, session, options={"types": dbus.UInt32(1), "persist_mode": dbus.UInt32(2),
    **({"restore_token": open(token_file).read().strip()} if has_token else {})})
t0 = time.time(); code, res = request(sc.Start, session, "")
if code is None: print(f"START no: no answer in {TIMEOUT} s with token={has_token} — a dialog is waiting for a human"); sys.exit(2)
if code != 0: print(f"START no: code {code}"); sys.exit(2)
print(f"START yes after {time.time()-t0:.1f} s with token={has_token}, streams={[int(s[0]) for s in res['streams']]}")
if "restore_token" in res: open(token_file, "w").write(str(res["restore_token"]))
