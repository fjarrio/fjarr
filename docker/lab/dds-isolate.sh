#!/usr/bin/env bash
# Force DDS onto the tunnel by taking away the direct path, which is the only way a "ROS 2 works
# over the link" claim means anything: both ends share a docker bridge here, so stock discovery
# would find the peer over eth0 and the tunnel would be carrying nothing. The spike did the same
# with iptables (agent/spikes/ros2-tunnel/setup.sh).
#
#   dds-isolate.sh on    drop DDS between the two eth0 addresses, and DDS multicast on eth0
#   dds-isolate.sh off   remove those rules
#   dds-isolate.sh show  what is in place
#
# ALL direct traffic between the two addresses is blocked, not just DDS's own ports: blocking the
# 7400-7600 range and its multicast group looked like enough, and discovery kept working anyway —
# Fast DDS had found a way round it. Guessing a port range is how you write a test that proves
# nothing, so the rule is now "no direct path at all", which is what the spike did too.
#
# The tunnel session therefore has to reach the robot another way, and does: with
# `--ice-policy relay` its media goes through coturn, which is a third host and unaffected. That is
# the same mechanism the rate-control gate uses to choose a path on purpose (docs/25).
# Traffic over fjarr0 is untouched: these rules match the eth0 path only.
# spec: docs/27-network-tunnel.md#ros2
set -euo pipefail
cd "$(dirname "$0")/../.."

DDS_MCAST=${DDS_MCAST:-239.255.0.0/16}
CHAIN=FJDDS

ip_of() { docker compose exec -T "$1" hostname -i 2>/dev/null | tr -d '\r' | awk '{print $1}'; }

apply_in() { # apply_in <service> <peer-ip>
  local svc=$1 peer=$2
  docker compose exec -T -u root "$svc" sh -euc "
    iptables -N $CHAIN 2>/dev/null || iptables -F $CHAIN
    iptables -C OUTPUT -o eth0 -j $CHAIN 2>/dev/null || iptables -I OUTPUT -o eth0 -j $CHAIN
    iptables -C INPUT -i eth0 -j $CHAIN 2>/dev/null || iptables -I INPUT -i eth0 -j $CHAIN
    iptables -A $CHAIN -d $peer -j DROP
    iptables -A $CHAIN -s $peer -j DROP
    iptables -A $CHAIN -p udp -d $DDS_MCAST -j DROP
    iptables -A $CHAIN -j RETURN
  "
}

clear_in() {
  docker compose exec -T -u root "$1" sh -c "
    iptables -D OUTPUT -o eth0 -j $CHAIN 2>/dev/null
    iptables -D INPUT -i eth0 -j $CHAIN 2>/dev/null
    iptables -F $CHAIN 2>/dev/null; iptables -X $CHAIN 2>/dev/null; true
  " >/dev/null 2>&1 || true
}

ROBOT_IP=$(ip_of demo-robot); DEV_IP=$(ip_of dev)
case "${1:-}" in
on)
  apply_in demo-robot "$DEV_IP"
  apply_in dev "$ROBOT_IP"
  echo "dds-isolate: DDS blocked between robot $ROBOT_IP and operator $DEV_IP on eth0; the tunnel is the only path left"
  ;;
off)
  clear_in demo-robot; clear_in dev
  echo "dds-isolate: direct DDS path restored"
  ;;
show)
  for s in demo-robot dev; do echo "--- $s"; docker compose exec -T -u root "$s" iptables -L $CHAIN -n -v 2>&1 | head -8; done
  ;;
*) echo "usage: dds-isolate.sh on|off|show" >&2; exit 2 ;;
esac
