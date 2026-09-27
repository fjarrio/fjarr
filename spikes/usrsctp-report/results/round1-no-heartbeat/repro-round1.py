#!/usr/bin/env python3
"""Minimal reproducer: a usrsctp data channel that stops delivering for good.

Two webrtcbin peers in one process, data channel only, no signalling server, no Fjarr code.
One channel, unordered, max-retransmits=0 (the "partially reliable, unordered" class). Peer A
pushes fixed-size messages to B as fast as its buffered amount allows; B answers with a small
message for every 16 it receives (the reverse flow a TCP-over-datachannel tunnel has). The run
ends when TOTAL bytes have been offered, or as a STALL when B receives nothing for --stall
seconds while A is still sending.

GStreamer pins usrsctp's path MTU to 1200 (PMTUD off), so a message of up to 1184 bytes is one
DATA chunk and anything larger is two. The claim under test: at 1185+ an A->B stall appears and
never recovers; at <= 1184 it does not.

Loss is supplied from outside (netem), e.g. in a container with --network none --cap-add NET_ADMIN:
    tc qdisc add dev lo root netem loss 1%
Exit status: 0 completed, 2 stalled, 1 setup failure.
"""
import argparse
import struct
import sys
import threading
import time

import gi

gi.require_version("Gst", "1.0")
gi.require_version("GstWebRTC", "1.0")
gi.require_version("GstSdp", "1.0")
from gi.repository import GLib, Gst, GstSdp, GstWebRTC  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument("--size", type=int, default=1280, help="message size in bytes")
ap.add_argument("--total-mb", type=int, default=1024, help="bytes A offers before the run completes")
ap.add_argument("--stall", type=float, default=10.0, help="seconds without A->B delivery that count as a stall")
ap.add_argument("--high", type=int, default=1 << 20, help="A stops sending above this buffered amount")
ap.add_argument("--ordered", action="store_true", help="control arm: ordered channel")
ap.add_argument("--reliable", action="store_true", help="control arm: fully reliable channel")
ap.add_argument("--timeout", type=float, default=600.0)
args = ap.parse_args()

Gst.init(None)

# Count SCTP_SEND_FAILED_EVENT the only way gst-plugins-bad exposes it: a GST_ERROR log line on the
# sctpassociation category. Kept at ERROR on purpose — raising sctp* to INFO/DEBUG slows the send
# path enough to hide the fault.
send_failed = 0


def on_log(category, level, file, function, line, obj, message, *user):
    global send_failed
    if category.get_name() == "sctpassociation" and "SCTP_SEND_FAILED" in (message.get() or ""):
        send_failed += 1


if "GST_DEBUG" not in __import__("os").environ:
    Gst.debug_remove_log_function(Gst.debug_log_default)  # else every event is also a stderr line
Gst.debug_add_log_function(on_log, None)
Gst.debug_set_threshold_for_name("sctpassociation", Gst.DebugLevel.ERROR)

loop = GLib.MainLoop()
t0 = time.monotonic()
state = {
    "a_sent": 0, "a_bytes": 0, "b_recv": 0, "b_bytes": 0, "b_sent": 0, "a_recv": 0,
    "last_b_recv": t0, "last_a_recv": t0, "open": False, "result": None,
}
chan = {}


def log(msg):
    print(f"[{time.monotonic() - t0:7.2f}s] {msg}", flush=True)


def make_peer(name):
    pipe = Gst.Pipeline.new(name)
    wb = Gst.ElementFactory.make("webrtcbin", name)
    wb.set_property("bundle-policy", GstWebRTC.WebRTCBundlePolicy.MAX_BUNDLE)
    pipe.add(wb)
    pipe.set_state(Gst.State.PLAYING)
    return pipe, wb


pipe_a, a = make_peer("a")
pipe_b, b = make_peer("b")

a.connect("on-ice-candidate", lambda _w, m, c: b.emit("add-ice-candidate", m, c))
b.connect("on-ice-candidate", lambda _w, m, c: a.emit("add-ice-candidate", m, c))

opts = Gst.Structure.new_empty("opts")
if not args.ordered:
    opts.set_value("ordered", False)
if not args.reliable:
    opts.set_value("max-retransmits", 0)
dc_a = a.emit("create-data-channel", "bulk", opts)
if dc_a is None:
    sys.exit("create-data-channel failed")
chan["a"] = dc_a
log(f"channel ordered={dc_a.props.ordered} max-retransmits={dc_a.props.max_retransmits} size={args.size}")

