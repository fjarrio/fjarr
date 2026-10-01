#!/bin/bash
# The fixture's desktop session, in the order a real login brings it up: a session bus at a fixed
# address (so `docker compose exec` joins the same session), PipeWire and WirePlumber, headless
# mutter with one virtual monitor, then the test window. /run/desktop/ready marks it usable.
# spec: docs/15-testing-strategy.md#the-desktop-test-lab
set -euo pipefail
rm -f /run/desktop/ready
dbus-daemon --session --address="$DBUS_SESSION_BUS_ADDRESS" --nofork --nopidfile >/run/desktop/dbus.log 2>&1 &
for _ in $(seq 50); do [ -S /run/desktop/bus ] && break; sleep 0.1; done
pipewire >/run/desktop/pipewire.log 2>&1 &
wireplumber >/run/desktop/wireplumber.log 2>&1 &
mutter --headless --virtual-monitor "${FIXTURE_WIDTH}x${FIXTURE_HEIGHT}" --wayland >/run/desktop/mutter.log 2>&1 &
mutter_pid=$!
for _ in $(seq 60); do [ -S "/run/desktop/$WAYLAND_DISPLAY" ] && break; sleep 0.5; done
[ -S "/run/desktop/$WAYLAND_DISPLAY" ] || { echo "fixture: mutter never opened its Wayland socket"; tail -20 /run/desktop/mutter.log; exit 1; }
for _ in $(seq 40); do
  busctl --user list 2>/dev/null | grep -q org.gnome.Mutter.ScreenCast && break; sleep 0.25
done
: > "$FJARR_FIXTURE_LOG"
GDK_BACKEND=wayland fixture-testwin >/run/desktop/testwin-app.log 2>&1 &
for _ in $(seq 40); do grep -q '^.* start ' "$FJARR_FIXTURE_LOG" && break; sleep 0.25; done
grep -q ' start ' "$FJARR_FIXTURE_LOG" || { echo "fixture: the test window never started"; cat /run/desktop/testwin-app.log; exit 1; }
# The window's log over HTTP on the compose network, for oracles that run elsewhere (opsim's
# desktop-control scenario). Only the log is in the served directory.
mkdir -p /run/desktop/oracle && ln -sf "$FJARR_FIXTURE_LOG" /run/desktop/oracle/testwin.log
python3 -m http.server 8090 --bind 0.0.0.0 --directory /run/desktop/oracle >/run/desktop/oracle-http.log 2>&1 &
# The session helper, as a real desktop session starts it (a user unit on a robot, ADR-0028): only
# when an agent's socket directory is shared in (make desktop-see), so the self-tests run without one.
if [ -d /run/fjarr ]; then
  FJARR_DESKTOP_SOCK=/run/fjarr/desktop.sock fjarr-desktop-session >/run/desktop/helper-live.log 2>&1 &
fi
touch /run/desktop/ready
echo "fixture: desktop ${FIXTURE_WIDTH}x${FIXTURE_HEIGHT} ready (mutter $(mutter --version 2>/dev/null | head -1 | awk '{print $2}'))"
wait "$mutter_pid"
