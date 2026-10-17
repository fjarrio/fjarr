#!/usr/bin/env python3
"""gstcpu.py MODE NODE WARMUP_S WINDOW_S OUTPREFIX  (desktop user, in the session: see as-desktop.sh)

Runs one capture-to-H.264 pipeline on mutter node NODE and
measures what it costs. Logs the caps negotiated on every pad (like gst-launch -v) and the memory
of the first buffer on each src pad, counts buffers out of pipewiresrc and out of the encoder,
writes the H.264 to OUTPREFIX.h264, and over WINDOW_S seconds after WARMUP_S reads CPU time of this
process, of gnome-shell (GS_PID), of the user's pipewire (PW_PID) and of the whole machine
(/proc/stat), and samples the GPU's busy percentage (GPU_BUSY, default
/sys/class/drm/card0/device/gpu_busy_percent, amdgpu) every 100 ms. MODE none runs no pipeline
(baseline: only the measurement). The result is one JSON line in OUTPREFIX.log and OUTPREFIX.json;
cputable.py tabulates several. NODE comes from castsession.py (OUTLOG.node). The encoder settings
are the agent's (cbr, no B-frames, target-usage 6, main profile), at 6000 kbit/s.

Environment: GS_PID, PW_PID, WIN_PID (gnome-shell, pipewire, the test window; 0 = not measured),
GSTCPU_DEBUG (a GST_DEBUG spec, written to OUTPREFIX.gstdebug, for negotiation runs only).

MODE:
  dmabuf-va         pipewiresrc DMABuf (DMA_DRM) ! vapostproc ! VAMemory NV12 ! vah264enc       (zero-copy)
  shm-va            pipewiresrc always-copy SHM BGRx ! vapostproc (upload) ! vah264enc
  shm-agent         pipewiresrc always-copy SHM BGRx ! videoconvert I420 (CPU) ! vapostproc ! vah264enc
                    (a stand-in for the agent's chain today: reader copy, videoconvert to I420,
                    vapostproc, vah264enc; not the agent's own tee/videorate graph)
  dmabuf-va-xr24    dmabuf-va with drm-format forced to XR24:0x0200000000000901
  dmabuf-gldl-va    pipewiresrc DMABuf ! GL convert to NV12 ! gldownload (system memory) ! vapostproc ! vah264enc
  dmabuf-gldl-bgrx  pipewiresrc DMABuf ! GL download BGRx ! videoconvert I420 ! vapostproc ! vah264enc
  dmabuf-gl         pipewiresrc DMABuf ! glupload ! glcolorconvert NV12 ! gldownload DMABuf ! vapostproc ! vah264enc
                    (failed to negotiate on the spike machine: "No supported formats found")
  none              no pipeline

Ends after WARMUP_S + WINDOW_S, or when a file named "stop" appears next to OUTPREFIX.
Example: GS_PID=$(pgrep -u desktop -x gnome-shell) ./as-desktop.sh python3 gstcpu.py dmabuf-va 57 5 20 runs/r1/gst-A
"""
import json
import os
import sys
import time

import gi

gi.require_version("Gst", "1.0")
from gi.repository import GLib, Gst  # noqa: E402

mode, node, warmup, window, prefix = sys.argv[1], sys.argv[2], float(sys.argv[3]), float(sys.argv[4]), sys.argv[5]
out = open(prefix + ".log", "a", buffering=1)
STOP = os.path.join(os.path.dirname(os.path.abspath(prefix)), "stop")
CLK = os.sysconf("SC_CLK_TCK")
GPU = os.environ.get("GPU_BUSY", "/sys/class/drm/card0/device/gpu_busy_percent")


def log(line):
    out.write(f"{time.time():.3f} {line}\n")


ENC = ("vah264enc name=enc rate-control=cbr bitrate=6000 key-int-max=60 b-frames=0 target-usage=6 ! video/x-h264,profile=main ! "
       f"h264parse config-interval=-1 ! video/x-h264,stream-format=byte-stream,alignment=au ! filesink name=sink location={prefix}.h264")
