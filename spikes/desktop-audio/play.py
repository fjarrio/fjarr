# Throwaway: a desktop app playing a tone to the default output (3.5.2 spike).
import sys, gi
gi.require_version("Gst", "1.0")
from gi.repository import Gst, GLib
Gst.init(None)
freq = sys.argv[1] if len(sys.argv) > 1 else "440"
p = Gst.parse_launch(f"audiotestsrc is-live=true freq={freq} ! audio/x-raw,rate=48000,channels=2 ! pipewiresink "
                     "stream-properties=props,node.name=spike-player,media.class=Stream/Output/Audio")
p.set_state(Gst.State.PLAYING)
bus = p.get_bus()
bus.add_signal_watch()
bus.connect("message::error", lambda b, m: (print("player error", m.parse_error()), sys.exit(1)))
GLib.MainLoop().run()
