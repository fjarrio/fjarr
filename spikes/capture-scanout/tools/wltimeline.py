#!/usr/bin/env python3
"""wltimeline.py RUN_DIR [FROM_EPOCH TO_EPOCH]  (anywhere: reads logs only)

Merges one run directory's logs into one timeline in epoch seconds: the window's WAYLAND_DEBUG
output (testwin-app.log; libwayland prints CLOCK_REALTIME microseconds truncated to 32 bits, as
milliseconds), the window's own log (testwin.log), the KMS plane log (kms.log), the CRC log
(crc.log), the independent capture (cap.log) and, when present, gnome-shell's MUTTER_DEBUG lines
(mutter.log, from mutterlog.sh). Prints only the interesting Wayland requests and events, one line
each: "<epoch> <TAG> <text>" with TAG WIN, WL, KMS, CRC, CAP or MUT.

Example: python3 wltimeline.py runs/h1-blink > runs/h1-blink/timeline.txt
"""
import os
import re
import sys

run = sys.argv[1]
lo = float(sys.argv[2]) if len(sys.argv) > 2 else 0
hi = float(sys.argv[3]) if len(sys.argv) > 3 else 1e12

events = []
t0 = None
with open(os.path.join(run, "testwin.log")) as f:
    for line in f:
        t, rest = line.split(" ", 1)
        t = float(t)
        if t0 is None:
            t0 = t
        events.append((t, "WIN", rest.strip()))

WRAP = 2 ** 32
INTERESTING = re.compile(
    r"wl_surface#\d+\.(attach|commit|frame|damage_buffer|set_buffer_scale)|wl_callback#\d+\.done|wl_buffer#\d+\.release|"
    r"zwp_linux_dmabuf_feedback_v1#\d+\.(tranche_flags|tranche_target_device|done|main_device|format_table)|"
    r"wp_presentation_feedback#\d+\.(presented|discarded)|wp_presentation#\d+\.feedback|wl_keyboard#\d+\.(key|enter|leave)|"
    r"wp_linux_drm_syncobj_surface_v1#\d+\.|wp_linux_drm_syncobj_timeline_v1|zwp_linux_buffer_params_v1#\d+\.(create_immed|created|add)|"
    r"zwp_linux_dmabuf_v1#\d+\.get_surface_feedback|xdg_toplevel#\d+\.configure|xdg_surface#\d+\.(ack_configure|configure)|"
    r"wl_display#\d+\.error|wp_fifo|wp_commit_timing|destroy\(\)"
)
pat = re.compile(r"^\[\s*(\d+)\.(\d+)\]\s+(\{[^}]*\})?\s*(.*)$")
app = os.path.join(run, "testwin-app.log")
if os.path.exists(app):
    with open(app, errors="replace") as f:
        for line in f:
            m = pat.match(line)
            if not m:
                if "Gdk-" in line or "WARNING" in line or "CRITICAL" in line:
                    events.append((None, "APP", line.strip()))
                continue
            us = int(m.group(1)) * 1000 + int(m.group(2))
            base = int(t0 * 1e6)
            k = round((base - us) / WRAP)
            t = (k * WRAP + us) / 1e6
            msg = m.group(4)
            if INTERESTING.search(msg):
                events.append((t, "WL", (m.group(3) or "") + " " + msg))

for name, tag in (("kms.log", "KMS"), ("crc.log", "CRC"), ("cap.log", "CAP"), ("mutter.log", "MUT")):
    p = os.path.join(run, name)
    if os.path.exists(p):
        with open(p, errors="replace") as f:
            for line in f:
                parts = line.split(" ", 1)
                try:
                    events.append((float(parts[0]), tag, parts[1].strip()))
                except (ValueError, IndexError):
                    pass

# Gdk warnings carry only a wall-clock time of day: keep them in file order next to nothing.
timed = sorted((e for e in events if e[0] is not None), key=lambda e: e[0])
for t, tag, msg in timed:
    if lo <= t <= hi:
        print(f"{t:.3f} {tag:4} {msg[:230]}")
for e in events:
    if e[0] is None:
        print("     APP ", e[2][:230])
