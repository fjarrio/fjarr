# desktop-measure (throwaway, M2 slice 2b phase 2)

The criteria-table harness from docs/07. `stamp.py` (GTK4, Wayland; run it with
`GSK_RENDERER=cairo`) and `stamp_x11.py` (Xlib) paint the clock as a 24-bit bar
code on every frame and flip their lower half on each key. `measure.py --combo
A|B|C|D|E` reports paint→capture latency, input→photon latency and the 1080p30
capture + `vah264enc` cost against them. `inputd.py` is the root uinput helper
that B and D inject through. `hotplug_e.py` and `x11_cursor_hotplug.py` log what
each side sees when a monitor changes; `cursor_e.py` looks for cursor metadata
on E's buffers.
