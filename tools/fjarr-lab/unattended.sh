#!/usr/bin/env bash
# The unattended-access test, the industrial gate (docs/15#unattended-access-test-the-industrial-gate):
# a desktop robot reached with nobody at the machine, after a reboot. Two halves, because a job
# cannot outlive the reboot it causes:
#
#   sudo unattended.sh prepare <dir>   <dir> holds the build: *.deb, fjarr-server, fjarr-opsim. Undoes
#                                      the last `setup desktop`, installs the build, runs `setup desktop
#                                      --ghost-screens 1`, points the agent at a fjarr-server on this
#                                      machine (a drop-in: the person's fjarr.toml is never touched),
#                                      then `fjarr-lab reboot`.
#   sudo unattended.sh verify          after the reboot: each step in order, and a failure names the
#                                      first one that broke; then the agent goes back to its own server.
#                                      prepare installs fjarr-lab-unattended.service, which runs this
#                                      once at the next boot and writes the verdict.
#   sudo unattended.sh report          the verdict of the last verify, for the lab-desktop-verify
#                                      workflow: exit 0 passed, 1 failed, 75 not yet (no reboot since
#                                      prepare, or still verifying).
#
# Run by the lab-desktop-prepare and lab-desktop-verify workflows, and by hand on the machine.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
STATE=${FJARR_LAB_STATE:-/var/lib/fjarr-lab}/unattended
ACCOUNT=desktop
ROBOT=lab-desktop
PORT=18080
SERVER_UNIT=/etc/systemd/system/fjarr-lab-server.service
DROPIN=/etc/systemd/system/fjarr-agent.service.d/fjarr-lab.conf
ORACLE=/tmp/fjarr-oracle
VERIFY_UNIT=/etc/systemd/system/fjarr-lab-unattended.service
VERDICT=""   # set while verifying at boot: where fail/pass leave the outcome

say() { echo "unattended: $*"; }
fail() {
  echo "FAIL $1: $2"
  echo "SUMMARY unattended-access: failed at $1"
  if [ -n "$VERDICT" ]; then echo "failed: $1" > "$VERDICT"; fi
  exit 1
}
pass() { echo "PASS $1: $2"; }
# prepare and verify change the machine; report only reads its state.
need_root() { [ "$(id -u)" -eq 0 ] || { echo "unattended.sh $1 needs root (sudo)"; exit 2; }; }

boot_id() { cat /proc/sys/kernel/random/boot_id; }

