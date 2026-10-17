#!/bin/bash
# The fixture's desktop session, in the order a real login brings it up: a session bus at a fixed
# address (so `docker compose exec` joins the same session), PipeWire and WirePlumber, headless
# mutter with one virtual monitor, then the test window. /run/desktop/ready marks it usable.
# `fixture-session dump` prints the session's internals instead, for a failed suite's log.
# spec: docs/15-testing-strategy.md#the-desktop-test-lab
set -euo pipefail

# What a failure needs to show (the readiness check below, and make desktop-e2e, desktop-browser,
# desktop-fixture-test and the compose gate when they fail): whether WirePlumber runs, the session
# daemons' logs, the links, and the session manager's view.
dump() {
  local f
  echo "fixture: wireplumber: $(pgrep -a wireplumber || echo "not running")"
  for f in wireplumber pipewire helper-live mutter; do
    [ -f "/run/desktop/$f.log" ] || continue
    echo "--- /run/desktop/$f.log (last 50 lines)"
    tail -n 50 "/run/desktop/$f.log" || true
  done
  echo "--- pw-link -l"
  timeout 5 pw-link -l 2>&1 || true
  echo "--- wpctl status"
  timeout 5 wpctl status 2>&1 | grep -v ' mod\.rt ' || true
}
if [ "${1:-}" = dump ]; then dump; exit 0; fi

# PipeWire, then WirePlumber once PipeWire answers on the session manager's socket, restarted when it
# fails: what systemd does on a robot (pipewire.socket; wireplumber.service is After= and BindsTo=
# pipewire.service with Restart=on-failure). WirePlumber connects once and exits for good, status 69,
# when nothing listens yet. Started together, as they were, a fixture ran on without a session
# manager in about 2 % of CI starts (2026-10-03 to 10-14), most likely through that race; every exit
# is logged, with its status and how long it ran, so the next one says what happened.
pipewire_answers() { [ -S /run/desktop/pipewire-0-manager ] && timeout 2 pw-cli -r pipewire-0-manager info 0 >/dev/null 2>&1; }
start_pipewire() { # [late|never]: test-only, PipeWire starts a second late or not at all
  local deadline=$((SECONDS + 10))
  case "${1:-}" in
  late) { sleep 1; exec pipewire; } >/run/desktop/pipewire.log 2>&1 & ;;
  never) : >/run/desktop/pipewire.log ;;
  *) pipewire >/run/desktop/pipewire.log 2>&1 & ;;
  esac
  while [ $SECONDS -lt $deadline ]; do pipewire_answers && return; sleep 0.1; done
  echo "fixture: PipeWire never answered on pipewire-0-manager"; tail -n 20 /run/desktop/pipewire.log; exit 1
}
supervise_wireplumber() {
  local t0 t1 rc note failed=()
  while :; do
    t0=$EPOCHREALTIME
    rc=0; wireplumber >>/run/desktop/wireplumber.log 2>&1 || rc=$?
    t1=$EPOCHREALTIME
    note=""; [ "$rc" -ne 0 ] || note=", a clean exit: not restarted"
    # The time from bash's clock, with date for whole seconds only: 26.04's date (uutils) misprints %6N.
    echo "fixture: wireplumber exited with status $rc after $(( (${t1/[.,]/} - ${t0/[.,]/}) / 1000 )) ms at $(date -u -d "@${t1%[.,]*}" +%FT%T).${t1#*[.,]}Z$note" \
      | tee -a /run/desktop/wireplumber.log
    [ "$rc" -ne 0 ] || return 0
    # Once the session is ready, systemd's start limit (StartLimitBurst=5 in 10 s): a WirePlumber that
    # crashes in a loop is given up, said once, rather than filling its log. Before ready, the start's
    # own deadlines bound it, and the race above must be able to recover.
    if [ -f /run/desktop/ready ]; then
      failed+=("$SECONDS")
      while [ $((SECONDS - failed[0])) -ge 10 ]; do failed=("${failed[@]:1}"); done
      if [ ${#failed[@]} -ge 5 ]; then
        echo "fixture: wireplumber: start limit hit (5 failures within 10 s), not restarted" | tee -a /run/desktop/wireplumber.log
        return 1
      fi
    fi
    sleep 0.5
  done
}

rm -f /run/desktop/ready
dbus-daemon --session --address="$DBUS_SESSION_BUS_ADDRESS" --nofork --nopidfile >/run/desktop/dbus.log 2>&1 &
for _ in $(seq 50); do [ -S /run/desktop/bus ] && break; sleep 0.1; done
: >/run/desktop/wireplumber.log
# FIXTURE_FAULT is test-only (docker/lab/fixture-startup-test.sh): wireplumber-first starts WirePlumber
# a second before PipeWire, the race made certain; pipewire-late keeps the order but starts PipeWire a
# second late, which only the wait above keeps WirePlumber from racing; no-session-manager never starts
# WirePlumber, and no-pipewire never starts PipeWire.
case "${FIXTURE_FAULT:-}" in
"") start_pipewire; supervise_wireplumber & ;;
wireplumber-first) supervise_wireplumber & sleep 1; start_pipewire ;;
pipewire-late) start_pipewire late; supervise_wireplumber & ;;
no-session-manager) start_pipewire ;;
no-pipewire) start_pipewire never ;;
*) echo "fixture: unknown FIXTURE_FAULT=$FIXTURE_FAULT"; exit 1 ;;
esac
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
# Monitor hot-plug on request (3.4): mutter virtual monitors, plugged and unplugged by the oracles.
fixture-plugd >/run/desktop/plugd.log 2>&1 &
# The session helper, as a real desktop session starts it (a user unit on a robot, ADR-0028): only
# when an agent's socket directory is shared in (make desktop-see), so the self-tests run without one.
if [ -d /run/fjarr ]; then
  FJARR_DESKTOP_SOCK=/run/fjarr/desktop.sock fjarr-desktop-session >/run/desktop/helper-live.log 2>&1 &
fi
# Ready once the session has a default output (the fixture's speakers, M3.5 3.5.2), as a robot's has.
# None after 20 s (by the clock: a loaded runner makes each wpctl slow) means no working session
# manager, so nothing links a capture either: fail here, loudly, and `up --wait` fails, rather than
# every suite a minute later with "no frames".
deadline=$((SECONDS + 20))
until wpctl inspect @DEFAULT_AUDIO_SINK@ >/dev/null 2>&1; do
  [ $SECONDS -lt $deadline ] || { echo "fixture: no default output"; dump; exit 1; }
  sleep 0.1
done
touch /run/desktop/ready
echo "fixture: desktop ${FIXTURE_WIDTH}x${FIXTURE_HEIGHT} ready (mutter $(mutter --version 2>/dev/null | head -1 | awk '{print $2}'))"
wait "$mutter_pid"
