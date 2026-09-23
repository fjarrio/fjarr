#!/usr/bin/env bash
# THROWAWAY SPIKE FIXTURE: bring one side of the point-to-point link up inside a container.
#   setup.sh <self-tun-ip> <peer-tun-ip> <peer-eth-ip> <domain>
# Everything between the two containers is blocked except the pump and relay ports, so DDS can
# only reach the peer through the tunnel — without that the containers share a docker network
# and the test would prove nothing.
set -euo pipefail
SELF_TUN=$1; PEER_TUN=$2; PEER_ETH=$3; DOMAIN=${4:-0}
PUMP_PORT=7777; RELAY_PORT=7778
ETH_IP=$(ip -4 -o addr show eth0 | awk '{print $4}' | cut -d/ -f1)

iptables -F || true
iptables -A INPUT -p udp --dport "$PUMP_PORT" -j ACCEPT
iptables -A INPUT -p udp --dport "$RELAY_PORT" -j ACCEPT
iptables -A INPUT -s "$PEER_ETH" -j DROP          # no direct path: the tunnel or nothing
iptables -A OUTPUT -d "$PEER_ETH" -p udp --dport "$PUMP_PORT" -j ACCEPT
iptables -A OUTPUT -d "$PEER_ETH" -p udp --dport "$RELAY_PORT" -j ACCEPT
iptables -A OUTPUT -d "$PEER_ETH" -j DROP

python3 /spike/pump.py tun0 "$PEER_ETH" "$PUMP_PORT" > /tmp/pump.log 2>&1 &
sleep 1
ip addr add "$SELF_TUN/24" dev tun0
ip link set tun0 mtu 1400 up
ip route add 239.255.0.0/16 dev eth0 2>/dev/null || true   # multicast stays local; the relay carries it
python3 /spike/relay.py "$DOMAIN" "$PEER_ETH" "$RELAY_PORT" "$ETH_IP" > /tmp/relay.log 2>&1 &
sleep 1
echo "link up: $SELF_TUN <-> $PEER_TUN (eth $ETH_IP, peer eth $PEER_ETH, domain $DOMAIN)"
ip -4 -o addr show tun0
