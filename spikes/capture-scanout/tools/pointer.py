#!/usr/bin/env python3
"""pointer.py COMMAND ... OUTLOG  (desktop user, in the session: see as-desktop.sh)

Moves the pointer through a mutter RemoteDesktop session of its own
(org.gnome.Mutter.RemoteDesktop.Session.NotifyPointerMotionRelative) and logs what it did.
mutter re-evaluates its cursor renderer (hardware or software cursor, and with it whether a
fullscreen window can be scanned out directly) only when the cursor moves or changes shape, and an
idle session keeps notification banners on screen (which block direct scanout) until there is
input. So the experiments move the pointer on purpose, and only when they say so.

  nudge DELAY_S SECONDS PERIOD_MS OUTLOG
      after DELAY_S, move 1 px right and back every PERIOD_MS for SECONDS
  square SECONDS STEP_PX PERIOD_MS OUTLOG
      move around a square, STEP_PX per motion every PERIOD_MS (10 right, 10 down, 10 left, 10 up,
      repeated), logging each motion with its running offset (for checking cursor metadata)
  move DX DY OUTLOG
      one relative motion (to put the pointer back where it was found)

Example: ./as-desktop.sh python3 pointer.py nudge 0 5 100 runs/r1/pointer.log
"""
import sys
import time

from gi.repository import Gio, GLib

RD = "org.gnome.Mutter.RemoteDesktop"
cmd = sys.argv[1] if len(sys.argv) > 1 else ""
nargs = {"nudge": 4, "square": 4, "move": 3}
if cmd not in nargs or len(sys.argv) != 2 + nargs[cmd]:
    sys.exit(__doc__)
out = open(sys.argv[-1], "a", buffering=1)
bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)


def log(line):
    out.write(f"{time.time():.3f} pointer {line}\n")


def call(path, iface, method, params, rtype=None):
    return bus.call_sync(RD, path, iface, method, params, GLib.VariantType(rtype) if rtype else None,
                         Gio.DBusCallFlags.NONE, 5000, None)


def motion(session, dx, dy):
    call(session, RD + ".Session", "NotifyPointerMotionRelative", GLib.Variant("(dd)", (dx, dy)))


session = call("/org/gnome/Mutter/RemoteDesktop", RD, "CreateSession", None, "(o)")[0]
call(session, RD + ".Session", "Start", None)
log(f"{cmd} session {session} started")
n = 0
try:
    if cmd == "nudge":
        delay, seconds, period = float(sys.argv[2]), float(sys.argv[3]), int(sys.argv[4]) / 1000.0
        time.sleep(delay)
        end = time.time() + seconds
        while time.time() < end:
            motion(session, 1.0 if n % 2 == 0 else -1.0, 0.0)
            if n == 0:
                log("first motion")
            n += 1
            time.sleep(period)
    elif cmd == "square":
        seconds, step, period = float(sys.argv[2]), float(sys.argv[3]), int(sys.argv[4]) / 1000.0
        dirs = [(step, 0.0), (0.0, step), (-step, 0.0), (0.0, -step)]
        end = time.time() + seconds
        ox = oy = 0.0
        while time.time() < end:
            dx, dy = dirs[(n // 10) % 4]
            motion(session, dx, dy)
            ox += dx
            oy += dy
            n += 1
            log(f"motion n={n} d={dx:+.0f},{dy:+.0f} offset={ox:+.0f},{oy:+.0f}")
            time.sleep(period)
    else:
        dx, dy = float(sys.argv[2]), float(sys.argv[3])
        motion(session, dx, dy)
        n = 1
        time.sleep(0.2)
        log(f"moved by {dx:+.0f},{dy:+.0f}")
finally:
    call(session, RD + ".Session", "Stop", None)
    log(f"stopped after {n} motions")
