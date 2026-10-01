# Throwaway: pipewiresrc through a narrowed connection — the screen works, the microphone does not.
import json, subprocess, importlib.machinery, importlib.util, dbus, dbus.mainloop.glib
from gi.repository import GLib
spec = importlib.util.spec_from_loader("selftest", importlib.machinery.SourceFileLoader("selftest", "/usr/local/bin/fixture-selftest"))
st = importlib.util.module_from_spec(spec); spec.loader.exec_module(st)
dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
bus, rd, stream = st.session()
loop = GLib.MainLoop(); node = {}
bus.add_signal_receiver(lambda n: (node.setdefault("id", int(n)), loop.quit()), "PipeWireStreamAdded", st.SC + ".Stream", path=stream)
rd.Start(); GLib.timeout_add_seconds(5, loop.quit); loop.run()
import time
# A microphone that really produces: a live Audio/Source node playing a tone.
micproc = subprocess.Popen(["python3", "-c", "import gi; gi.require_version('Gst','1.0'); from gi.repository import Gst, GLib; Gst.init(None);"
    "p=Gst.parse_launch('audiotestsrc is-live=true ! audio/x-raw,rate=48000,channels=2 ! pipewiresink mode=provide stream-properties=\"props,media.class=Audio/Source,node.name=live-mic\"');"
    "p.set_state(Gst.State.PLAYING); GLib.MainLoop().run()"])
time.sleep(2)
mic = next(o["id"] for o in json.loads(subprocess.check_output(["pw-dump"])) if o.get("info", {}).get("props", {}).get("node.name") == "live-mic")
import os
dump = json.loads(subprocess.check_output(["pw-dump"]))
factory = next(o["id"] for o in dump if o.get("type", "").endswith("Factory") and o.get("info", {}).get("props", {}).get("factory.name") == "client-node")
os.environ["PW_CLIENT_NODE_FACTORY"] = str(factory)
print("screen node", node["id"], "mic node", mic, "client-node factory", factory, flush=True)
serial = lambda nid: next(o["info"]["props"].get("object.serial") for o in json.loads(subprocess.check_output(["pw-dump"])) if o["id"] == nid)
def run(prop, label, narrow=True, audio=False):
    code = ("import gi,sys; gi.require_version('Gst','1.0'); from gi.repository import Gst; Gst.init(None);"
            f"p=Gst.parse_launch('pipewiresrc fd=3 {prop} always-copy=true ! {'audioconvert ! audio/x-raw' if audio else 'videoconvert ! video/x-raw,format=RGB'} ! appsink name=s max-buffers=1 drop=true sync=false');"
            "p.set_state(Gst.State.PLAYING); s=p.get_by_name('s').emit('try-pull-sample', 5*Gst.SECOND);"
            "m=p.get_bus().pop_filtered(Gst.MessageType.ERROR); print('FRAME' if s else 'NO FRAME', m.parse_error()[0].message if m else '');"
            "p.set_state(Gst.State.NULL); sys.exit(0 if s else 1)")
    env = dict(os.environ)
    if not narrow: env["PW_NO_NARROW"] = "1"
    try:
        r = subprocess.run(["/tmp/narrow", "narrow", str(node["id"]), "python3", "-c", code], capture_output=True, text=True, timeout=15, env=env)
        print(f"{label}: exit {r.returncode}", (r.stdout + r.stderr).strip().splitlines()[-1:], flush=True)
    except subprocess.TimeoutExpired as e:
        print(f"{label}: TIMEOUT", (e.stdout or b"").decode().strip()[-200:], flush=True)
s_screen, s_mic = serial(node["id"]), serial(mic)
print("serials: screen", s_screen, "mic", s_mic, flush=True)
run(f"path={node['id']}", "control, unnarrowed, path=id", narrow=False)
run(f"target-object={s_screen}", "control, unnarrowed, target-object=serial", narrow=False)
run(f"path={node['id']}", "narrowed, path=id")
run(f"target-object={s_screen}", "narrowed, target-object=serial")
run(f"target-object={s_mic}", "control, unnarrowed, the MICROPHONE by serial", narrow=False, audio=True)
run(f"target-object={s_mic}", "narrowed, the MICROPHONE by serial", audio=True)
run(f"target-object=live-mic", "narrowed, the MICROPHONE by name", audio=True)
run(f"path={mic}", "narrowed, the MICROPHONE by id", audio=True)
run("", "narrowed, no target (the default source)", audio=True)
micproc.kill()
rd.Stop()
