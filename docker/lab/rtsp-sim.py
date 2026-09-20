#!/usr/bin/env python3
"""A network camera stand-in for the lab (docs/06 fjarr.camera `rtsp`, slice 4): GStreamer's RTSP
server serving a moving test pattern as H.264 on rtsp://<host>:8554/pattern. Software encoder only —
this is a fixture, not a robot."""
import gi

gi.require_version("Gst", "1.0")
gi.require_version("GstRtspServer", "1.0")
from gi.repository import GLib, Gst, GstRtspServer  # noqa: E402

Gst.init(None)
server = GstRtspServer.RTSPServer()
server.set_service("8554")
factory = GstRtspServer.RTSPMediaFactory()
factory.set_launch(
    "( videotestsrc is-live=true pattern=ball ! video/x-raw,width=1280,height=720,framerate=30/1 "
    "! videoconvert ! openh264enc bitrate=2000000 gop-size=30 ! h264parse ! rtph264pay name=pay0 pt=96 config-interval=1 )"
)
factory.set_shared(True)
server.get_mount_points().add_factory("/pattern", factory)
server.attach(None)
print("rtsp-sim: rtsp://0.0.0.0:8554/pattern", flush=True)
GLib.MainLoop().run()