prepare() {
  need_root prepare
  local dir=${1:?usage: unattended.sh prepare <dir with the .debs, fjarr-server and fjarr-opsim>}
  mkdir -p "$STATE"
  boot_id > "$STATE/boot-id"
  # The baseline: whatever the last run or a person set up for the desktop goes first.
  if command -v fjarr-agent >/dev/null; then fjarr-agent setup --undo desktop >/dev/null 2>&1 || true; fi
  local debs=()
  for d in "$dir"/fjarr-agent_*.deb "$dir"/fjarr-tools_*.deb "$dir"/fjarr-desktop-wayland_*.deb; do [ -f "$d" ] && debs+=("$(readlink -f "$d")"); done
  [ ${#debs[@]} -eq 3 ] || fail install "expected fjarr-agent, fjarr-tools and fjarr-desktop-wayland .debs in $dir"
  DEBIAN_FRONTEND=noninteractive apt-get install -y -q --reinstall --allow-downgrades "${debs[@]}" >"$STATE/apt.log" 2>&1 || { tail -20 "$STATE/apt.log"; fail install "apt could not install the build"; }
  install -m 0755 "$dir/fjarr-server" "$dir/fjarr-opsim" "$STATE/"
  install -m 0644 "$here/../../docker/desktop-fixture/testwin.py" "$STATE/testwin.py"
  install -m 0755 "$0" "$STATE/unattended.sh" # the checkout is gone by the next boot
  rm -f "$STATE/result" "$STATE/result.log"
  echo "${GITHUB_RUN_ID:-by hand}" > "$STATE/prepare-run"
  pass install "$(dpkg-query -W -f '${Package} ${Version}  ' fjarr-agent fjarr-desktop-wayland)"

  # A server of its own, on this machine, with secrets of its own for this run.
  local grant token
  grant=$(head -c 24 /dev/urandom | base64 | tr -d '/+=')
  token=$(head -c 24 /dev/urandom | base64 | tr -d '/+=')
  umask 077
  printf 'GRANT=%s\nTOKEN=%s\n' "$grant" "$token" > "$STATE/secrets"
  umask 022
  cat > "$SERVER_UNIT" <<EOF
# Written by tools/fjarr-lab/unattended.sh prepare; removed by verify (docs/15).
[Unit]
Description=fjarr-server for the unattended-access test
After=network.target
[Service]
ExecStart=$STATE/fjarr-server
Environment=FJARR_BIND=127.0.0.1:$PORT FJARR_GRANT_HS256_SECRET=$grant FJARR_DEV_DEVICE_TOKEN=$token
DynamicUser=yes
[Install]
WantedBy=multi-user.target
EOF
  mkdir -p "$(dirname "$DROPIN")"
  cat > "$DROPIN" <<EOF
# Written by tools/fjarr-lab/unattended.sh prepare; removed by verify. The agent's own config stays as
# it is: these override it for the test (docs/23: the environment wins over the file).
[Service]
Environment=FJARR_SERVER_URL=ws://127.0.0.1:$PORT/ws FJARR_ROBOT_ID=$ROBOT FJARR_DEV_DEVICE_TOKEN=$token
EOF
  systemctl daemon-reload
  systemctl enable --now fjarr-lab-server.service >/dev/null 2>&1
  systemctl restart fjarr-agent.service

  # The desktop, as a customer would set it up.
  fjarr-agent setup desktop --yes --account "$ACCOUNT" --ghost-screens 1 --reboot no >"$STATE/setup-desktop.log" 2>&1 \
    || { sed 's/\x1b\[[0-9;]*m//g' "$STATE/setup-desktop.log" | tail -25; fail "setup desktop" "it failed or its --check did"; }
  pass "setup desktop" "$(grep -oE '[A-Z]+[A-Za-z0-9-]* → Fjarr Ghost [0-9]+' "$STATE/setup-desktop.log" | head -1) · --check passed"
  # The machine verifies itself at the next boot, whenever GitHub gets round to asking (docs/15).
  {
    echo "# Written by tools/fjarr-lab/unattended.sh prepare: verify once at the next boot, then disable (docs/15)."
    echo "[Unit]"
    echo "Description=Fjarr unattended-access test: verify after the reboot"
    echo "After=network-online.target fjarr-agent.service fjarr-lab-server.service gdm.service"
    echo "Wants=network-online.target"
    echo "[Service]"
    echo "Type=oneshot"
    echo "ExecStart=$STATE/unattended.sh verify --on-boot"
    echo "TimeoutStartSec=15min"
    echo "[Install]"
    echo "WantedBy=multi-user.target"
  } > "$VERIFY_UNIT"
  systemctl daemon-reload
  systemctl enable fjarr-lab-unattended.service >/dev/null 2>&1
  pass "verify at boot" "fjarr-lab-unattended.service runs it once after the reboot"
  fjarr-lab reboot
  pass reboot "requested; fjarr-lab reboots once this job has ended"
}

wait_for() { # seconds, then a command; true once it succeeds
  local limit=$1; shift
  for _ in $(seq "$limit"); do "$@" >/dev/null 2>&1 && return 0; sleep 1; done
  return 1
}
seat_session() { loginctl list-sessions --no-legend | awk -v u="$ACCOUNT" '$3 == u && $4 == "seat0" {found=1} END {exit !found}'; }
agent_says() { journalctl -b -u fjarr-agent --no-pager -o cat | grep -qE "$1"; }

cleanup() {
  if [ -n "$VERDICT" ]; then
    [ -f "$VERDICT" ] && mv "$VERDICT" "$STATE/result"
    systemctl disable fjarr-lab-unattended.service >/dev/null 2>&1 || true
    rm -f "$VERIFY_UNIT"
  fi
  pkill -f "$ORACLE/testwin.py" 2>/dev/null || true
  pkill -f "http.server 8090" 2>/dev/null || true
  rm -f "$DROPIN"
  systemctl disable --now fjarr-lab-server.service >/dev/null 2>&1 || true
  rm -f "$SERVER_UNIT"
  systemctl daemon-reload
  systemctl restart fjarr-agent.service || true
  say "the agent is back on its own server; the desktop stays set up until the next prepare"
}

verify() {
  need_root verify
  if [ "${1:-}" = "--on-boot" ]; then
    VERDICT="$STATE/result.tmp"
    exec > >(tee "$STATE/result.log") 2>&1
    echo "unattended: verifying at boot $(boot_id), for prepare run $(cat "$STATE/prepare-run" 2>/dev/null)"
  fi
  trap cleanup EXIT
  [ -f "$STATE/boot-id" ] || fail rebooted "no prepare ran here ($STATE/boot-id missing)"
  [ "$(boot_id)" != "$(cat "$STATE/boot-id")" ] || fail rebooted "this is still the boot prepare ran in"
  pass rebooted "up since $(uptime -s)"
  wait_for 180 seat_session || fail "GDM automatic login" "no session of $ACCOUNT on seat0 within 3 min of the check starting"
  pass "GDM automatic login" "$ACCOUNT holds seat0, nobody logged in"
  wait_for 120 agent_says "helper connected" || fail "session helper" "the agent never heard from fjarr-desktop-session"
  pass "session helper" "connected to the agent"
  wait_for 60 agent_says "capture of .* ready" || fail "agent backend" "no capture became ready"
  pass "agent backend" "$(journalctl -b -u fjarr-agent --no-pager -o cat | grep -oE 'capture of .* ready[^,]*' | tail -1)"

  # The test window in the account's session: its log is the input oracle (docs/15).
  local uid
  uid=$(id -u "$ACCOUNT")
  install -d -m 1777 "$ORACLE"
  rm -f "$ORACLE/testwin.log"
  install -m 0644 "$STATE/testwin.py" "$ORACLE/testwin.py"
  # shellcheck disable=SC2024  # root writes the window's console log; the window runs as the account
  sudo -u "$ACCOUNT" setsid env XDG_RUNTIME_DIR="/run/user/$uid" WAYLAND_DISPLAY=wayland-0 \
    DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/$uid/bus" GDK_BACKEND=wayland FJARR_FIXTURE_LOG="$ORACLE/testwin.log" \
    python3 "$ORACLE/testwin.py" >"$ORACLE/testwin-app.log" 2>&1 </dev/null &
  setsid python3 -m http.server 8090 --bind 127.0.0.1 --directory "$ORACLE" >/dev/null 2>&1 </dev/null &
  wait_for 20 grep -q ' start ' "$ORACLE/testwin.log" || fail "test window" "it did not start: $(tail -3 "$ORACLE/testwin-app.log")"

  local GRANT=""
  # shellcheck source=/dev/null
  . "$STATE/secrets"
  local rc=0
  for scenario in desktop-see desktop-control; do
    "$STATE/fjarr-opsim" --server "ws://127.0.0.1:$PORT/ws" --robot "$ROBOT" --grant-secret "$GRANT" --scenario "$scenario" \
      --timeout 120 --desktop-oracle "http://127.0.0.1:8090/testwin.log" >"$STATE/$scenario.log" 2>&1 || rc=1
    grep -E '^(PASS|FAIL|SUMMARY)' "$STATE/$scenario.log"
  done
  [ $rc -eq 0 ] || fail stream "an opsim scenario failed (above; full logs in $STATE)"
  echo "SUMMARY unattended-access: passed"
  if [ -n "$VERDICT" ]; then echo "passed" > "$VERDICT"; fi
  return 0
}

# What lab-desktop-verify reports: the verdict the machine wrote at boot.
report() {
  if [ ! -f "$STATE/boot-id" ]; then echo "unattended: no prepare has run on this machine"; return 1; fi
  # The verdict must be for the prepare the workflow asks about: when tonight's prepare never reached
  # the machine, the state here is an older night's, and its "passed" was reported as tonight's once
  # (2026-10-03).
  if [ "${1:-}" = "--prepare-run" ]; then
    local want=${2:?--prepare-run needs a run id} have
    have=$(cat "$STATE/prepare-run" 2>/dev/null)
    if [ "$have" != "$want" ]; then
      echo "unattended: the machine was last prepared by run ${have:-unknown}, not run $want: that prepare never reached it"
      return 1
    fi
  fi
  if [ "$(boot_id)" = "$(cat "$STATE/boot-id")" ]; then
    echo "unattended: not rebooted since prepare (run $(cat "$STATE/prepare-run" 2>/dev/null)) yet"
    return 75
  fi
  if [ ! -f "$STATE/result" ]; then
    if systemctl is-active --quiet fjarr-lab-unattended.service || systemctl is-enabled --quiet fjarr-lab-unattended.service 2>/dev/null; then
      echo "unattended: rebooted; the machine is still verifying"
      return 75
    fi
    echo "unattended: rebooted, but no verdict was written (journalctl -u fjarr-lab-unattended)"
    return 1
  fi
  cat "$STATE/result.log" 2>/dev/null || true # a missing log must not swallow the verdict (set -e)
  echo "unattended: verdict for prepare run $(cat "$STATE/prepare-run" 2>/dev/null): $(cat "$STATE/result")"
  [ "$(cat "$STATE/result")" = "passed" ]
}

case "${1:-}" in
  prepare) shift; prepare "$@" ;;
  verify) shift; verify "$@" ;;
  report) shift; report "$@" ;;
  *) echo "usage: unattended.sh prepare <dir> | verify [--on-boot] | report [--prepare-run <id>]"; exit 2 ;;
esac
