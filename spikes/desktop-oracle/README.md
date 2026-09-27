# desktop-oracle (throwaway, M2 slice 2b)

The injection oracle every desktop spike reads back through (docs/07, phase 1).
`oracle.py` is autostarted in the spike session from `fjarr-oracle.desktop`; it
paints the screen magenta and appends each key and click it receives to
`~/fjarr-oracle.log`. Installed on the spike machine at `/opt/fjarr-spike/`.

On X11 sessions the oracle is `oracle_x11.py` instead: the GTK4 window neither
painted nor reliably took focus under Openbox, so a bare Xlib window with a
server-painted background replaces it there.
