#!/usr/bin/env python3
"""crcmon.py SECONDS OUTLOG  (root)

What a connector actually displays, frame by frame. Reads the CRTC CRC from DRM debugfs (CRC_DATA,
default /sys/kernel/debug/dri/0/crtc-0/crc/data; the source is left at its default "auto"): while
the file is open the kernel computes a CRC of every frame the CRTC sends, and it stops when the
file is closed. Logs each change of the CRC triple with the frame number and the time it was read.
A screen that changes colour changes the CRC; a frozen screen keeps it.

Two caveats from the spike: the CRC shows content changes, not the path (in one run a flat colour
gave the same triple composited from XR30 and scanned out from XR24; use kmsmon.py for the path),
and the values depend on the driver and the output format. Tested on amdgpu only. Read-only.
Ends after SECONDS, or when a file named "stop" appears next to OUTLOG.

Example: sudo python3 crcmon.py 120 runs/r1/crc.log
"""
import os
import select
import sys
import time

seconds = float(sys.argv[1])
out = open(sys.argv[2], "a", buffering=1)
STOP = os.path.join(os.path.dirname(os.path.abspath(sys.argv[2])), "stop")
DATA = os.environ.get("CRC_DATA", "/sys/kernel/debug/dri/0/crtc-0/crc/data")


def log(line):
    out.write(f"{time.time():.3f} {line}\n")


fd = os.open(DATA, os.O_RDONLY)
log("start")
end = time.time() + seconds
buf = b""
last = None
frames = 0
while time.time() < end and not os.path.exists(STOP):
    r, _, _ = select.select([fd], [], [], 0.2)
    if not r:
        continue
    chunk = os.read(fd, 4096)
    if not chunk:
        continue
    buf += chunk
    while b"\n" in buf:
        line, buf = buf.split(b"\n", 1)
        parts = line.decode().split()
        if len(parts) < 2:
            continue
        frames += 1
        crc = " ".join(parts[1:])
        if crc != last:
            log(f"frame={parts[0]} crc={crc}")
            last = crc
os.close(fd)
log(f"end frames={frames}")
