#!/usr/bin/env python3
"""h264check.py FILE.h264 OUTLOG  (any user with access to the VA device)

Decodes an H.264 byte stream written by gstcpu.py (vah264dec, then
vapostproc to BGRx in system memory) and classifies every decoded frame by five sampled points
(the four quadrants' centres and the frame centre): all magenta, all green, mixed (a frame that
holds two blink states, i.e. torn or half-written), or other. Logs a summary line.

Its limit, found in the spike's review: with a window that changes colour every frame and mutter
cycling 8 buffers, a frame read too early from a reused buffer still shows a plausible colour, so
"0 mixed" at 30 Hz does not prove a sync scheme correct. Slow blinks (blinkrun.py, blinktable.py)
are the stronger test.

Example: python3 h264check.py runs/r1/gst-A.h264 runs/r1/check-A.log
"""
import sys
import time

import gi

gi.require_version("Gst", "1.0")
from gi.repository import Gst  # noqa: E402

path, outlog = sys.argv[1], sys.argv[2]
out = open(outlog, "a", buffering=1)


def log(line):
    out.write(f"{time.time():.3f} {line}\n")


Gst.init(None)
pipe = Gst.parse_launch(f"filesrc location={path} ! h264parse ! vah264dec ! vapostproc ! video/x-raw,format=BGRx ! "
                        "appsink name=sink sync=false emit-signals=false max-buffers=4")
sink = pipe.get_by_name("sink")
pipe.set_state(Gst.State.PLAYING)
stats = {"magenta": 0, "green": 0, "mixed": 0, "other": 0}
seq = []
n = 0
while True:
    sample = sink.emit("try-pull-sample", 5 * Gst.SECOND)
    if sample is None:
        break
    buf = sample.get_buffer()
    st = sample.get_caps().get_structure(0)
    w, h = st.get_value("width"), st.get_value("height")
    ok, info = buf.map(Gst.MapFlags.READ)
    if not ok:
        continue
    stride = info.size // h
    cls = []
    for fx, fy in ((0.25, 0.25), (0.75, 0.25), (0.5, 0.5), (0.25, 0.75), (0.75, 0.75)):
        p = int(fy * h) * stride + int(fx * w) * 4
        b, g, r = info.data[p], info.data[p + 1], info.data[p + 2]
        cls.append("m" if r > 200 and b > 200 and g < 60 else "g" if g > 200 and r < 60 and b < 60 else "o")
    buf.unmap(info)
    n += 1
    if all(c == "m" for c in cls):
        stats["magenta"] += 1
        seq.append("M")
    elif all(c == "g" for c in cls):
        stats["green"] += 1
        seq.append("G")
    elif "o" not in cls:
        stats["mixed"] += 1
        seq.append("X")
        log(f"frame {n} mixed {''.join(cls)}")
    else:
        stats["other"] += 1
        seq.append("o")
pipe.set_state(Gst.State.NULL)
changes = sum(1 for i in range(1, len(seq)) if seq[i] != seq[i - 1])
log(f"decoded {n} frames {w if n else 0}x{h if n else 0}: {stats}; colour changes between consecutive frames: {changes}; "
    f"first 60: {''.join(seq[:60])}")
