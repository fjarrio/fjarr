#!/bin/sh
# mutterlog.sh SINCE_EPOCH OUTLOG  (root, or a user who can read the desktop user's journal)
#
# Extracts gnome-shell's MUTTER_DEBUG lines (topics RENDER, SCREEN_CAST, KMS, BACKEND) since
# SINCE_EPOCH from the journal, as "<epoch> <text>" lines that wltimeline.py merges as MUT. The
# decisive ones: "RENDER: Assigning scanout to stage view" (a client buffer goes straight to KMS),
# "RENDER: No direct scanout candidate: <reason>", and "SCREEN_CAST: Recording full frame on
# stream N" / "Recording cursor frame on stream N". Writes nothing if the session has no such lines.
#
# The session must run with MUTTER_DEBUG set, which needs a gnome-shell restart. On Ubuntu 26.04
# (GDM autologin) the spike used a user drop-in and a GDM restart, removed again afterwards:
#   ~DESKTOP_USER/.config/systemd/user/org.gnome.Shell@ubuntu.service.d/zz-debug.conf
#     [Service]
#     Environment=MUTTER_DEBUG=render,screen-cast
#   then: systemctl --user daemon-reload (as that user), and sudo systemctl restart gdm
# The same drop-in carried the levers MUTTER_DEBUG_PAINT=disable-direct-scanout and
# MUTTER_DEBUG_DISABLE_HW_CURSORS=1. Under MUTTER_DEBUG, journald may rate-limit gnome-shell
# ("Suppressed N messages"): check for gaps before reading absence of a line as evidence.
#
#   DESKTOP_USER   default desktop
#
# Example: sudo ./mutterlog.sh "$(cat runs/r1/t0)" runs/r1/mutter.log
set -eu
SINCE=${1%.*}
OUT=$2
UD=$(id -u "${DESKTOP_USER:-desktop}")
journalctl _UID="$UD" --since "@$SINCE" -o short-unix --no-pager 2>/dev/null \
  | grep -E 'gnome-shell\[[0-9]+\]: (RENDER|SCREEN_CAST|KMS|BACKEND):' \
  | sed -E 's/^([0-9.]+) [^ ]+ gnome-shell\[[0-9]+\]: /\1 /' > "$OUT" || true
[ -s "$OUT" ] || rm -f "$OUT"
