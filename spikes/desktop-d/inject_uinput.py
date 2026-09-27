#!/usr/bin/env python3
# Candidate D's input half (docs/07): a root helper creates a uinput keyboard+mouse and injects a
# known sequence below the compositor — no portal, no session credentials. The oracle must log it.
# Run as root. Throwaway spike code.
import time
from evdev import UInput, ecodes as e

caps = {e.EV_KEY: [e.KEY_D, e.KEY_U, e.BTN_LEFT, e.BTN_RIGHT], e.EV_REL: [e.REL_X, e.REL_Y]}
with UInput(caps, name="fjarr-spike-uinput") as ui:
    time.sleep(1.5)  # libinput must see the device before events count
    for k in (e.KEY_D, e.KEY_U):
        ui.write(e.EV_KEY, k, 1); ui.syn(); ui.write(e.EV_KEY, k, 0); ui.syn()
    ui.write(e.EV_REL, e.REL_X, 40); ui.write(e.EV_REL, e.REL_Y, 30); ui.syn()
    ui.write(e.EV_KEY, e.BTN_LEFT, 1); ui.syn(); ui.write(e.EV_KEY, e.BTN_LEFT, 0); ui.syn()
    time.sleep(0.5)
print("INJECT sent via uinput: key d, key u, relative move, click — read the oracle log")
