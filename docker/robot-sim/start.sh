#!/usr/bin/env bash
# Boot the fake robot desktop: Xvfb + openbox + moving apps + noVNC viewer.
# `start.sh dump` prints the X11 fixture's kiosk session instead, for a failed suite's log.
set -u

: "${DISPLAY:=:99}"
: "${SIM_RESOLUTION:=1920x1080x24}"
# The X11 fixture's kiosk account, as its session runs (M3.5 3.5.2, ADR-0034).
KIOSK=(setpriv --reuid=kiosk --regid=kiosk --init-groups env XDG_RUNTIME_DIR=/run/kiosk HOME=/home/kiosk
       DBUS_SESSION_BUS_ADDRESS=unix:path=/run/kiosk/bus DISPLAY="${DISPLAY}" XAUTHORITY=/run/fixture/xauth)

# What a failure needs to show (the readiness check below, and make x11-e2e when it fails): whether
# WirePlumber runs, the kiosk session's logs, its links, and the session manager's view.
# spec: docs/15-testing-strategy.md#the-desktop-test-lab
dump() {
  local f
  echo "robot-sim: wireplumber: $(pgrep -a wireplumber || echo "not running")"
  for f in wireplumber pipewire session; do
    [ -f "/run/fixture/$f.log" ] || continue
    echo "--- /run/fixture/$f.log (last 50 lines)"
    tail -n 50 "/run/fixture/$f.log"
  done
  echo "--- pw-link -l"
  "${KIOSK[@]}" timeout 5 pw-link -l 2>&1
  echo "--- wpctl status"
  "${KIOSK[@]}" timeout 5 wpctl status 2>&1 | grep -v ' mod\.rt '
}
if [ "${1:-}" = dump ]; then dump; exit 0; fi

