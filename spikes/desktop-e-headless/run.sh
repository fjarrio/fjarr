#!/bin/bash
# Inside the container, as an ordinary user: a session bus, PipeWire, headless mutter, the oracle,
# then the M2 probe against the primary (virtual) monitor. Prints what each step found.
set -u
export XDG_RUNTIME_DIR=/tmp/xdg-$(id -u); mkdir -p -m 700 "$XDG_RUNTIME_DIR"
exec dbus-run-session -- bash -c '
  pipewire >/tmp/pw.log 2>&1 &
  sleep 1; wireplumber >/tmp/wp.log 2>&1 &
  sleep 1
  mutter --headless --virtual-monitor 1280x720 --wayland >/tmp/mutter.log 2>&1 &
  for i in $(seq 30); do [ -S "$XDG_RUNTIME_DIR/wayland-0" ] && break; sleep 0.5; done
  echo "mutter: $( [ -S "$XDG_RUNTIME_DIR/wayland-0" ] && echo wayland socket up || echo NO wayland socket)"
  busctl --user list 2>/dev/null | grep -oE "org.gnome.Mutter.(RemoteDesktop|ScreenCast)" | sort -u | sed "s/^/dbus: /"
  export FJARR_ORACLE_LOG=/tmp/oracle.log
  WAYLAND_DISPLAY=wayland-0 GDK_BACKEND=wayland python3 /spike/oracle.py >/tmp/oracle-app.log 2>&1 &
  sleep 3
  python3 /spike/probe_e.py "" | grep -E "CAPTURE|pipewire node"
  : > /tmp/oracle.log
  python3 /spike/probe_eis.py 2>&1 | grep -E "eis:|INJECT|device"
  sleep 1
  echo "--- oracle log"; cat /tmp/oracle.log 2>/dev/null || echo "(no oracle log)"
  echo "--- mutter log (tail)"; tail -5 /tmp/mutter.log
'