PIPES = {
    "dmabuf-va": f"pipewiresrc name=src path={node} ! video/x-raw(memory:DMABuf) ! vapostproc name=pp ! "
                 f"video/x-raw(memory:VAMemory),format=NV12 ! {ENC}",
    "dmabuf-va-xr24": f"pipewiresrc name=src path={node} ! video/x-raw(memory:DMABuf),format=DMA_DRM,drm-format=XR24:0x0200000000000901 ! "
                      f"vapostproc name=pp ! video/x-raw(memory:VAMemory),format=NV12 ! {ENC}",
    "dmabuf-gldl-va": f"pipewiresrc name=src path={node} ! video/x-raw(memory:DMABuf) ! glupload ! glcolorconvert ! "
                      f"video/x-raw(memory:GLMemory),format=NV12 ! gldownload ! video/x-raw,format=NV12 ! vapostproc name=pp ! "
                      f"video/x-raw(memory:VAMemory),format=NV12 ! {ENC}",
    "dmabuf-gldl-bgrx": f"pipewiresrc name=src path={node} ! video/x-raw(memory:DMABuf) ! glupload ! glcolorconvert ! "
                        f"video/x-raw(memory:GLMemory),format=BGRx ! gldownload ! video/x-raw,format=BGRx ! videoconvert name=vc ! "
                        f"video/x-raw,format=I420 ! vapostproc name=pp ! video/x-raw(memory:VAMemory),format=NV12 ! {ENC}",
    "shm-va": f"pipewiresrc name=src path={node} always-copy=true ! video/x-raw,format=BGRx ! vapostproc name=pp ! "
              f"video/x-raw(memory:VAMemory),format=NV12 ! {ENC}",
    "shm-agent": f"pipewiresrc name=src path={node} always-copy=true ! video/x-raw,format=BGRx ! videoconvert name=vc ! "
                 f"video/x-raw,format=I420 ! vapostproc name=pp ! video/x-raw(memory:VAMemory),format=NV12 ! {ENC}",
    "dmabuf-gl": f"pipewiresrc name=src path={node} ! video/x-raw(memory:DMABuf) ! glupload ! glcolorconvert ! "
                 f"video/x-raw(memory:GLMemory),format=NV12 ! gldownload ! video/x-raw(memory:DMABuf) ! vapostproc name=pp ! "
                 f"video/x-raw(memory:VAMemory),format=NV12 ! {ENC}",
}


def proc_cpu(pid):
    try:
        with open(f"/proc/{pid}/stat") as f:
            fields = f.read().rsplit(")", 1)[1].split()
        return (int(fields[11]) + int(fields[12])) / CLK  # utime + stime
    except (OSError, ValueError, IndexError):
        return None


def sys_cpu():
    with open("/proc/stat") as f:
        v = [int(x) for x in f.readline().split()[1:]]
    idle = v[3] + v[4]
    return sum(v) / CLK, idle / CLK


def snapshot():
    return {"t": time.time(), "self": proc_cpu("self"), "gs": proc_cpu(os.environ.get("GS_PID", "0")),
            "pw": proc_cpu(os.environ.get("PW_PID", "0")), "win": proc_cpu(os.environ.get("WIN_PID", "0")), "sys": sys_cpu()}


if os.environ.get("GSTCPU_DEBUG"):  # e.g. "va*:5,pipewiresrc:5": a debug log of its own, for negotiation runs only
    os.environ["GST_DEBUG"] = os.environ["GSTCPU_DEBUG"]
    os.environ["GST_DEBUG_FILE"] = prefix + ".gstdebug"
    os.environ["GST_DEBUG_NO_COLOR"] = "1"
