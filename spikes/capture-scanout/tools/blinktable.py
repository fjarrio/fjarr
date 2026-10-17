#!/usr/bin/env python3
"""blinktable.py RUN_DIR...  (anywhere: reads blinkrun.py run directories)

For each capture client of a blinkrun.py run: when it was started (relative to the previous blink),
when its stream started, when its first buffer arrived and with which colour, the next blink, and
then, for every later blink, whether a buffer with that blink's colour arrived within 150 ms.
Also: was the primary plane a client XR24 framebuffer (direct scanout) from before the start to
the end, and the sequence of CRC values (each distinct triple gets a letter, A, B, ...; a blinking
window should alternate two). The primary plane is plane-3 on the spike machine; set PLANE=plane-N
for another. The scanout test looks for XR24 client buffers (GTK4 on Mesa); a client that is
scanned out as AR24 needs SCANOUT_FORMAT=AR24.

Example: python3 blinktable.py runs/q1-r1 runs/q1-r2
"""
import os
import re
import sys


def lines(path):
    try:
        with open(path, errors="replace") as f:
            return [ln.rstrip("\n") for ln in f]
    except OSError:
        return []


def colour_of(line):
    m = re.search(r"centre=\(\d+,\d+,\d+\) (\w+)", line) or re.search(r"colour=(\w+)\(", line)
    if m:
        return m.group(1)
    return "?"


PLANE = os.environ.get("PLANE", "plane-3")
FMT = "format=" + os.environ.get("SCANOUT_FORMAT", "XR24")

for run in sys.argv[1:]:
    name = os.path.basename(run.rstrip("/"))
    win = lines(f"{run}/testwin.log")
    blinks = [(float(l.split()[0]), l.split()[2]) for l in win if " blink " in l]
    orch = lines(f"{run}/orch.log")
    kms = [l for l in lines(f"{run}/kms.log") if f" {PLANE} " in l]
    first_xr24 = next((float(l.split()[0]) for l in kms if FMT in l), None)
    after = [l for l in kms if first_xr24 and float(l.split()[0]) >= first_xr24]
    all_xr24 = all(FMT in l for l in after)
    print(f"== {name}: {len(blinks)} blinks; {PLANE} {FMT[7:]} (client fb, direct scanout) from {first_xr24 or 0:.3f}"
          f" to the end: {'yes' if first_xr24 and all_xr24 else 'NO'} ({len(after)} flips)")
    for l in orch:
        m = re.match(r"([\d.]+) client (\S+) kind=(\S+) (?:duration=\S+ )?starting", l)
        if not m:
            continue
        t0, label, kind = float(m.group(1)), m.group(2), m.group(3)
        prev = [b for b in blinks if b[0] <= t0]
        nxt = [b for b in blinks if b[0] > t0]
        pb = prev[-1] if prev else None
        log = lines(f"{run}/cap-{label}.log") or lines(f"{run}/probe-{label}.log")
        bufs = [l for l in log if " buf n=" in l]
        full = [l for l in bufs if "EMPTY" not in l and "size=0 " not in l]
        stream_t = None
        for l in log:
            if "pipeline PLAYING" in l or "(fixed)" in l or "no modifier (SHM)" in l:
                stream_t = float(l.split()[0])
                break
        scan = FMT[7:] if first_xr24 and first_xr24 < t0 else "not scanned out"
        head = f"  {label} {kind}: started {t0:.3f} = {t0 - pb[0]:.2f} s after blink {len(prev)} ({pb[1]})" if pb else f"  {label} {kind}: {t0:.3f}"
        print(head + f", {PLANE} {scan}; stream {('%.3f' % stream_t) if stream_t else '-'}")
        if not full:
            print(f"     no frame at all ({len(bufs)} buffers, {len(bufs) - len(full)} without pixels)")
            continue
        f0 = full[0]
        tf = float(f0.split()[0])
        nb = nxt[0] if nxt else None
        print(f"     first frame {tf:.3f}: +{1000 * (tf - t0):.0f} ms after the start, +{1000 * (tf - stream_t):.0f} ms after the stream"
              f" started, colour {colour_of(f0)} (screen: {pb[1] if pb else '?'}), "
              + (f"{1000 * (nb[0] - tf):.0f} ms before blink {len(prev) + 1}" if nb and tf < nb[0] else
                 ("AFTER the next blink" if nb else "no blink after it: the screen stayed still")))
        ends = [float(l.split()[0]) for l in log if re.match(r"[\d.]+ end ", l)]
        t_end = ends[0] if ends else 1e12
        later = [b for b in blinks if tf < b[0] < t_end - 0.2]  # blinks while the client ran
        hits = 0
        detail = []
        for i, (bt, bc) in enumerate(later):
            got = [l for l in full if bt - 0.005 <= float(l.split()[0]) <= bt + 0.150]
            ok = any(colour_of(l) == bc for l in got)
            unknown = got and all(colour_of(l) == "?" for l in got)
            hits += ok
            mark = "ok" if ok else ("?" if unknown else ("WRONG" if got else "MISSING"))
            detail.append(f"{bc[0]}:{mark}{('@%d' % int(1000 * (float(got[0].split()[0]) - bt))) if got else ''}")
        extra = [l for l in full if not any(bt - 0.005 <= float(l.split()[0]) <= bt + 0.150 for bt, _ in later)][1:]
        print(f"     later blinks recorded with the right colour: {hits}/{len(later)} [{' '.join(detail)}]"
              " (colour:ok|?=frame, colour not readable|WRONG|MISSING @ ms after the window logged the blink)")
        print(f"     other frames (not at a blink): {len(extra)} at " + ", ".join(f"{float(l.split()[0]):.3f}" for l in extra))
    crc = [l.split("crc=", 1)[1].strip() for l in lines(f"{run}/crc.log") if "crc=" in l]
    names = {}
    for c in crc:
        names.setdefault(c, chr(ord("A") + len(names)) if len(names) < 26 else "?")
    print(f"  CRC changes: {len(crc)}; last 12 values: " + " ".join(names[c] for c in crc[-12:])
          + "  (" + ", ".join(f"{v}={k}" for k, v in list(names.items())[:6]) + ")")
