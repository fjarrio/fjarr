#!/usr/bin/env python3
"""THROWAWAY SPIKE FIXTURE (agent/spikes/ros2-tunnel): a userspace point-to-point link.

Reads IP packets from a TUN device and sends each as one UDP datagram to the peer, and the
reverse. This stands in for a Fjarr data channel while the DDS question is isolated; stage B
replaces the UDP hop with a real channel. Not production code, not reviewed, not shipped.
"""
import fcntl
import os
import select
import socket
import struct
import sys

TUNSETIFF = 0x400454CA
IFF_TUN = 0x0001
IFF_NO_PI = 0x1000


def open_tun(name: str) -> int:
    fd = os.open("/dev/net/tun", os.O_RDWR)
    fcntl.ioctl(fd, TUNSETIFF, struct.pack("16sH", name.encode(), IFF_TUN | IFF_NO_PI))
    return fd


def main() -> None:
    tun_name, peer_ip, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
    tun = open_tun(tun_name)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("0.0.0.0", port))
    print(f"pump: {tun_name} <-> udp://{peer_ip}:{port}", flush=True)
    sent = received = 0
    while True:
        ready, _, _ = select.select([tun, sock], [], [], 5.0)
        if not ready:
            print(f"pump: {sent} out, {received} in", flush=True)
            continue
        for fd in ready:
            if fd is tun:
                packet = os.read(tun, 65535)
                sock.sendto(packet, (peer_ip, port))
                sent += 1
            else:
                packet, _ = sock.recvfrom(65535)
                os.write(tun, packet)
                received += 1


if __name__ == "__main__":
    main()
