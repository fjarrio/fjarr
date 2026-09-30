#!/usr/bin/env bash
# make compose-gate — the reference compose file proven (docs/26#containerized-robots, M2.5 gate):
# setup inside the image, --check, a ROS container started BEFORE the agent that waits for carrier,
# an agent without privileges, a restart the device survives, and real IP over the tunnel.
#
# Host-run, like every target that creates containers (CLAUDE.md). Needs the lab stack (fjarr-server
# and dev) and the .debs in dist/deb/<arch>. FJARR_GATE_SANDBOX=0 runs the file with real host
# networking (CI); the default keeps fjarr0 out of a workstation's own namespace.
set -euo pipefail
cd "$(dirname "$0")/../.."

ARCH=${DEB_ARCH:-$(dpkg --print-architecture 2>/dev/null || echo amd64)}
export FJARR_GATE_IMAGE=${FJARR_GATE_IMAGE:-fjarr-agent:compose-gate}
SANDBOX=${FJARR_GATE_SANDBOX:-1}
ID=${FJARR_GATE_DEVICE:-compose-robot-01}
TOKEN=${FJARR_DEV_DEVICE_TOKEN:-dev-only-device-token}
OP_DEV=fjarr1 # the operator's end in dev; fjarr0 there belongs to the lab's demo robot
files=(-f packaging/compose/docker-compose.yml -f packaging/compose/gate.override.yml)
if [ "$SANDBOX" = 1 ]; then
  files+=(-f packaging/compose/gate.sandbox.yml)
  SERVER=ws://fjarr-server:8080/ws
else
  SERVER=ws://127.0.0.1:8080/ws
fi
dc() { docker compose -p fjarr-compose-gate "${files[@]}" "$@"; }
say() { echo "compose-gate: $*"; }
# Captured first: `logs | grep -q` fails under pipefail when grep exits early and logs gets SIGPIPE.
logs_have() {
  local out
  out=$(dc --profile example logs --no-color "$1" 2>&1)
  grep -q -- "$2" <<<"$out"
}
fail() {
  echo "compose-gate: FAIL $*" >&2
  dc --profile example logs --no-color --tail 40 >&2 || true
  exit 1
}
cleanup() {
  dc --profile example down -v >/dev/null 2>&1 || true
  docker/lab/tundev.sh down dev "$OP_DEV" >/dev/null 2>&1 || true
}
trap cleanup EXIT

if [ "$SANDBOX" != 1 ] && ss -ltnH 2>/dev/null | awk '{print $4}' | grep -qE '(^|:)7381$'; then
  fail "host networking: port 7381 is taken on this host (the lab's demo-robot publishes it: docker compose stop demo-robot)"
fi
ls dist/deb/"$ARCH"/fjarr-agent_*.deb >/dev/null 2>&1 || fail "no dist/deb/$ARCH/fjarr-agent_*.deb — run 'make deb' first"
say "image $FJARR_GATE_IMAGE from dist/deb/$ARCH"
docker build -q -f docker/agent/Dockerfile --target runtime -t "$FJARR_GATE_IMAGE" . >/dev/null
cleanup
[ "$SANDBOX" = 1 ] && dc up -d gate-host >/dev/null

# 1. The first run: the setup tool inside the image writes the configuration into the volume.
out=$(dc run --rm -T fjarr-agent setup --yes --server "$SERVER" --device-id "$ID" --token "$TOKEN" \
  --encoder software --cameras none --terminal none --net yes --ros no 2>&1) || fail "setup in the image: $out"
say "setup ok: $(echo "$out" | tail -1)"

# 2. ROS first — the order a reboot may produce. It must wait, not start.
dc --profile example up -d --no-deps ros >/dev/null
sleep 3
logs_have ros ros-start && fail "ROS started before the agent had attached fjarr0"
say "ROS container is up and waiting for carrier"

# 3. The agent: healthy, and ROS then starts with carrier.
dc up -d --no-deps fjarr-agent >/dev/null
for _ in $(seq 60); do
  [ "$(docker inspect -f '{{.State.Health.Status}}' "$(dc ps -q fjarr-agent)")" = healthy ] && break
  sleep 1
done
[ "$(docker inspect -f '{{.State.Health.Status}}' "$(dc ps -q fjarr-agent)")" = healthy ] || fail "the agent never became healthy"
for _ in $(seq 30); do logs_have ros ros-start && break; sleep 1; done
line=$(dc --profile example logs --no-color ros 2>&1 | grep ros-start | head -1 || true)
echo "$line" | grep -q "carrier=1" || fail "ROS did not start with carrier on fjarr0: '${line:-no start}'"
say "agent healthy; ROS started after it, with carrier ($line)"

# 4. --check, and the agent holds nothing it should not.
dc exec -T -u fjarr fjarr-agent fjarr-agent --check >/dev/null || fail "--check: $(dc exec -T -u fjarr fjarr-agent fjarr-agent --check 2>&1 | tail -5)"
status=$(dc exec -T fjarr-agent cat /proc/1/status)
echo "$status" | grep -qE '^Uid:\s+10001\s' || fail "the agent is not uid 10001: $(echo "$status" | grep ^Uid)"
echo "$status" | grep -qE '^CapEff:\s+0+$' || fail "the agent has effective capabilities: $(echo "$status" | grep ^Cap)"
echo "$status" | grep -qE '^CapBnd:\s+0+$' || fail "the agent's bounding set is not empty: $(echo "$status" | grep ^CapBnd)"
say "--check ok; agent is uid 10001 with no capabilities"

# 5. An agent restart: the device survives, carrier comes back, ROS keeps running.
ros_id=$(dc --profile example ps -q ros)
dc restart fjarr-agent >/dev/null
for _ in $(seq 60); do
  [ "$(docker inspect -f '{{.State.Health.Status}}' "$(dc ps -q fjarr-agent)")" = healthy ] && break
  sleep 1
done
[ "$(docker exec "$ros_id" cat /sys/class/net/fjarr0/carrier 2>/dev/null)" = 1 ] || fail "no carrier on fjarr0 after the agent restarted"
[ "$(docker inspect -f '{{.State.Running}} {{.RestartCount}}' "$ros_id")" = "true 0" ] || fail "the ROS container did not run straight through the agent's restart"
logs_have fjarr-agent "already there" || fail "the restart did not find the device already there"
say "agent restarted; fjarr0 survived, ROS ran straight through"

# 6. Real IP over the tunnel, from an operator.
addr=$(dc exec -T -u fjarr fjarr-agent fjarr-agent --net-address | tail -1 | tr -d '\r')
docker/lab/tundev.sh up dev 100.64.0.1 "$addr" "$OP_DEV" 1184 >/dev/null
docker compose exec -T -e FJARR_TUN_DEV="$OP_DEV" dev ./build/"${BUILD_PRESET:-release}"/agent/tools/fjarr-opsim --server ws://fjarr-server:8080/ws \
  --robot "$ID" --grant-secret "${FJARR_GRANT_HS256_SECRET:-dev-only-grant-secret}" --timeout 60 --scenario tunnel \
  --introspect-token dev-only-introspect-token 2>&1 | tee /tmp/compose-gate-opsim.log | grep -E '^(PASS|FAIL|SUMMARY)'
grep -q "0 failed" /tmp/compose-gate-opsim.log || fail "the tunnel scenario against the compose robot"
say "PASS — the reference compose file runs a robot, tunnel included"
