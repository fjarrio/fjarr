#!/usr/bin/env python3
"""A network camera stand-in for the lab (docs/06 fjarr.camera `rtsp`, slices 4 and 6b): GStreamer's
RTSP server serving a moving test pattern as H.264 on rtsp://<host>:8554/pattern, and the same
pattern as a low-resolution substream on /pattern-low — what a real network camera offers, and what
a passthrough track uses as its thumbnail tier. Software encoder only: this is a fixture, not a robot."""
import gi

gi.require_version("Gst", "1.0")
gi.require_version("GstRtspServer", "1.0")
from gi.repository import GLib, Gst, GstRtspServer  # noqa: E402

Gst.init(None)
server = GstRtspServer.RTSPServer()
server.set_service("8554")


def mount(path: str, width: int, height: int, fps: int, bitrate: int) -> None:
    factory = GstRtspServer.RTSPMediaFactory()
    factory.set_launch(
        f"( videotestsrc is-live=true pattern=ball ! video/x-raw,width={width},height={height},framerate={fps}/1 "
        f"! videoconvert ! openh264enc bitrate={bitrate} gop-size={fps * 2} ! h264parse "
        "! rtph264pay name=pay0 pt=96 config-interval=1 )"
    )
    factory.set_shared(True)
    server.get_mount_points().add_factory(path, factory)


mount("/pattern", 1280, 720, 30, 2000000)
mount("/pattern-low", 640, 360, 5, 300000)  # the substream: a passthrough track's thumbnail tier (docs/06)
server.attach(None)
print("rtsp-sim: rtsp://0.0.0.0:8554/pattern and /pattern-low", flush=True)
GLib.MainLoop().run()
