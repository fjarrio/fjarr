#!/usr/bin/env python3
"""THROWAWAY SPIKE FIXTURE: a DDS discovery multicast relay.

A point-to-point link carries no multicast, so discovery announcements never cross it. This
joins the RTPS discovery group on the local side, forwards each datagram to the peer over the
tunnel, and re-emits what the peer sends into the local group. Packets it re-emitted itself are
recognised by payload and not forwarded back, or the two relays would loop.

RTPS port arithmetic (the reason the domain id matters): the discovery group's port is
7400 + 250 * domain_id. A relay that assumed domain 0 would silently carry nothing.
"""
import hashlib
import select
import socket
import struct
import sys
import time

GROUP = "239.255.0.1"


def discovery_port(domain: int) -> int:
    return 7400 + 250 * domain  # PB + DG * domainId + d0


def main() -> None:
    domain, peer_ip, relay_port, local_ip = int(sys.argv[1]), sys.argv[2], int(sys.argv[3]), sys.argv[4]
    port = discovery_port(domain)

    rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rx.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    rx.bind(("", port))
    rx.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, struct.pack("4s4s", socket.inet_aton(GROUP), socket.inet_aton(local_ip)))

    tunnel = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    tunnel.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    tunnel.bind(("0.0.0.0", relay_port))

    tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    tx.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(local_ip))
    tx.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 1)
    tx.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 1)  # local participants must hear it

    print(f"relay: domain {domain} group {GROUP}:{port} <-> {peer_ip}:{relay_port} (local {local_ip})", flush=True)
    echoes: dict[str, float] = {}
    out = back = 0
    while True:
        ready, _, _ = select.select([rx, tunnel], [], [], 5.0)
        now = time.time()
        for key, at in list(echoes.items()):
            if now - at > 5:
                del echoes[key]
        if not ready:
            print(f"relay: {out} forwarded, {back} injected", flush=True)
            continue
        for fd in ready:
            if fd is rx:
                data, _ = rx.recvfrom(65535)
                key = hashlib.sha1(data).hexdigest()
                if key in echoes:  # our own re-emission coming back: never forward it
                    continue
                tunnel.sendto(data, (peer_ip, relay_port))
                out += 1
            else:
                data, _ = tunnel.recvfrom(65535)
                echoes[hashlib.sha1(data).hexdigest()] = now
                tx.sendto(data, (GROUP, port))
                back += 1


if __name__ == "__main__":
    main()