HDR = struct.Struct("!Q")
payload_pad = b"\x5a" * (args.size - HDR.size)
total = args.total_mb << 20


def pump():
    """Send while the buffered amount is under --high; the low-threshold callback re-arms it."""
    if state["result"]:
        return False
    for _ in range(256):
        if state["a_bytes"] >= total:
            state.setdefault("a_done_at", time.monotonic())
            return False
        if dc_a.props.buffered_amount > args.high:
            return False
        dc_a.send_data_full(GLib.Bytes.new(HDR.pack(state["a_sent"]) + payload_pad))
        state["a_sent"] += 1
        state["a_bytes"] += args.size
    return True


pumping = threading.Lock()


def arm_pump():
    if pumping.acquire(blocking=False):
        def run():
            more = pump()
            if not more:
                pumping.release()
            return more
        GLib.idle_add(run)


def on_a_open(dc):
    state["open"] = True
    state["last_b_recv"] = state["last_a_recv"] = time.monotonic()
    log("channel open, sending")
    GLib.idle_add(lambda: (arm_pump(), False)[1])


dc_a.set_property("buffered-amount-low-threshold", args.high // 4)
dc_a.connect("on-buffered-amount-low", lambda dc: GLib.idle_add(lambda: (arm_pump(), False)[1]))
dc_a.connect("on-open", on_a_open)


def on_a_msg(dc, data):
    state["a_recv"] += 1
    state["last_a_recv"] = time.monotonic()


dc_a.connect("on-message-data", on_a_msg)


def on_b_channel(_wb, dc):
    chan["b"] = dc
    ack = GLib.Bytes.new(b"\x00" * 64)

    def on_b_msg(dc, data):
        state["b_recv"] += 1
        state["b_bytes"] += data.get_size()
        state["last_b_recv"] = time.monotonic()
        if state["b_recv"] % 16 == 0:
            dc.send_data_full(ack)
            state["b_sent"] += 1

    dc.connect("on-message-data", on_b_msg)


b.connect("on-data-channel", on_b_channel)


def on_answer(promise, _):
    reply = promise.get_reply()  # keep the structure alive: the SDP below points into it
    answer = reply.get_value("answer")
    b.emit("set-local-description", answer, None)
    a.emit("set-remote-description", answer, None)


def on_offer(promise, _):
    reply = promise.get_reply()
    offer = reply.get_value("offer")
    a.emit("set-local-description", offer, None)
    b.emit("set-remote-description", offer, None)
    b.emit("create-answer", None, Gst.Promise.new_with_change_func(on_answer, None))


a.emit("create-offer", None, Gst.Promise.new_with_change_func(on_offer, None))

last = {"b_recv": 0, "a_recv": 0, "a_sent": 0}


def tick():
    now = time.monotonic()
    s = state
    log(f"A sent {s['a_sent']:>8} ({s['a_bytes'] >> 20:>5} MiB, buffered {dc_a.props.buffered_amount:>8})"
        f" | B got {s['b_recv']:>8} (+{s['b_recv'] - last['b_recv']:>6}/s)"
        f" | B->A {s['b_sent']:>6} sent, {s['a_recv']:>6} got (+{s['a_recv'] - last['a_recv']}/s)"
        f" | send-failed {send_failed}")
    for k in last:
        last[k] = s[k]
    if not s["open"]:
        if now - t0 > 20:
            finish("SETUP", "channel never opened")
        return True
    done_at = s.get("a_done_at")
    if done_at and now - s["last_b_recv"] > 2 and s["last_b_recv"] > done_at - 1:
        finish("COMPLETED", "A offered every byte and B drained")
    elif now - s["last_b_recv"] > args.stall:
        finish("STALL", f"B has received nothing for {now - s['last_b_recv']:.0f}s; "
                        f"B->A last delivery {now - s['last_a_recv']:.0f}s ago")
    elif now - t0 > args.timeout:
        finish("TIMEOUT", "")
    return True


def finish(result, why):
    if state["result"]:
        return
    state["result"] = result
    s = state
    delivered = 100.0 * s["b_recv"] / max(s["a_sent"], 1)
    log(f"RESULT {result} size={args.size}: {why}; A sent {s['a_sent']}, B received {s['b_recv']}"
        f" ({delivered:.2f} %), send-failed events {send_failed}")
    loop.quit()


GLib.timeout_add(1000, tick)
try:
    loop.run()
finally:
    pipe_a.set_state(Gst.State.NULL)
    pipe_b.set_state(Gst.State.NULL)
sys.exit({"COMPLETED": 0, "STALL": 2}.get(state["result"], 1))