# The kiosk's PipeWire, then its WirePlumber once PipeWire answers on the session manager's socket,
# restarted when it fails and every exit logged: the mutter fixture's start (session.sh), which is
# what systemd does on a robot (docs/15#the-desktop-test-lab).
pipewire_answers() { [ -S /run/kiosk/pipewire-0-manager ] && "${KIOSK[@]}" timeout 2 pw-cli -r pipewire-0-manager info 0 >/dev/null 2>&1; }
start_pipewire() { # [late|never]: test-only, PipeWire starts a second late or not at all
  local deadline=$((SECONDS + 10))
  case "${1:-}" in
  late) { sleep 1; exec "${KIOSK[@]}" pipewire; } >/run/fixture/pipewire.log 2>&1 & ;;
  never) : >/run/fixture/pipewire.log ;;
  *) "${KIOSK[@]}" pipewire >/run/fixture/pipewire.log 2>&1 & ;;
  esac
  while [ $SECONDS -lt $deadline ]; do pipewire_answers && return; sleep 0.1; done
  echo "robot-sim: the kiosk's PipeWire never answered on pipewire-0-manager"; tail -n 20 /run/fixture/pipewire.log; exit 1
}
supervise_wireplumber() {
  local t0 t1 rc note failed=()
  while :; do
    t0=$EPOCHREALTIME
    rc=0; "${KIOSK[@]}" wireplumber >>/run/fixture/wireplumber.log 2>&1 || rc=$?
    t1=$EPOCHREALTIME
    note=""; [ "$rc" -ne 0 ] || note=", a clean exit: not restarted"
    # The time from bash's clock, with date for whole seconds only: 26.04's date (uutils) misprints %6N.
    echo "robot-sim: wireplumber exited with status $rc after $(( (${t1/[.,]/} - ${t0/[.,]/}) / 1000 )) ms at $(date -u -d "@${t1%[.,]*}" +%FT%T).${t1#*[.,]}Z$note" \
      | tee -a /run/fixture/wireplumber.log
    [ "$rc" -ne 0 ] || return 0
    # Once the session is ready, systemd's start limit (StartLimitBurst=5 in 10 s): a WirePlumber that
    # crashes in a loop is given up, said once, rather than filling its log. Before ready, the start's
    # own deadlines bound it, and the race above must be able to recover.
    if [ -f /run/fixture/ready ]; then
      failed+=("$SECONDS")
      while [ $((SECONDS - failed[0])) -ge 10 ]; do failed=("${failed[@]:1}"); done
      if [ ${#failed[@]} -ge 5 ]; then
        echo "robot-sim: wireplumber: start limit hit (5 failures within 10 s), not restarted" | tee -a /run/fixture/wireplumber.log
        return 1
      fi
    fi
    sleep 0.5
  done
}

# The /tmp/.X11-unix volume is shared with the dev container; make sure a
# stale socket from a previous run doesn't block Xvfb. The lock file lives
# in the container's own /tmp and survives an unclean stop (host reboot,
# docker restart): "Server is already active for display 99" otherwise.
rm -f "/tmp/.X11-unix/X${DISPLAY#:}" "/tmp/.X${DISPLAY#:}-lock" 2>/dev/null || true
mkdir -p /tmp/.X11-unix && chmod 1777 /tmp/.X11-unix

if [ "${SIM_FIXTURE:-0}" = 1 ]; then
  # The X11 fixture (docs/15#the-desktop-test-lab, M3 3.7): access control on, a root wide enough to
  # carve a second monitor from (Xvfb's root cannot grow), and the agent's account granted the way a
  # kiosk session grants it (`xhost +si:localuser:fjarr`, docs/23#desktop-x11).
  SIM_RESOLUTION=3840x1080x24
  rm -f /run/fixture/ready # a restarted container's is the last start's
  mkdir -p /run/fixture/oracle && : > /run/fixture/oracle/testwin.log
  xauth -f /run/fixture/xauth add "${DISPLAY}" . "$(head -c 16 /dev/urandom | od -An -tx1 | tr -d ' \n')"
  # -noreset: Xvfb regenerates when its last client leaves, which wiped the grant and the carved
  # monitor before openbox connected (a real kiosk session always holds clients).
  AUTH=(-auth /run/fixture/xauth -noreset)
  export XAUTHORITY=/run/fixture/xauth
fi

echo "robot-sim: starting Xvfb ${DISPLAY} at ${SIM_RESOLUTION}"
Xvfb "${DISPLAY}" -screen 0 "${SIM_RESOLUTION}" -nolisten tcp ${AUTH[@]+"${AUTH[@]}"} &
XVFB_PID=$!

# Wait for the X server to accept connections.
for _ in $(seq 1 50); do
  if xdpyinfo -display "${DISPLAY}" >/dev/null 2>&1; then break; fi
  sleep 0.2
done

if [ "${SIM_FIXTURE:-0}" = 1 ]; then
  xrandr --setmonitor LEFT 1920/500x1080/280+0+0 screen
  # The package's session program grants the agent's account and keeps the outputs laid out, as the
  # kiosk session's autostart entry runs it (docs/23#desktop-x11); built in /workspace (make agent-build).
  SESSION=/workspace/build/release/agent/fjarr-x11-session
  [ -x "$SESSION" ] || { echo "robot-sim: $SESSION is not built: make agent-build"; exit 1; }
  # The kiosk's session runs as its own account (M3.5 3.5.2, ADR-0034): a session bus, PipeWire and
  # WirePlumber with the fixture's speakers, and the session program, which hands the agent the sound
  # over /run/fjarr/desktop.sock (a tmpfs shared with x11-robot, whose agent makes the socket with
  # group fjarr-desktop). Xvfb and the test window stay root's.
  install -d -o kiosk -g kiosk -m 0700 /run/kiosk
  chmod 0644 /run/fixture/xauth
  "${KIOSK[@]}" dbus-daemon --session --address=unix:path=/run/kiosk/bus --nofork --nopidfile >/run/fixture/dbus.log 2>&1 &
  for _ in $(seq 50); do [ -S /run/kiosk/bus ] && break; sleep 0.1; done
  : >/run/fixture/wireplumber.log
  # FIXTURE_FAULT is test-only (docker/lab/fixture-startup-test.sh), as in session.sh: wireplumber-first,
  # pipewire-late, no-session-manager, no-pipewire.
  case "${FIXTURE_FAULT:-}" in
  "") start_pipewire; supervise_wireplumber & ;;
  wireplumber-first) supervise_wireplumber & sleep 1; start_pipewire ;;
  pipewire-late) start_pipewire late; supervise_wireplumber & ;;
  no-session-manager) start_pipewire ;;
  no-pipewire) start_pipewire never ;;
  *) echo "robot-sim: unknown FIXTURE_FAULT=$FIXTURE_FAULT"; exit 1 ;;
  esac
  "${KIOSK[@]}" FJARR_DESKTOP_SOCK=/run/fjarr/desktop.sock "$SESSION" --account fjarr >/run/fixture/session.log 2>&1 &
  for _ in $(seq 1 50); do grep -q "granted" /run/fixture/session.log 2>/dev/null && break; sleep 0.1; done
  cat /run/fixture/session.log
  openbox &
  sleep 0.5
  testwin_x11.py >/run/fixture/testwin.err 2>&1 &
  python3 -m http.server 8090 --bind 0.0.0.0 --directory /run/fixture/oracle >/dev/null 2>&1 &
  plugd_x11.py &
  # Ready once the kiosk has a default output, as a robot's session has one by the time anyone connects.
  # None after 20 s by the clock means no working session manager (CI 2026-10-12 ran 30 s with none,
  # docs/15): fail here, loudly, and `up --wait` fails, rather than the suites with no sound.
  deadline=$((SECONDS + 20))
  until "${KIOSK[@]}" wpctl inspect @DEFAULT_AUDIO_SINK@ >/dev/null 2>&1; do
    [ $SECONDS -lt $deadline ] || { echo "robot-sim: no default output"; dump; exit 1; }
    sleep 0.1
  done
  touch /run/fixture/ready
  echo "robot-sim: X11 fixture ready (3840x1080 root, monitor LEFT, oracle :8090, plugd :8091)"
  wait "${XVFB_PID}"
  exit
fi

openbox &
# Moving content so captured video is never a static frame:
xclock -update 1 -geometry 300x300+50+50 &
xeyes -geometry 200x200+400+80 &
# No glxgears: on Xvfb it renders unthrottled through llvmpipe, burning ~3.5 cores for ever and
# pushing the laptop into package-power throttling — which pinned the iGPU (and the VA-API
# encoder under test) to its floor. The clock's second hand and the heartbeat terminal keep the
# desktop moving at 1 Hz, which is what capture tests need.
xterm -geometry 100x12+400+340 -e \
  'while true; do date "+robot-sim heartbeat  %F %T"; sleep 1; done' &

echo "robot-sim: starting x11vnc + noVNC on :6080"
x11vnc -display "${DISPLAY}" -forever -shared -nopw -quiet -bg
websockify --web=/usr/share/novnc 6080 localhost:5900 &

wait "${XVFB_PID}"
