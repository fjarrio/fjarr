#!/usr/bin/env bash
# docker/lab/tundev.sh — create the tunnel interface in a compose service, playing the part the
# installer plays on a real robot (docs/26, docs/27#lifecycle): a persistent TUN device that
# exists, is addressed and is owned by the agent's user BEFORE the agent starts. The agent then
# attaches with no CAP_NET_ADMIN of its own — which is the property this script exists to keep
# honest, so it never runs as part of the agent.
#
#   tundev.sh up <service> <address> <peer> [interface] [mtu]
#   tundev.sh down <service> [interface]
#   tundev.sh show <service> [interface]
#
# `up` is idempotent: an existing device is re-addressed rather than recreated, because taking
# the device down is exactly what breaks participants already bound to it.
#
# spec: docs/27-network-tunnel.md#lifecycle
set -euo pipefail

cd "$(dirname "$0")/../.."

cmd=${1:?usage: tundev.sh up|down|show <service> …}
service=${2:?service}
in_service() { docker compose exec -T -u root "$service" "$@"; }

case "$cmd" in
up)
  address=${3:?address}
  peer=${4:?peer}
  dev=${5:-fjarr0}
  mtu=${6:-1280}
  # The account the service runs as: the device must be owned by it, since the agent is not root.
  owner=$(docker compose exec -T "$service" id -un | tr -d '\r\n')
  in_service sh -euc "
    test -e /dev/net/tun || { echo 'tundev: /dev/net/tun is not in the container — add it to the service devices'; exit 1; }
    ip link show $dev >/dev/null 2>&1 || ip tuntap add $dev mode tun user $owner
    ip addr flush dev $dev
    ip addr add $address peer $peer dev $dev
    ip link set $dev mtu $mtu up
  "
  echo "tundev: $service $dev $address peer $peer mtu $mtu (owner $owner)"
  ;;
down)
  dev=${3:-fjarr0}
  in_service sh -c "ip link show $dev >/dev/null 2>&1 && ip tuntap del $dev mode tun || true"
  ;;
show)
  dev=${3:-fjarr0}
  in_service sh -c "ip addr show $dev || true"
  ;;
*) echo "tundev: unknown command '$cmd'" >&2; exit 2 ;;
esac
