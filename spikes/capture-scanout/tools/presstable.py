#!/usr/bin/env python3
"""presstable.py TIMELINE.txt  (anywhere: reads a wltimeline.py output)

For every F9 press (state 1) and release (state 0) in a merged timeline, what followed within
150 ms: a window commit (attach), mutter's scanout assignment, a 'Recording full frame', a KMS flip
of the primary plane (and its format), a CRC change, a buffer in the own capture, and the
presentation feedback flags (15 = ZERO_COPY, i.e. directly scanned out; 7 = composited).
The primary plane is plane-3 on the spike machine; set PLANE=plane-N for another.
Needs the window under WAYLAND_DEBUG=1 (for the key events) and MUTTER_DEBUG=render,screen-cast.
"""
import os
import re, sys
rows = []
for line in open(sys.argv[1], errors="replace"):
    p = line.split(None, 2)
    if len(p) < 3:
        continue
    try:
        rows.append((float(p[0]), p[1], p[2].rstrip("\n")))
    except ValueError:
        pass
events = [(t, m) for t, tag, m in rows if tag == "WL" and re.search(r"wl_keyboard#\d+\.key\(\d+, \d+, 67, [01]\)", m)]
print("time            ev   attach scanout-assign full-frame-rec kms-flip(format)     crc-change  cap-buf      presented-flags")
for t, m in events:
    st = "PRESS" if m.rstrip().endswith("1)") else "REL  "
    win = [(tt, tag, mm) for tt, tag, mm in rows if t <= tt <= t + 0.150]
    attach = any(tag == "WL" and ".attach(" in mm for _, tag, mm in win)
    assign = sum(1 for _, tag, mm in win if tag == "MUT" and "Assigning scanout" in mm)
    full = sum(1 for _, tag, mm in win if tag == "MUT" and "Recording full frame" in mm)
    kms = [re.search(r"format=(\S+)", mm).group(1) for _, tag, mm in win if tag == "KMS" and os.environ.get("PLANE", "plane-3") + " " in mm]
    crc = [re.search(r"crc=(\S+ \S+ \S+)", mm).group(1) for _, tag, mm in win if tag == "CRC"]
    cap = [mm.split()[-1] for _, tag, mm in win if tag == "CAP" and "buf n=" in mm]
    pres = [re.search(r", (\d+)\)$", mm).group(1) for _, tag, mm in win if tag == "WL" and "presented(" in mm]
    print(f"{t:.3f} {st} {str(attach):6} {assign:14} {full:14} {','.join(kms) or '-':20} {','.join(c.split()[0] for c in crc) or '-':11} {','.join(cap) or '-':12} {','.join(pres) or '-'}")
