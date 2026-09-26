#!/usr/bin/env bash
# The docs/27 gate's own commands, run with a tunnel link up and $FJARR_ADDR pointing at the robot.
# Meant to be handed to `fjarr-opsim --scenario tunnel --exec`, which is the rehearsal of
# `fjarr-connect <robot> -- <command>`: nothing in here knows it is inside a WebRTC data channel.
#
#   tunnel-checks.sh ssh    a shell on the robot over the link
#   tunnel-checks.sh scp    pull the payload and verify its sha256 end to end
#
# spec: docs/27-network-tunnel.md#testing
set -euo pipefail

: "${FJARR_ADDR:?FJARR_ADDR is not set — run this through fjarr-opsim --exec}"
KEY=${FJARR_LAB_KEY:-/srv/fjarr-lab-key/id_ed25519}
# A throwaway lab key and a host that changes with every container: host-key checking would only
# ever produce noise here, and the link itself is authenticated by the session grant (docs/10).
SSH_OPTS=(-i "$KEY" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR
          -o ConnectTimeout=20 -o ServerAliveInterval=5)

case "${1:-}" in
ssh)
  out=$(ssh "${SSH_OPTS[@]}" "robot@$FJARR_ADDR" 'echo "shell:$(id -un)@$(hostname) addr=$(hostname -i | cut -d" " -f1)"')
  echo "tunnel-checks: $out"
  case "$out" in *"shell:robot@"*) ;; *) echo "tunnel-checks: unexpected shell output" >&2; exit 1 ;; esac
  ;;
scp)
  dest=$(mktemp -d)
  trap 'rm -rf "$dest"' EXIT
  want=$(ssh "${SSH_OPTS[@]}" "robot@$FJARR_ADDR" 'cat /srv/fjarr-lab/payload.bin.sha256')
  size=$(ssh "${SSH_OPTS[@]}" "robot@$FJARR_ADDR" 'stat -c %s /srv/fjarr-lab/payload.bin')
  t0=$(date +%s.%N)
  scp "${SSH_OPTS[@]}" -q "robot@$FJARR_ADDR:/srv/fjarr-lab/payload.bin" "$dest/payload.bin"
  t1=$(date +%s.%N)
  got=$(sha256sum "$dest/payload.bin" | awk '{print $1}')
  mbps=$(awk -v s="$size" -v a="$t0" -v b="$t1" 'BEGIN{d=b-a; if(d<=0)d=0.001; printf "%.1f", s*8/1000000/d}')
  secs=$(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.1f", b-a}')
  echo "tunnel-checks: scp $((size / 1024 / 1024)) MiB in ${secs}s (${mbps} Mbps), sha256 $got"
  if [ "$got" != "$want" ]; then echo "tunnel-checks: HASH MISMATCH — wanted $want" >&2; exit 1; fi
  echo "tunnel-checks: hash verified end to end"
  ;;
ros2)
  # ROS 2 lives in a sidecar on this namespace, so the query runs there; this container has the
  # docker socket (docs/12) and the sidecar has the interface. What is being tested is whether DDS
  # discovery and user traffic cross the link, with the direct path removed by dds-isolate.sh.
  ros_env='set +u; source /opt/ros/jazzy/setup.bash; [ -s /tmp/fastdds-tunnel.xml ] && export FASTRTPS_DEFAULT_PROFILES_FILE=/tmp/fastdds-tunnel.xml;'
  out=$(docker compose exec -T operator-ros bash -lc "$ros_env timeout 25 ros2 topic list" 2>&1)
  echo "tunnel-checks: ros2 topic list ->" $(echo "$out" | tr '\n' ' ')
  case "$out" in *"/fjarr/robot_heartbeat"*) ;; *) echo "tunnel-checks: the robot's topic is not visible over the link" >&2; exit 1 ;; esac
  msg=$(docker compose exec -T operator-ros bash -lc "$ros_env timeout 25 ros2 topic echo --once /fjarr/robot_heartbeat" 2>&1)
  echo "tunnel-checks: ros2 topic echo ->" $(echo "$msg" | tr '\n' ' ')
  case "$msg" in *demo-robot-01*) ;; *) echo "tunnel-checks: discovery worked but no sample arrived" >&2; exit 1 ;; esac
  echo "tunnel-checks: ROS 2 discovery and data both crossed the link"
  ;;
*) echo "usage: tunnel-checks.sh ssh|scp|ros2" >&2; exit 2 ;;
esac
