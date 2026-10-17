#!/bin/sh
# as-desktop.sh COMMAND [ARGS...]  (root or a sudoer)
#
# Runs COMMAND as the logged-in desktop user, inside that user's session: its Wayland display,
# session D-Bus (for mutter's ScreenCast and RemoteDesktop APIs) and PipeWire. The capture
# clients, the test window and pointer.py need this; the KMS and CRC monitors run as root instead.
#
#   DESKTOP_USER   the session's user (default: desktop, as on the lab mini-PC)
#   WAYLAND_DISPLAY  default wayland-0
#   CACHE_DIR      if set, XDG_CACHE_HOME for COMMAND, so shader and GStreamer registry caches
#                  land there and not in the user's home
#
# Example: sudo ./as-desktop.sh python3 capclient.py metadata 30 /tmp/run1/cap.log
set -eu
U=${DESKTOP_USER:-desktop}
UD=$(id -u "$U")
set -- env XDG_RUNTIME_DIR="/run/user/$UD" WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}" \
  DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/$UD/bus" ${CACHE_DIR:+XDG_CACHE_HOME="$CACHE_DIR"} "$@"
exec sudo -u "$U" "$@"
