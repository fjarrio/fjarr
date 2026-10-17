#!/usr/bin/env python3
"""Ask for a boot server the way UEFI does, and print every reply on one line.

`discover` broadcasts a DHCPDISCOVER with the vendor class and client architecture of UEFI PXE
(PXEClient, arch 11 on arm64) or UEFI HTTP boot (HTTPClient, arch 19); `request4011` sends the
PXE boot-server request a UEFI client sends to a proxy's port 4011. Test helper for bench.sh.

spec: docs/32-fleet-ota.md#jetson-test-image ("Three rescue layers", item 2)
"""

import argparse
import random
import socket
import struct
import time

TYPES = {1: "DISCOVER", 2: "OFFER", 3: "REQUEST", 5: "ACK", 6: "NAK"}


def packet(msg_type, mac, arch, vendor, ciaddr, xid):
    chaddr = bytes.fromhex(mac.replace(":", "")).ljust(16, b"\0")
    head = struct.pack("!BBBBIHH4s4s4s4s16s64s128s", 1, 1, 6, 0, xid, 0, 0x8000,
                       socket.inet_aton(ciaddr), bytes(4), bytes(4), bytes(4), chaddr, b"", b"")
    vendor = f"{vendor}:Arch:{arch:05d}:UNDI:003016".encode()
    opts = bytes([53, 1, msg_type, 93, 2]) + struct.pack("!H", arch)
    opts += bytes([94, 3, 1, 3, 16, 97, 17, 0]) + bytes(16)
    opts += bytes([60, len(vendor)]) + vendor + bytes([55, 6, 1, 3, 43, 60, 66, 67, 255])
    return head + b"\x63\x82\x53\x63" + opts


def parse(data):
    xid, = struct.unpack_from("!I", data, 4)
    yiaddr, siaddr = socket.inet_ntoa(data[16:20]), socket.inet_ntoa(data[20:24])
    file = data[108:236].split(b"\0")[0].decode(errors="replace")
    opts, i = {}, 240
    while i < len(data) and data[i] != 255:
        if data[i] == 0:
            i += 1
            continue
        opts[data[i]] = data[i + 2:i + 2 + data[i + 1]]
        i += 2 + data[i + 1]
    return xid, {
        "type": TYPES.get(opts.get(53, b"\0")[0], "?"),
        "server": socket.inet_ntoa(opts[54]) if 54 in opts else "-",
        "yiaddr": yiaddr,
        "siaddr": siaddr,
        "vendor": opts.get(60, b"").decode(errors="replace") or "-",
        "file": file or opts.get(67, b"").decode(errors="replace") or "-",
        "opt43": "yes" if 43 in opts else "no",
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("mode", choices=["discover", "request4011"])
    ap.add_argument("--mac", required=True)
    ap.add_argument("--client", choices=["pxe", "http"], required=True)
    ap.add_argument("--iface", default="eth0")
    ap.add_argument("--server", help="the proxy, for request4011")
    ap.add_argument("--ciaddr", default="0.0.0.0", help="the client's address, for request4011")
    ap.add_argument("--wait", type=float, default=3)
    args = ap.parse_args()
    vendor, arch = ("PXEClient", 11) if args.client == "pxe" else ("HTTPClient", 19)
    xid = random.getrandbits(32)

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BINDTODEVICE, args.iface.encode())
    s.bind(("0.0.0.0", 68))
    if args.mode == "discover":
        s.sendto(packet(1, args.mac, arch, vendor, "0.0.0.0", xid), ("255.255.255.255", 67))
    else:
        s.sendto(packet(3, args.mac, arch, vendor, args.ciaddr, xid), (args.server, 4011))
    end = time.monotonic() + args.wait
    while (left := end - time.monotonic()) > 0:
        s.settimeout(left)
        try:
            data, (src, _) = s.recvfrom(4096)
        except TimeoutError:
            break
        if len(data) < 240 or data[0] != 2:
            continue
        rxid, r = parse(data)
        if rxid == xid:
            print(f"from={src} " + " ".join(f"{k}={v}" for k, v in r.items()), flush=True)


if __name__ == "__main__":
    main()