Gst.init(None)
counts = {"src": 0, "enc": 0}
seen_first = set()
pipe = None
if mode != "none":
    desc = PIPES[mode]
    log(f"pipeline {desc}")
    pipe = Gst.parse_launch(desc)

    def on_caps(pad, _pspec):
        caps = pad.get_current_caps()
        if caps:
            log(f"caps {pad.get_parent_element().get_name()}.{pad.get_name()}: {caps.to_string()}")

    def first_buffer_probe(pad, info, name):
        if name in seen_first:
            return Gst.PadProbeReturn.OK
        buf = info.get_buffer()
        if buf is None:
            return Gst.PadProbeReturn.OK
        seen_first.add(name)
        mem = buf.peek_memory(0)
        try:
            alloc = mem.allocator
            kind = f"{alloc.__gtype__.name}({alloc.get_name()})" if alloc is not None else "none"
        except Exception as e:  # noqa: BLE001
            kind = f"? ({e})"
        log(f"first buffer on {name}: n_memory={buf.n_memory()} allocator={kind} size={buf.get_size()}")
        return Gst.PadProbeReturn.OK

    it = pipe.iterate_elements()
    elements = []
    while True:
        r, e = it.next()
        if r != Gst.IteratorResult.OK:
            break
        elements.append(e)
    for e in elements:
        for pad in e.pads:
            pad.connect("notify::caps", on_caps)
            if pad.get_direction() == Gst.PadDirection.SRC:
                pad.add_probe(Gst.PadProbeType.BUFFER, first_buffer_probe, f"{e.get_name()}.{pad.get_name()}")

    def count(name):
        def probe(_pad, _info):
            counts[name] += 1
            return Gst.PadProbeReturn.OK
        return probe

    pipe.get_by_name("src").get_static_pad("src").add_probe(Gst.PadProbeType.BUFFER, count("src"))
    pipe.get_by_name("enc").get_static_pad("src").add_probe(Gst.PadProbeType.BUFFER, count("enc"))

loop = GLib.MainLoop()
state = {"phase": "warmup", "a": None, "gpu": [], "res": None}


def on_msg(_b, msg):
    if msg.type == Gst.MessageType.ERROR:
        err, dbg = msg.parse_error()
        log(f"ERROR gst {err.message} {dbg}")
        loop.quit()
    elif msg.type == Gst.MessageType.WARNING:
        err, dbg = msg.parse_warning()
        log(f"WARNING gst {err.message} {dbg}")
    elif msg.type == Gst.MessageType.STATE_CHANGED and msg.src == pipe:
        _o, new, _p = msg.parse_state_changed()
        log(f"pipeline {Gst.Element.state_get_name(new)}")


if pipe:
    bus = pipe.get_bus()
    bus.add_signal_watch()
    bus.connect("message", on_msg)
    pipe.set_state(Gst.State.PLAYING)


def gpu_sample():
    if state["phase"] == "window":
        try:
            with open(GPU) as f:
                state["gpu"].append(int(f.read().strip()))
        except (OSError, ValueError):
            pass
    return True


def start_window():
    state["phase"] = "window"
    state["a"] = snapshot()
    state["c0"] = dict(counts)
    log(f"window start counts={counts}")
    return False


def end_window():
    b = snapshot()
    a = state["a"]
    dt = b["t"] - a["t"]
    res = {"mode": mode, "seconds": round(dt, 2),
           "src_fps": round((counts["src"] - state["c0"]["src"]) / dt, 2), "enc_fps": round((counts["enc"] - state["c0"]["enc"]) / dt, 2)}
    for k in ("self", "gs", "pw", "win"):
        if a[k] is not None and b[k] is not None:
            res[f"cpu_{k}_pct"] = round(100.0 * (b[k] - a[k]) / dt, 1)
    total = b["sys"][0] - a["sys"][0]
    idle = b["sys"][1] - a["sys"][1]
    res["cpu_machine_pct_of_all_cores"] = round(100.0 * (total - idle) / total, 1) if total else None
    res["cpu_machine_cores_busy"] = round((total - idle) / dt, 2)
    res["gpu_busy_pct_mean"] = round(sum(state["gpu"]) / len(state["gpu"]), 1) if state["gpu"] else None
    res["gpu_samples"] = len(state["gpu"])
    state["phase"] = "done"
    log("RESULT " + json.dumps(res))
    with open(prefix + ".json", "w") as f:
        json.dump(res, f)
    loop.quit()
    return False


GLib.timeout_add(100, gpu_sample)
GLib.timeout_add(int(warmup * 1000), start_window)
GLib.timeout_add(int((warmup + window) * 1000), end_window)
GLib.timeout_add(200, lambda: (loop.quit() if os.path.exists(STOP) else None) or True)
loop.run()
if pipe:
    pipe.send_event(Gst.Event.new_eos())
    pipe.get_bus().timed_pop_filtered(3 * Gst.SECOND, Gst.MessageType.EOS | Gst.MessageType.ERROR)
    pipe.set_state(Gst.State.NULL)
log(f"end counts={counts}")
