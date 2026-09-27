#!/usr/bin/env python3
# A stand-in for fjarr-inputd (ADR-0009): the root helper that owns a uinput device and injects on
# request from the unprivileged agent over a unix socket. "k" = one space key press+release,
# "c" = one left click. Run as root. Throwaway spike code.
import os, socket, time
from evdev import UInput, ecodes as e

SOCK = "/run/fjarr-spike-inputd.sock"
ui = UInput({e.EV_KEY: [e.KEY_SPACE, e.BTN_LEFT], e.EV_REL: [e.REL_X, e.REL_Y]}, name="fjarr-spike-inputd")
time.sleep(1)
if os.path.exists(SOCK): os.unlink(SOCK)
srv = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM); srv.bind(SOCK); os.chmod(SOCK, 0o666)
while True:
    cmd = srv.recv(16)
    code = e.KEY_SPACE if cmd.startswith(b"k") else e.BTN_LEFT
    ui.write(e.EV_KEY, code, 1); ui.syn(); ui.write(e.EV_KEY, code, 0); ui.syn()
