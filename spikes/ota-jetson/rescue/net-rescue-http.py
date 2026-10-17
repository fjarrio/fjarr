#!/usr/bin/env python3
"""Serve the network rescue to one machine's UEFI HTTP boot.

One file, by one name, to one MAC address: a request from any other machine on the LAN gets 403
(the MAC comes from the kernel's neighbour table, which holds the client's address once its TCP
connection is up). Loopback is allowed, so net-rescue.sh --status can check the server. HEAD and
GET only, with Content-Type application/efi, which UEFI's HTTP boot reads to tell an EFI program
from a disk image. Installed and started by net-rescue.sh on the lab mini-PC.

spec: docs/32-fleet-ota.md#jetson-test-image ("Three rescue layers", item 2)
"""

import argparse
import http.server
import ipaddress
import os
import shutil


def neighbour_mac(ip):
    """The MAC the kernel has for an IPv4 neighbour, or None."""
    try:
        with open("/proc/net/arp") as f:
            next(f)
            for line in f:
                fields = line.split()
                # IP address, HW type, Flags, HW address, Mask, Device; flag 0x2 = complete
                if len(fields) >= 4 and fields[0] == ip and int(fields[2], 16) & 0x2:
                    return fields[3].lower()
    except OSError:
        pass
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--file", required=True, help="the EFI file to serve")
    ap.add_argument("--mac", required=True, help="the only client MAC served")
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8471)
    args = ap.parse_args()
    path = os.path.abspath(args.file)
    url_path = "/" + os.path.basename(path)
    allowed = args.mac.lower()

    class Handler(http.server.BaseHTTPRequestHandler):
        server_version = "fjarr-net-rescue"
        sys_version = ""

        def client_allowed(self):
            ip = self.client_address[0].removeprefix("::ffff:")
            if ipaddress.ip_address(ip).is_loopback:
                return True, "loopback"
            mac = neighbour_mac(ip)
            return mac == allowed, mac or "no neighbour entry"

        def serve(self, body):
            ok, who = self.client_allowed()
            if not ok:
                self.log_message("refused %s (%s)", self.client_address[0], who)
                return self.send_error(403)
            if self.path.split("?", 1)[0] != url_path:
                return self.send_error(404, explain=f"only {url_path} is served")
            with open(path, "rb") as f:
                size = os.fstat(f.fileno()).st_size
                self.send_response(200)
                self.send_header("Content-Type", "application/efi")
                self.send_header("Content-Length", str(size))
                self.end_headers()
                if body:
                    shutil.copyfileobj(f, self.wfile, 1 << 20)
            self.log_message("served %s to %s (%s), %d bytes", url_path, self.client_address[0], who,
                             size if body else 0)

        def do_GET(self):
            self.serve(True)

        def do_HEAD(self):
            self.serve(False)

    if not os.path.isfile(path):
        raise SystemExit(f"net-rescue-http: no file {path}")
    server = http.server.ThreadingHTTPServer((args.bind, args.port), Handler)
    server.daemon_threads = True
    print(f"net-rescue-http: {url_path} on {args.bind}:{args.port} for {allowed}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
