#!/usr/bin/env python3
# Monitor hot-plug for the X11 fixture (docs/07#simulating-hot-plug, M3 3.7): GET /plug carves a
# 1280x720 RandR monitor right of the first, GET /unplug removes it. Xvfb's root cannot grow, so the
# fixture starts with a 3840x1080 root and monitors are carved out of it.
import subprocess
from http.server import BaseHTTPRequestHandler, HTTPServer

NAME = "RIGHT"


def xrandr(*args):
    return subprocess.run(["xrandr", *args], capture_output=True, text=True, timeout=5)


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/plug":
            r = xrandr("--setmonitor", NAME, "1280/340x720/190+1920+0", "none")
            code, body = (200, NAME) if r.returncode == 0 else (500, r.stderr)
        elif self.path == "/unplug":
            r = xrandr("--delmonitor", NAME)
            code, body = (200, NAME) if r.returncode == 0 else (404, r.stderr or "nothing plugged")
        elif self.path == "/":
            code, body = 200, "plugd: /plug, /unplug"
        else:
            code, body = 404, "/plug or /unplug"
        data = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *args):
        pass


HTTPServer(("0.0.0.0", 8091), Handler).serve_forever()
