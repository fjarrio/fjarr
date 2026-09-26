#!/usr/bin/env bash
# The three facts docs/27's lifecycle rule rests on, as a regression rather than a paragraph. They
# were measured once in a throwaway spike and never checked again, and the whole ordering
# requirement — the installer creates the device, the agent's unit starts before the robot's
# software — is built on them:
#
#   B  a participant created while the agent is ATTACHED reaches the peer over the tunnel
#   A  a participant created while the agent is DETACHED never does, however long it runs
#   C  a participant that was working keeps working across an agent restart
#
# Run from the host after `make tun-up ROS=1`. Needs the direct path removed, so it takes care of
# that itself; the tunnel session uses the relay because the direct path is what was removed.
# spec: docs/27-network-tunnel.md#lifecycle
set -euo pipefail
cd "$(dirname "$0")/../.."

HOLD=/tmp/fjarr-agent-hold
say() { printf '\n== %s\n' "$*"; }
robot_sh() { docker compose exec -T -u root demo-robot sh -c "$1"; }
restart_participant() {
  docker compose --profile demo --profile ros up -d --no-deps --force-recreate robot-ros >/dev/null
  for _ in $(seq 40); do
    docker compose --profile demo --profile ros logs --no-color robot-ros 2>/dev/null | grep -q "publishing /fjarr" && return 0
    sleep 1
  done
  return 1
}
# Does the operator see the robot's topic over a link? The answer is whether the scenario's `exec`
# check passed, NOT the simulator's exit status: other assertions in the scenario can fail for
# unrelated reasons (#28 among them), and with `pipefail` a non-zero opsim made this read "no topic"
# even when the topic was right there — which is how this script first reported two facts broken
# that were fine.
sees_topic() {
  local log; log=$(mktemp)
  docker compose exec -T dev ./build/release/agent/tools/fjarr-opsim \
    --server ws://fjarr-server:8080/ws --robot "${OPSIM_ROBOT:-demo-robot-01}" \
    --grant-secret "${FJARR_GRANT_HS256_SECRET:-dev-only-grant-secret}" \
    --introspect http://demo-robot:7381 --introspect-token "${FJARR_INTROSPECT_TOKEN:-dev-only-introspect-token}" \
    --timeout 240 --ice-policy relay --scenario tunnel \
    --exec 'docker/lab/tunnel-checks.sh ros2' >"$log" 2>&1 || true
  if grep -q "^PASS exec" "$log"; then rm -f "$log"; return 0; fi
  grep -E "^FAIL" "$log" | head -3 | sed 's/^/    /'
  rm -f "$log"; return 1
}
wait_online() {
  for _ in $(seq 60); do docker compose logs --no-color demo-robot 2>/dev/null | tail -40 | grep -q "hello-ack: online" && return 0; sleep 1; done
  return 1
}

trap 'robot_sh "rm -f $HOLD" >/dev/null 2>&1 || true; ./docker/lab/dds-isolate.sh off >/dev/null 2>&1 || true' EXIT
./docker/lab/dds-isolate.sh on >/dev/null
fail=0

say "B: a participant created while the agent is attached"
restart_participant || { echo "FAIL B: the participant never started"; exit 1; }
if sees_topic; then echo "PASS B: the topic is visible over the link"; else echo "FAIL B: attached at creation and still not visible"; fail=1; fi

say "C: the same participant across an agent restart"
robot_sh "pkill -f 'demos/demo-robot/demo-robot'" >/dev/null 2>&1 || true
wait_online || { echo "FAIL C: the agent did not come back"; exit 1; }
robot_sh "grep -c fjarr0 /proc/net/dev" >/dev/null || { echo "FAIL C: the interface did not survive the restart"; exit 1; }
if sees_topic; then echo "PASS C: still visible after the agent restarted, with no ROS restart"; else echo "FAIL C: the restart cost the participant its tunnel"; fail=1; fi

say "A: a participant created while the agent is detached"
robot_sh "touch $HOLD; pkill -f 'demos/demo-robot/demo-robot'" >/dev/null 2>&1 || true
sleep 3
robot_sh "grep -q fjarr0 /proc/net/dev" || { echo "FAIL A: the interface vanished with the agent"; exit 1; }
restart_participant || { echo "FAIL A: the participant never started"; exit 1; }
robot_sh "rm -f $HOLD" >/dev/null
wait_online || { echo "FAIL A: the agent did not come back"; exit 1; }
if sees_topic; then
  echo "FAIL A: it is visible, so docs/27's ordering rule is not the constraint it claims"; fail=1
else
  echo "PASS A: not visible — a participant created with the carrier down never sees the interface"
fi

say "restoring a working participant"
restart_participant >/dev/null || true
[ "$fail" = 0 ] && echo "ros-ordering: all three facts hold" || echo "ros-ordering: FAILURES above"
exit "$fail"
