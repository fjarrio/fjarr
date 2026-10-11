# Throwaway (3.5.2): can the session helper hand the agent the desktop's sound — the default sink's
# monitor — through a connection narrowed like the screen's, and nothing else (not the microphone)?
# Run in the desktop fixture as the desktop user, with spikes/pipewire-narrowing/narrow.c at /tmp/narrow.
import json, os, subprocess, sys, time

def dump():
    return json.loads(subprocess.check_output(["pw-dump"]))

def node_by_name(name):
    for o in dump():
        if o.get("info", {}).get("props", {}).get("node.name") == name:
            return o["id"], o["info"]["props"]["object.serial"]
    return None

def gst(desc):
    code = ("import gi,sys; gi.require_version('Gst','1.0'); from gi.repository import Gst, GLib; Gst.init(None);"
            "p=Gst.parse_launch(sys.argv[1]); p.set_state(Gst.State.PLAYING); GLib.MainLoop().run()")
    return subprocess.Popen(["python3", "-c", code, desc])

# A speaker (a null sink, as a robot with no sound card is given), the default.
subprocess.run(["pw-cli", "create-node", "adapter", "{ factory.name=support.null-audio-sink node.name=spike-speakers "
                "media.class=Audio/Sink object.linger=true audio.position=[FL FR] }"], check=True, capture_output=True)
time.sleep(1)
sink, sink_serial = node_by_name("spike-speakers")
subprocess.run(["wpctl", "set-default", str(sink)], check=True)
# A desktop app playing 440 Hz to the default output (pw-play: GStreamer's pipewiresink never started
# the graph here), and a live microphone at 1000 Hz.
import wave, struct, math
w = wave.open("/tmp/tone.wav", "wb"); w.setnchannels(1); w.setsampwidth(2); w.setframerate(48000)
w.writeframes(b"".join(struct.pack("<h", int(12000 * math.sin(2 * math.pi * 440 * i / 48000))) for i in range(48000 * 60)))
w.close()
player = subprocess.Popen(["pw-play", "/tmp/tone.wav"])
mic = gst("audiotestsrc is-live=true freq=1000 ! audio/x-raw,rate=48000,channels=2 ! pipewiresink mode=provide "
          "stream-properties=props,media.class=Audio/Source,node.name=spike-mic")
time.sleep(2)
mic_id, mic_serial = node_by_name("spike-mic")
factory = next(o["id"] for o in dump() if o.get("type", "").endswith("Factory") and o.get("info", {}).get("props", {}).get("factory.name") == "client-node")
links = [(o["info"]["output-node-id"], o["info"]["input-node-id"]) for o in dump() if o.get("type", "").endswith(":Link")]
print(f"sink {sink} serial {sink_serial}; mic {mic_id} serial {mic_serial}; factory {factory}; links {links}", flush=True)

# The capture: S16 mono at 48 kHz, RMS and the zero crossing rate (frequency) of a second's audio.
CODE = r'''
import gi, sys, struct, math; gi.require_version("Gst","1.0"); from gi.repository import Gst; Gst.init(None)
p = Gst.parse_launch("pipewiresrc fd=3 SRC ! audioconvert ! audioresample ! audio/x-raw,format=S16LE,rate=48000,channels=1 ! appsink name=s sync=false")
p.set_state(Gst.State.PLAYING); s = p.get_by_name("s"); n = 0; data = b""
import time; t0 = time.time()
while time.time() - t0 < 6 and len(data) < 96000:
    smp = s.emit("try-pull-sample", Gst.SECOND)
    if smp:
        b = smp.get_buffer(); data += b.extract_dup(0, b.get_size())
v = struct.unpack("<%dh" % (len(data) // 2), data[: len(data) // 2 * 2])
rms = math.sqrt(sum(x * x for x in v) / len(v)) if v else 0
zc = sum(1 for i in range(1, len(v)) if (v[i - 1] < 0) != (v[i] < 0))
print("samples", len(v), "rms", round(rms), "freq~", round(zc / 2 / (len(v) / 48000)) if v else 0)
p.set_state(Gst.State.NULL)
'''

def run(label, node, src, narrow=True):
    env = dict(os.environ, PW_CLIENT_NODE_FACTORY=str(factory))
    if not narrow: env["PW_NO_NARROW"] = "1"
    try:
        r = subprocess.run(["/tmp/narrow", "narrow", str(node), "python3", "-c", CODE.replace("SRC", src)], capture_output=True, text=True, timeout=20, env=env)
        print(f"{label}: {(r.stdout + r.stderr).strip().splitlines()[-1:]}", flush=True)
    except subprocess.TimeoutExpired:
        print(f"{label}: TIMEOUT", flush=True)

CAP = "stream-properties=props,stream.capture.sink=true"
run("control, unnarrowed, the sink's monitor by serial", sink, f"target-object={sink_serial} {CAP}", narrow=False)
run("narrowed to the sink, its monitor by serial", sink, f"target-object={sink_serial} {CAP}")
run("narrowed to the sink, the default sink's monitor (no target)", sink, CAP)
run("narrowed to the sink, the microphone by serial", sink, f"target-object={mic_serial}")
run("narrowed to the sink, the default source", sink, "")
player.kill(); mic.kill()
