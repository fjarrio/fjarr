#!/usr/bin/env python3
"""kmsmon.py INTERVAL_MS SECONDS OUTLOG  (root)

Which framebuffer each active KMS plane scans out. Samples the DRM debugfs state file
(DRI_STATE, default /sys/kernel/debug/dri/0/state) every INTERVAL_MS and logs a line whenever an
active plane's fb changes: its id, size, format, modifier and who allocated it. On the spike
machine (amdgpu) gnome-shell's own onscreen buffer shows as format=XR30, alloc='gnome-shell', and a
client buffer scanned out directly as format=XR24 (or AR24), alloc='KMS thread'. "plane-N off"
means the plane was switched off (for the cursor plane: a software cursor, or none). Read-only.
Ends after SECONDS, or when a file named "stop" appears next to OUTLOG.

Example: sudo python3 kmsmon.py 20 120 runs/r1/kms.log
"""
import os
import re
import sys
import time

interval = int(sys.argv[1]) / 1000.0
seconds = float(sys.argv[2])
out = open(sys.argv[3], "a", buffering=1)
STATE = os.environ.get("DRI_STATE", "/sys/kernel/debug/dri/0/state")


def log(line):
    out.write(f"{time.time():.3f} {line}\n")


def parse(text):
    planes = {}
    for block in re.split(r"\n(?=plane\[)", text):
        m = re.match(r"plane\[(\d+)\]: (\S+)", block)
        if not m:
            continue
        crtc = re.search(r"\n\tcrtc=(\S+)", block)
        if not crtc or crtc.group(1) == "(null)":
            continue
        fb = re.search(r"\n\tfb=(\d+)", block)
        fmt = re.search(r"format=(\S+)", block)
        mod = re.search(r"modifier=(\S+)", block)
        size = re.search(r"\n\t\tsize=(\S+)", block)
        alloc = re.search(r"allocated by = (.*)", block)
        imp = re.search(r"imported=(\S+)", block)
        planes[m.group(2)] = (crtc.group(1), fb.group(1) if fb else "?", size.group(1) if size else "?", fmt.group(1) if fmt else "?",
                              mod.group(1) if mod else "?", alloc.group(1).strip() if alloc else "?", imp.group(1) if imp else "?")
    return planes


last = {}
end = time.time() + seconds
n = 0
log(f"start interval={int(interval * 1000)}ms")
STOP = os.path.join(os.path.dirname(os.path.abspath(sys.argv[3])), "stop")  # the run's stop file ends it early
while time.time() < end and not os.path.exists(STOP):
    with open(STATE) as f:
        planes = parse(f.read())
    n += 1
    for name, v in planes.items():
        if last.get(name) != v:
            crtc, fb, size, fmt, mod, alloc, imp = v
            log(f"{name} crtc={crtc} fb={fb} size={size} format={fmt} modifier={mod} alloc='{alloc}' imported={imp}")
    for name in set(last) - set(planes):
        log(f"{name} off")
    last = planes
    time.sleep(interval)
log(f"end samples={n}")
