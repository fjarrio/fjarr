#!/usr/bin/env python3
"""cputable.py RUN_DIR...  (anywhere: reads blinkrun.py run directories with gst-* clients)

One row per gstcpu.py client of each run: mode, fps out of pipewiresrc and the encoder, CPU (% of
one core) of the pipeline process, gnome-shell, the test window, the whole machine (cores busy),
the GPU's mean busy %, plus the primary plane's state (direct scanout or composited) during that
client's measurement window and the decode check (h264check.py, check-LABEL.log) when there is one.
The primary plane is plane-3 on the spike machine; set PLANE=plane-N for another.

Example: python3 cputable.py runs/cpu-scanout runs/cpu-composited
"""
import glob
import json
import os
import re
import sys

print(f"{'run':28} {'client':6} {'mode':10} {'primary plane':22} {'src fps':>7} {'enc fps':>7} {'pipeline':>8} {'shell':>6} {'window':>6} "
      f"{'machine':>7} {'gpu':>5}  decode")
PLANE = os.environ.get("PLANE", "plane-3")
for run in sys.argv[1:]:
    name = os.path.basename(run.rstrip("/"))
    kms = []
    for ln in open(f"{run}/kms.log", errors="replace"):
        f = ln.split()
        if len(f) > 5 and f[1] == PLANE:
            kms.append((float(f[0]), f[5].split("=")[1], ln.split("alloc='")[1].split("'")[0]))
    for js in sorted(glob.glob(f"{run}/gst-*.json")):
        label = os.path.basename(js)[4:-5]
        res = json.load(open(js))
        log = open(f"{run}/gst-{label}.log").read()
        m = re.search(r"([\d.]+) window start", log)
        t0 = float(m.group(1)) if m else 0
        t1 = t0 + res["seconds"]
        before = [k for k in kms if k[0] <= t0]
        within = [k for k in kms if t0 < k[0] <= t1]
        states = {f"{k[1]}/{k[2]}" for k in (before[-1:] + within)}
        plane = "scanout (XR24 client)" if states == {"XR24/KMS thread"} else ("composited (XR30)" if states == {"XR30/gnome-shell"} else "+".join(sorted(states)))
        chk = ""
        cp = f"{run}/check-{label}.log"
        if os.path.exists(cp):
            m2 = re.search(r"decoded (\d+) frames \S+: (\{[^}]*\})", open(cp).read())
            if m2:
                chk = f"{m2.group(1)} frames {m2.group(2)}"
        print(f"{name:28} {label:6} {res['mode']:10} {plane:22} {res['src_fps']:7.2f} {res['enc_fps']:7.2f} {res.get('cpu_self_pct', 0):7.1f}% "
              f"{res.get('cpu_gs_pct', 0):5.1f}% {res.get('cpu_win_pct', 0):5.1f}% {res['cpu_machine_cores_busy']:7.2f} {res['gpu_busy_pct_mean']:4.1f}%  {chk}")
