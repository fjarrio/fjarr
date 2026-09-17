# Spike: webrtcbin loopback probe (1.24, re-run on 1.28)

Timeboxed technical spike answering six questions about GStreamer `webrtcbin`
behaviour that the agent architecture (docs/09, docs/21, docs/23) depends on.
A single process runs two `webrtcbin` instances — **A = "agent" (offerer)**
and **B = "browser" (answerer)** — and exchanges SDP/ICE through GLib
main-loop idle callbacks (no network signaling). Every question is answered
with PASS/FAIL/UNCLEAR plus the exact API used, and the raw evidence is in
`results/` (1.24) and `results/gst-1.28/` (the slice-2.9 re-run, see
[below](#re-run-on-gstreamer-128)).

This directory is **not** referenced by the root build and is not a product
of the spec workflow; it is throwaway evidence that feeds the architecture doc.
Nothing under `agent/src`, `docs/`, `signaling/` or `web/` was touched.

## Environment (dev container)

| Component | Original run (2026-09-17) | Re-run (2026-09-18, ADR-0022) |
|---|---|---|
| OS | Ubuntu 24.04 | Ubuntu 26.04 LTS |
| GStreamer | 1.24.2 (`gstreamer1.0-plugins-bad` 1.24.2-1ubuntu4) | 1.28.2 |
| libnice / gstreamer1.0-nice | 0.1.21-2build3 | 0.1.23-2 |
| Compiler | g++ 13.3.0 (clang 18.1.3 also present), C++20 | g++ 15.2.0 (clang 21.1.8 also present), C++20 |
| CMake / Ninja | 3.28.3 / 1.11.1 | 4.2.3 / 1.13.2 |
| Encoder used | `vp8enc` (BSD) — `x264enc` is absent by design; `openh264enc` and `vah264enc` are also available | same |

## Build and run (from scratch)

```bash
docker compose up -d dev                                       # once
docker compose exec -T dev bash -c '
  cd /workspace/agent/spikes/webrtcbin-probe &&
  rm -rf build &&
  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo &&
  cmake --build build &&
  ./build/webrtcbin-probe --bundle=max-bundle --remove=inactive'
```

Flags: `--bundle=none|balanced|max-bundle` (default `max-bundle`),
`--remove=inactive|sendonly|release-pad` (default `inactive`, selects how
track 2 is "removed" in Q3) and `--reuse-pads` (sets `reuse-source-pads=TRUE`
on both webrtcbins where the property exists, i.e. GStreamer ≥ 1.26; a
no-op on 1.24). The program runs all questions sequentially,
prints a `RESULT` line per check and a summary table, and exits 0 on
completion (1 if a prerequisite such as the first connection fails, 3 on the
120 s watchdog). Full outputs of the four configurations discussed below are
in `results/run-bundle-<policy>-remove-<mode>.txt`. `GST_DEBUG=webrtcbin:5`
is the useful debug env; `NICE_DEBUG=nice` prints libnice internals.

## Summary

Configuration: `--bundle=max-bundle` unless stated. "B" is the webrtcbin
answerer, which in Fjarr will be a browser, so answerer-side bugs are noted
but do not necessarily affect the agent.

| Q | Check | Verdict | Evidence (short) |
|---|---|---|---|
| Q1 | Data channel created before create-offer appears as `m=application` | **PASS** | offer: `m=0:video mid=video0`, `m=1:application mid=application1` |
| Q1 | B gets `on-data-channel`, A→B string arrives | **PASS** | `B: on-data-channel label=control id=1 ordered=1`; `B: on-message-string "hello from A"` |
| Q1 | `buffered-amount`, `buffered-amount-low-threshold`, `notify::buffered-amount`, `on-buffered-amount-low` | **PASS** | 64 × 32 KiB burst: buffered-amount peaked at 1 736 704, ended at 0; 132 notifies; low fired 4× (2 of them spuriously at open) |
| Q1 | `create-data-channel` in NULL state | **FAIL (by design)** | `CRITICAL: gst_webrtc_bin_create_data_channel: assertion 'webrtc->priv->is_closed != TRUE' failed` → returns NULL; works once the pipeline is READY/PLAYING |
| Q2 | Caps-gated offer via pad probe on payloader src | **PASS** | fixed caps with `payload=(int)96, ssrc=(uint)…` seen 3 ms after PLAYING, before create-offer |
| Q2 | When does transceiver `mid` become non-NULL | **PASS** (answered) | NULL before create-offer, NULL after create-offer, **NULL after set-local-description**, `video0` only after the answer is applied (signaling-state `stable`). The offer SDP carries `a=mid:video0` from the start |
| Q3 | `on-negotiation-needed` after requesting a 2nd sink pad | **PASS** | fires once, immediately after `sink_2` is requested/linked |
| Q3a | Renegotiated offer keeps `mid` of m=0, adds a new m-section | **PASS** | `m=0 mid=video0` unchanged, new `m=2:video mid=video2` (m=1 is the data channel) |
| Q3b | Track 1 keeps flowing during add-track renegotiation | **PASS** | buffers 61→117 over the step, max inter-buffer gap **34.8 ms** (< 200 ms) |
| Q3c | B `pad-added` + `on-new-transceiver` for track 2, media flows | **PASS** | `B: pad-added src_1 transceiver mid=video2 mlineindex=2`; 31 buffers in < 1 s |
| Q3 | Remove track via `direction=inactive` + renegotiate | **FAIL** | SDP is right (`a=inactive` both ways) and track 2 stops, but **B stops reading its ICE socket**: kernel `OutDatagrams=+67 InDatagrams=+0 RcvbufErrors=+47`; A→B data channel lost, B→A still arrives; never recovers |
| Q3 | Remove track via `direction=sendonly` + renegotiate | **PASS** | harmless direction change; everything keeps flowing; combine with `valve drop=true` to stop the media |
| Q3 | Remove track via `gst_element_release_request_pad` | **FAIL** (as a removal) | pad goes away and track 2 stops, but the transceiver stays `sendrecv` in the next offer and **no** `on-negotiation-needed` fires; track 1 unaffected |
| Q4 | ICE restart via `create-offer` options `ice-restart`/`iceRestart` | **FAIL** | options are received (`creating offer sdp with options options, ice-restart=(boolean)true…`) but ignored: `sdp_media_from_transceiver: … Using previous ice parameters`; ufrag/pwd identical; no ICE state change. No `ice-restart` string exists in `libgstwebrtc.so` 1.24.2 |
| Q5 | `get-stats` types and per-track counters | **PASS** | types: candidate-pair, codec, inbound-rtp, local-candidate, outbound-rtp, peer-connection, remote-candidate, remote-inbound-rtp, remote-outbound-rtp, transport. `outbound-rtp` has `ssrc`, `bytes-sent`, `packets-sent` (guint64) → 76 kbit/s, 29 pkt/s over 1 s |
| Q6 | `turn-server` property and `add-turn-server` signal | **PASS** | both accept `turn(s)://user:pass@host:port[?transport=tcp]`; `add-turn-server` returns FALSE for a non-turn URL |
| Q6 | `bundle-policy=none` (the default!) | **FAIL** | media transport connects but the data-channel transport never does: `B: connection-state -> failed` after 5 s, `A: connecting`; UNCLEAR root cause (ICE reached `completed` on both, DTLS on the second transport did not) |

## Findings in detail

### Q1 — data channels

* API: `g_signal_emit_by_name(webrtc, "create-data-channel", "control", NULL /*GstStructure options*/, &channel)` returns a `GstWebRTCDataChannel*` (transfer full). It must be called with webrtcbin at least READY — in NULL it asserts and returns NULL. Called before the first `create-offer`, the channel becomes `m=application` in the offer and B's `on-data-channel` fires ~1 ms after `connection-state=connected`.
* Send: `gst_webrtc_data_channel_send_string_full()` / `gst_webrtc_data_channel_send_data_full(GBytes*, GError**)` (the non-`_full` variants are deprecated). Receive: `on-message-string(gchar*)`, `on-message-data(GBytes*)`.
* `GstWebRTCDataChannel` properties (runtime enumeration): `label, ordered, max-packet-lifetime, max-retransmits, protocol, negotiated, id, priority, ready-state(ro), buffered-amount(guint64, ro), buffered-amount-low-threshold(guint64, rw)`. Signals: `on-open on-close on-error on-message-data on-message-string on-buffered-amount-low send-data send-string close`. There is no `max-message-size` on the channel; it lives on the `sctp-transport` object (`max-message-size` = 65536 here).
* Back-pressure works: a 2 MiB burst of 32 KiB messages left `buffered-amount=1 736 704` immediately after the loop, drained to 0 in ~0.2 s, `notify::buffered-amount` fired 132 times and `on-buffered-amount-low` fired when crossing the 4096 threshold. Caveat: `on-buffered-amount-low` also fires twice right at open with nothing buffered, so treat it as level-triggered.
* Reliability classes are set through the options GstStructure of `create-data-channel` (`ordered`, `max-retransmits`, `max-packet-lifetime`, `protocol`, `negotiated`, `id`) — not exercised here.

### Q2 — caps-gated offer and `mid`

* Pipeline: `videotestsrc is-live=1 ! video/x-raw,320x240@30 ! vp8enc deadline=1 cpu-used=4 target-bitrate=300000 ! rtpvp8pay pt=96 ! valve ! webrtcbin.sink_%u`.
* Gate: `gst_pad_add_probe(pay_src, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, …)`, look for `GST_EVENT_CAPS`, accept when `gst_caps_is_fixed()` and the structure has `payload` and `ssrc`. Fires 1–2 ms after PLAYING, well before any offer. (A second caps event with reordered fields arrives after negotiation — harmless.) Probing the payloader src pad is preferable to the webrtcbin sink pad because webrtcbin installs its own blocking probe on the sink pad until the transport is ready.
* `sink_%u` request pads map to m-line indexes: after the data channel took `m=1`, the second `gst_element_request_pad_simple(webrtc, "sink_%u")` returned **`sink_2`** and the offer got `m=2`.
* `mid`: readable via the transceiver's `mid` property (`g_object_get(trans, "mid", …)`), obtained with the `get-transceiver`(index) / `get-transceivers` action signals or the `transceiver` property of a webrtcbin pad. It is **NULL until the negotiation reaches `stable`** (i.e. after A applies the answer) — not after create-offer, not after set-local-description. For a manifest built at offer time, parse `a=mid` from the offer SDP (`gst_sdp_media_get_attribute_val(media, "mid")`), which webrtcbin generates as `<kind><mline>` (`video0`, `application1`, `video2`). The `GstWebRTCRTPTransceiver` struct is opaque; use properties: `mlineindex, mid, direction(rw), current-direction, kind, sender, receiver, codec-preferences`.

### Q3 — mid-session renegotiation

* Adding a track while connected: request a new sink pad and link the caps-gated branch; `on-negotiation-needed` fires once. The new offer reuses the negotiated transceivers ("using previous negotiated transceiver … with mid video0 into media index 0") and appends the new one. Track 1 did not skip a beat: max gap 34–36 ms in every run (frame period 33 ms).
* Answerer side: B's `on-new-transceiver` fires on set-remote-description, but **`pad-added` fires ~200 ms later, only when the first RTP packet with the new SSRC arrives**. B's src pads are named `src_<counter>` (`src_1` for m=2) — the number is *not* the m-line index; map via the pad's `transceiver` property → `mid` / `mlineindex`.
* Removing a track — three methods tried, `results/` has one file each:
  * `direction=inactive` on A's transceiver → `on-negotiation-needed` fires, offer/answer show `a=inactive`, A even keeps pushing track-2 RTP into the transport (+37 pkt/s) until a `valve` stops it. But B's receive path dies: kernel counters over 1 s show `OutDatagrams=+67 InDatagrams=+0 RcvbufErrors=+47` (datagrams reach B's socket, nobody reads them), B's `nicesrc` counts 0 buffers, `packets-received` stays flat for both SSRCs, the A→B data-channel ping is lost while B→A arrives, and a later renegotiation does not recover it. B's `webrtc-B:ice` thread is alive in `poll()`; libnice logs nothing (gdb attach is not permitted in the container, so no stack). Root cause not identified within the timebox; it is an answerer-side webrtcbin/libnice defect, so a browser answerer may or may not be affected — **do not use `inactive` on 1.24 until verified against a real browser.**
  * `direction=sendonly` → SDP correct, everything keeps flowing, data channel fine in both directions. Since A's transceivers are offered `sendrecv` and answered `recvonly`, `sendonly` is the honest direction for the agent anyway.
  * `gst_element_release_request_pad(webrtc, sink_2)` → track stops (source valved first), but the transceiver survives, the next offer still says `sendrecv`, and no `on-negotiation-needed` fires. Not a removal in SDP terms.
  * Conclusion for the architecture: keep transceivers stable for the session and gate media with a per-track `valve drop=true` (no renegotiation, zero impact on other tracks — verified: track 1 unaffected in all runs), matching the docs/09 idiom "per-track valve instead of renegotiation". Add tracks by renegotiation only.

### Q4 — ICE restart

* `create-offer` takes a `GstStructure* options`; webrtcbin 1.24.2 logs it (`creating offer sdp with options …`) and then ignores it: `sdp_media_from_transceiver: … Using previous ice parameters`. `strings libgstwebrtc.so` has no `ice-restart`/`iceRestart`, and there is no restart entry point on `GstWebRTCICE`. Both spellings were passed in one structure; ufrag/pwd stayed identical and no `ice-connection-state` change occurred.
* Consequence: on 1.24 an ICE restart means tearing down the session (new webrtcbin) and re-offering. Re-check the plugin strings after any GStreamer upgrade (`strings /usr/lib/x86_64-linux-gnu/gstreamer-1.0/libgstwebrtc.so | grep -i restart`).

### Q5 — stats

* `g_signal_emit_by_name(webrtc, "get-stats", NULL /*pad or NULL for all*/, promise)`. The reply is a `GstStructure` whose fields are sub-structures with a `type` field (`GstWebRTCStatsType`). Present with one video track: `candidate-pair codec inbound-rtp local-candidate outbound-rtp peer-connection remote-candidate remote-inbound-rtp remote-outbound-rtp transport`.
* `outbound-rtp` sample: `id=rtp-outbound-stream-stats_<ssrc>, ssrc, codec-id=codec-stats-sink_0, transport-id, kind=video, bytes-sent(guint64), packets-sent(guint64), fir-count, pli-count, nack-count, remote-id, gst-rtpsource-stats`. Two calls 1 s apart give a clean per-track bandwidth report (76–79 kbit/s, 29–30 pkt/s here). `inbound-rtp` has `bytes-received`/`packets-received` the same way. Passing a webrtcbin pad restricts the reply to that transceiver.

### Q6 — properties

* `latency` default 200 ms (jitterbuffer); `ice-transport-policy` `all`|`relay`; `bundle-policy` `none`(default)|`balanced`|`max-compat`|`max-bundle`.
* **Use `bundle-policy=max-bundle`.** With the default `none` the loopback got media but the data-channel transport never reached connected (B failed after 5 s) — and browsers negotiate BUNDLE anyway. All Q3/Q4 results above are with max-bundle; with bundling, all m-sections share one ICE/DTLS transport, one `nicesrc`/`nicesink` pair and one `rtpfunnel`, which is why an answerer-side stall on one m-line took every track and the data channel with it.
* TURN: `g_object_set(webrtc, "turn-server", "turn://user:pass@host:port?transport=tcp", NULL)` (single server, read back verbatim) or `g_signal_emit_by_name(webrtc, "add-turn-server", "turns://user:pass@host:5349", &ok)` for several; `turn(s)://timestamp:username:password@host:port` for time-limited credentials (escape `:` and base64 as the property doc says). `stun-server` is `stun://host:port`. No STUN/TURN was configured for the loopback (host candidates only).
* `on-negotiation-needed` also fires once on B when it goes to PLAYING with nothing to negotiate, and once on A when the first sink pad is linked — always gate it on your own state.

## Re-run on GStreamer 1.28

Slice 2.9 (ADR-0022) rebuilt the probe unchanged (plus the `--reuse-pads`
flag) on the 26.04 image and ran five configurations; full outputs are in
`results/gst-1.28/`. Everything that passed on 1.24 passes on 1.28 with the
same numbers (renegotiation gap 34.3–34.8 ms, 2 MiB burst drained, stats
types identical, `max-message-size` still 65 536). What changed, and what
did not:

| Configuration | Q3 removal | DC after removal (A→B / B→A) | Track 1 after valve | Kernel counters (1 s) | Q4 ICE restart |
|---|---|---|---|---|---|
| `--remove=inactive` (default props) | stall, as on 1.24 | **lost** / ok | **+0** | `InDatagrams=+0 RcvbufErrors=+48` | FAIL, ufrag unchanged |
| `--remove=inactive --reuse-pads` | no stall | ok / ok | +45 | `InDatagrams=+69 RcvbufErrors=+0` | FAIL, ufrag unchanged |
| `--remove=sendonly` | PASS | ok / ok | +45 | clean | FAIL, ufrag unchanged |
| `--remove=sendonly --reuse-pads` | PASS | ok / ok | +45 | clean | FAIL, ufrag unchanged |
| `--bundle=none --remove=inactive` | n/a | B `connection-state → failed` after 5 s, DC never opens (as on 1.24) | — | — | — |

* **The `inactive` stall is the answerer's EOS, and it is still there on
  1.28 by default.** `reuse-source-pads` (added in 1.26, default FALSE:
  "If FALSE, webrtcbin will send EOS on source pads with inactive
  transceivers") removes it completely: no receive-buffer errors, the data
  channel works in both directions, the untouched track keeps flowing
  before and after the valve. The property lives on the *answerer*; the
  offerer needs nothing. A browser answerer has no EOS mechanic.
* The probe still prints `Q3-remove FAIL` for the `--reuse-pads` run
  because its stop criterion is "track 2 buffers stop at B": with
  `reuse-source-pads` B keeps delivering the packets A keeps pushing (+57
  in 1.5 s) until A's valve closes — after which `Q3-rmvalve` shows track 2
  at +0 and track 1 at +45. Direction `inactive` in the SDP stops nothing
  by itself on either side; **close the valve before re-offering.**
* On 1.28 the offerer's transceiver reports `current-direction=sendonly`
  once negotiated (1.24 reported `sendrecv`); the `mid` timing with
  `max-bundle` is unchanged (NULL until the answer is applied). With
  `bundle-policy=none` the `mid` appears after `set-local-description`, but
  that policy still never connects the data channel.
* ICE restart: unchanged, exactly as the source read predicted (the
  options argument is unused through 1.28). `strings libgstwebrtc.so |
  grep -i restart` still finds nothing.

Decision taken from this (docs/23, ADR-0022): remove a track with the
valve closed first and the transceiver set to `inactive`; every webrtcbin
answerer Fjarr ships (`fjarr-opsim`, loop tests) sets
`reuse-source-pads=TRUE` and needs GStreamer ≥ 1.26; the Chromium-answerer
check happens in slice 3a.

## API sequences that worked (for the architecture doc)

All signal handlers marshal to one `GMainLoop` (`g_idle_add_full`); promises are consumed with `gst_promise_new_with_change_func`, the reply is `gst_structure_copy`'d and the promise unref'd in the change func.

1. **Setup (offerer A)**: `webrtcbin bundle-policy=max-bundle` → `gst_element_set_state(PLAYING)` → `create-data-channel("control")` → request `sink_%u`, link caps-gated branch (`… ! rtpvp8pay ! valve ! webrtcbin`).
2. **Caps gate**: wait for the fixed-caps event on the payloader src pad, then `create-offer(NULL, promise)`.
3. **Offer/answer**: A `set-local-description(offer)` → B `set-remote-description(offer)` → flush B's queued remote candidates → B `create-answer` → B `set-local-description(answer)` → A `set-remote-description(answer)` → flush A's queued candidates. `on-ice-candidate(mline, cand)` → peer `add-ice-candidate(mline, cand)`; candidates arriving before the remote description are queued.
4. **Manifest**: read `a=mid` per m-section from the offer SDP; transceiver `mid` property only after `stable`. Map B's `src_%u` pads via the pad `transceiver` property.
5. **Add a track**: request + link a new sink pad → `on-negotiation-needed` → repeat 2–3. Other tracks keep flowing (gap ≤ 36 ms).
6. **Mute a track**: `valve drop=true` on that branch; do not renegotiate. **Remove a track**: valve closed first, then `direction=inactive` and re-offer — a webrtcbin answerer must have `reuse-source-pads=TRUE` (≥ 1.26), see the [1.28 re-run](#re-run-on-gstreamer-128).
7. **Bandwidth**: `get-stats` every second, diff `outbound-rtp.bytes-sent` per `ssrc`.
8. **ICE restart**: not available on any release through 1.28; recreate the session.

## Files

* `main.cpp` — the probe (~800 lines: RAII wrappers, marshaling, negotiation helper, Q1–Q6 steps, diagnostics).
* `CMakeLists.txt` — standalone build, `gstreamer-1.0 gstreamer-webrtc-1.0 gstreamer-sdp-1.0`, `GST_USE_UNSTABLE_API`.
* `results/` — full stdout of the four 1.24 configurations quoted above; `results/gst-1.28/` — the five slice-2.9 runs.
