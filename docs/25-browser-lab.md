---
title: Browser Lab
description: CDP-driven Chromium in the dev stack — end-to-end tests of the whole system and of the web library in a real browser, signaling traffic introspection, network emulation, and performance profiling (CPU, memory, web vitals), usable by developers and by AI agents alike.
---

> **Status: review** — specified alongside slice 3
> ([docs/23](23-agent-core-architecture.md)); **built in slice 3a**
> (2026-09-19: the `browser` service, `@fjarr/e2e`, the wire tap,
> `LoopbackAgent`, the frame stamp, `fjarr-lab`, the CI job — see
> [implementation notes](#implementation-notes-slice-3a)) so the slice-3
> gate and everything after it run on it. It is the browser-side half of
> the testing strategy ([docs/15](15-testing-strategy.md)) and of
> agent-first development ([docs/20](20-agentic-development.md)).

## Why

Slice 2's review found ten defects in code that was fully unit-tested
against a mock, because the mock is not a browser
([slice-2 review](reviews/slice-2-review.md)). The browser lab makes the
real thing as easy to drive as the mock: a headless Chromium with the
Chrome DevTools Protocol (CDP) exposed, in the compose stack, with a
harness that an engineer *or an AI agent* uses to run the system end to
end, watch the wire, degrade the network, and measure CPU, memory and
responsiveness — every run leaving artifacts that are readable without a
screen.

## What CDP can and cannot do here

CDP is the browser's own instrumentation, so it reaches things Playwright's
public API does not, and it has one hard limit that shapes this design:

| Need | Mechanism | Note |
|---|---|---|
| Drive pages, assert DOM, screenshots, video, trace viewer | Playwright (which speaks CDP underneath) | the harness; raw CDP sessions where needed (`context.newCDPSession(page)`) |
| Signaling traffic | `Network.webSocketFrameSent/Received` | every docs/08 signaling message with timing, decoded by the harness |
| DataChannel traffic | **not visible to CDP** (SCTP inside DTLS) | `@fjarr/core` exposes an **opt-in wire tap** (`createFjarrClient({ wireTap: true })`, then `client.on("wire", …)`; shape in [docs/21](21-web-client-architecture.md#wire-tap)) — never on by default, since envelopes carry keystrokes and clipboard text |
| WebRTC internals (ICE pairs, RTP stats, codecs) | `RTCPeerConnection.getStats()` through the library's stats store (`session.stats`) | `chrome://webrtc-internals` is not scriptable; the same data is |
| Network conditions on HTTP/WebSocket | `Network.emulateNetworkConditions` (latency, throughput, offline) | **does not touch WebRTC media or DataChannels** (UDP bypasses the browser's network stack emulation); `offline` blocks *new* connections — an established WebSocket stays up (verified on Chrome 153), so a ladder test enters rung 3 by other means (below) |
| Network conditions on the media path | `tc netem` in the `demo-robot` container (loss, delay, jitter, rate) — docs/15; `RobotContainer.netem(profile[, iface])` keeps the introspection port out of the impairment, `netemToward(profile, ip)` impairs the robot's egress toward one viewer only (slice 6a) | applied robot-side; `NET_ADMIN` in the demo profile; profiles below |
| CPU profile | `Profiler.start/stop` (`.cpuprofile`) and `Tracing` with `devtools.timeline` + `v8.cpu_profiler` categories | opens in DevTools / Perfetto |
| Memory | `HeapProfiler.collectGarbage` + `Performance.getMetrics` (`JSHeapUsedSize`, `Nodes`, `JSEventListeners`) per cycle; `HeapProfiler.takeHeapSnapshot` on demand | the leak oracle for connect/disconnect and mount/unmount soaks |
| Responsiveness | `PerformanceObserver` in-page (long tasks, event timing → INP, LCP, layout shift — no library); `Tracing` for frame drops | budgets in docs/16 |
| Media timing | `requestVideoFrameCallback` + the machine-readable frame stamp (below) | glass-to-glass and time-to-first-frame |
| Media devices without hardware | Chromium flags `--use-fake-device-for-media-stream --use-fake-ui-for-media-stream` | push-to-talk and autoplay tests |

## Topology

```text
docker compose --profile demo --profile lab up
  fjarr-server ── demo-backend ── demo-dashboard (Vite)
       │
  demo-robot (embeds libfjarr + fjarr::TestCapability) ← tc netem here for media-path faults
       │  WebRTC (host candidates on the compose network; TURN via coturn with --profile turn)
  browser  ── headless Chromium, CDP on :9222, fake media devices, no sandbox
       ▲
  dev container: the harness (Playwright + CDP helpers) and the `fjarr-lab` CLI
```

The `browser` service is the Playwright image pinned to the harness's
`@playwright/test` version (`mcr.microsoft.com/playwright:v<x.y.z>-noble`)
started by `docker/lab/run-chromium.sh`: headless Chromium with its
DevTools port exposed on the compose network, fake media devices, extra
flags through `LAB_CHROMIUM_FLAGS` only where a test needs them. The
harness connects with `chromium.connectOverCDP(…)` from the `dev`
container, so the same browser serves scripted tests and ad-hoc sessions,
and its process is observable from the host at
`http://127.0.0.1:9222/json` (the mechanics: [implementation notes](#implementation-notes-slice-3a)).

## The harness

`web/e2e/` (a workspace package, `@fjarr/e2e`, never published): Playwright
Test with fixtures:

- `stack` — checks `fjarr-server /healthz` and the robot container
  (skips the test when they are down, fails in CI), exposes
  `robot.introspect()` ([docs/24](24-pipeline-introspection.md)),
  `robot.netem(profile)`, `robot.freeze()/thaw()/restart()` and
  `restartServer()`.
- `dashboard` — a page on the demo dashboard with the grant flow done;
  `goto({ role })` picks the demo's operator/developer role,
  `connect(robotId)`, `waitForState("connected")`, `tracks()`,
  `diagnostics()` (the Diagnostics tab's state and its graph's SVG node
  count).
- fixtures beside the harness: the `rtsp-sim` compose service
  (`docker/lab/rtsp-sim.py`, GStreamer's RTSP server serving a moving test
  pattern as H.264) stands in for a network camera, so `fjarr.camera`'s
  `rtsp` source type is exercised in CI without hardware
  (`tests/stack/camera.spec.ts`). From slice 6b it also serves
  `/pattern-low` (640×360 at 5 fps), the substream a passthrough track
  uses as its thumbnail tier.
- `tests/stack/introspect.spec.ts` (slice 5a): `fjarr.introspect` through
  the session pipeline feed on the lab page — bodies as blob references
  over the bulk channel, the valve following `select-tracks`, the hot-plug
  branch appearing, history scrubbing; an operator grant answered
  `capability-denied`; the demo dashboard's Diagnostics tab rendering the
  session graph with d3-graphviz for the developer role only.
- `tests/stack/ratecontrol.spec.ts` (slice 6a): the offer's feedback lines
  and Chromium's answer, TWCC feedback in the agent's stats; a lone viewer
  under `bad` (estimate and encoder reaction, decoding at the thumbnail
  tier, promotion and recovery once cleared); `lossy` repaired by
  retransmission with rare keyframe requests; three viewers with one
  behind `bad` toward it alone (the simulator's `congested-viewer` in
  `dev`), the clean two untouched. Measured through the introspection
  port, which the impairment exempts.
- `tests/stack/passthrough.spec.ts` (slice 6b): the demo's RTSP track as
  the camera's own H.264 — the producer's pipeline holds no encoder and no
  decoder, `/stats` reports `passthrough` with no encoder target, no
  keyframe request reaches the camera, and a tier switch hands the browser
  the 640×360 substream and back without a renegotiation.
- `tests/stack/viewer.spec.ts` (slice 5b): the viewer the endpoint serves
  — the page without a token, the API refused without and served with it,
  traversal refused; in the lab browser the token entered once, the
  connected robot's session graph rendered (SVG nodes counted) and its
  summary tab. CI runs it a second time against the demo robot from the
  `fjarr-agent` image.
- `cdp` — a CDP session for the page with helpers: `network.emulate(profile)`,
  `signaling.capture()` (WebSocket frames → docs/08 messages),
  `wire.capture()` (the library's DataChannel tap), `profile.cpu(ms)`,
  `profile.trace(ms, categories)`, `memory.sample()`, `memory.snapshot()`,
  `vitals()`.
- `loopback` — the in-browser loopback agent (below) for tests that need
  a real peer connection but no robot.

Every test writes to `web/e2e/out/<test>/`: the Playwright trace, HAR,
signaling and wire captures as JSON lines, profiles, metrics, screenshots,
and a `summary.json` + `summary.txt` — the text form is what an AI agent
reads first, the JSON is what CI compares against budgets.

**Network profiles** (the browser half through `cdp.network.emulate`, the
media half through `robot.netem`; `fjarr-lab net` applies both):

| Profile | Browser (CDP: latency / down / up) | Media path (netem on demo-robot) |
|---|---|---|
| `lan` | 0 ms / unlimited | none |
| `wifi-ok` | 20 ms / 50 Mbps / 20 Mbps | delay 10 ms ± 3 ms |
| `4g` | 80 ms / 10 Mbps / 3 Mbps | delay 40 ms ± 10 ms, rate 8 Mbit |
| `lossy` | 50 ms / 5 Mbps / 2 Mbps | loss 5 %, delay 30 ms ± 15 ms |
| `bad` | 200 ms / 1 Mbps / 500 kbps | loss 15 %, delay 100 ms ± 40 ms, rate 1.5 Mbit |
| `offline` | offline | drop all |
| `relay-only` | `lan` | `iceTransportPolicy: "relay"` in the client; coturn required |

The docs/15 fault menu maps onto these plus the agent's own switches
(`fjarr-opsim` and the `fjarr.test` hooks: hot-plug, go-silent, SIGSTOP).

## The in-browser loopback agent

`@fjarr/core/testing/browser` adds `LoopbackAgent`: the mock agent's
signaling behaviour, but the media side is a **real second
`RTCPeerConnection` in the same page**, offering a `canvas.captureStream()`
track with the frame stamp drawn on it, real DataChannels with the docs/08
labels and reliability, real ICE (host candidates) and DTLS. Core and React
tests run against it in the real browser: autoplay policy, `srcObject`,
IntersectionObserver, `jitterBufferTarget`, `getStats` keying by `mid`,
`replaceTrack` for push-to-talk, renegotiation and ICE restart with a real
`webrtc` stack. It is the missing rung between the unit mock and the C++
agent, and it is what makes the web packages testable in a browser today,
before slice 3 lands. CDP throttling does not reach it (loopback UDP), so
media-condition tests still need the real robot path.

## Against the real agent

The loopback agent is a rung, not the destination. The lab's main job is
the **whole system**: the web packages talking to the C++ agent through
`fjarr-server`, in a real browser, with the media path degraded on the
robot side and the browser instrumented on the operator side. That is the
only place where the two implementations of docs/08 meet under real
timing, and it is where slice 2's mock-versus-browser gap actually closes:

- every docs/08 rule has a two-sided test here (the agent's renegotiation
  queue against the client's renegotiation-safe registry, the agent's
  `ice-restart` re-offer against the client's ladder, `select-tracks`
  against the valve, the heartbeat both ways), with the signaling capture,
  the wire tap and the robot's introspection endpoint giving the
  agent-side and client-side view of the same second;
- the React components are exercised as the customer uses them — a
  `<VideoGrid>` on a real decoded stream, `<ConnectionQuality>` on real
  `getStats`, push-to-talk into a real uplink transceiver — with CPU,
  memory and INP measured while it happens;
- a change on either side (a new source type, a new capability, a core
  fix) is verified against the other side without a human at a screen,
  which is what lets an AI agent implement and validate a slice end to
  end.

## Frame stamp (the latency harness's oracle)

`fjarr.test`'s tracks (and later a `stamp = true` option on any track)
carry a **machine-readable stamp** painted by the core into the raw frame
before encoding (a luma-only pad probe on `video/x-raw`; no extra
element or dependency): a horizontal strip of **96 blocks** across the
top-left, each block `max(4, width/128)` px wide and 16 px tall, black or
white, encoding 96 bits MSB first: 8-bit sync `0xA5`, 32-bit frame
counter, 48-bit sender timestamp (unix milliseconds, agent clock; the
reader applies the `fjarr.core/time-sync` offset), 8-bit XOR checksum of
the preceding 11 bytes. At 1280 px wide that is a 960×16 strip; at 640 px
a 480×16 strip. The reader samples each block's centre with one canvas
`drawImage` and `getImageData` call per `requestVideoFrameCallback`,
thresholds at mid-grey, verifies sync and checksum, and discards frames
that fail either — no OCR, robust to scaling and compression. From it: glass-to-glass per frame, time to first
frame after enable, frame gaps during renegotiation (the docs/16 hot-plug
"zero dropped frames" criterion becomes a counter, not a claim), and
frozen-frame detection independent of `getStats`.

## Performance profiling and budgets

`lab.profile(scenario)` runs a scripted scenario (connect, stream two
tracks for 60 s, toggle tracks, hot-plug, disconnect; or a
connect/disconnect soak of N cycles; or a mount/unmount soak of a grid)
while collecting a CPU profile, a trace, memory samples per cycle and
vitals, and writes a comparison against the web-side budgets in
[docs/16](16-performance-budgets.md#web-client-budgets): main-thread
busy %, long tasks, INP, heap growth per cycle, listener growth, dropped
frames. Regressions fail CI once the numbers are confirmed (docs/16
"initial targets" rule); until then they trend in the nightly job.

## `fjarr-lab`: the agent-first CLI

Scripted tests are half of it; the other half is being able to *look*.
`pnpm fjarr-lab <command>` (in `dev`, against the running `browser`
service) gives a shell-driven view of the browser that an engineer or an
AI coding agent uses without writing a test:

| Command | Does |
|---|---|
| `open <url>` / `pages` / `close` | manage tabs in the lab browser |
| `eval <js>` | evaluate in the page (e.g. `client.sessions.get("demo-robot-01").getState()` — the demo exposes `window.__fjarr` in dev) |
| `screenshot [path]` | PNG, printed path |
| `net <profile>` | apply a network profile (both halves) |
| `signaling [--follow]` | decoded WebSocket frames (docs/08 messages) |
| `wire [--follow] [--cap fjarr.camera]` | DataChannel envelopes via the library tap |
| `stats [robot]` | the session's `getStats` summary and health |
| `profile cpu <s>` / `profile trace <s>` | write a `.cpuprofile` / trace and print the top self-time functions |
| `memory [--cycles N]` | heap/nodes/listeners now, or after N connect/disconnect cycles, with the delta |
| `vitals` | LCP, CLS, INP, long tasks since navigation |
| `introspect [pipelines\|<id>[.txt\|.json\|.dot]\|stats\|memory\|log\|events]` | the robot's introspection endpoint ([docs/24](24-pipeline-introspection.md)), through `docker compose exec` into the robot container or `E2E_INTROSPECT_HTTP` (from slice 5a on the core package's HTTP [pipeline feed](21-web-client-architecture.md#pipeline-feeds), `E2E_INTROSPECT_TOKEN` as the header): `pipelines` (default) lists them with each summary; `<id>` prints the summary (`.json`/`.dot` the raw body); `memory --checkpoint` / `--since cp-n` for the soak oracle; `log [--minutes n]`; `events [--body txt]` streams one line per snapshot until Ctrl-C |
| `report` | writes `out/adhoc/summary.{json,txt}` from everything captured so far |

Every command prints a one-screen text result and writes JSON next to it,
so a transcript of a debugging session is also its evidence.

## How this is used

- **`/verify`** runs the e2e smoke (connect, first frame, toggle, close)
  when the demo profile is up; CI runs the full suite on the compose stack
  (software encoding) and the profiling scenarios nightly.
- **Slice gates** cite lab artifacts: the slice-3 gate's "web client's
  ladder against the real agent" is a lab test with the `bad` and
  `offline` profiles and the agent's fault switches; the docs/06 camera
  and desktop acceptance criteria become lab scenarios.
- **Debugging**: `make lab-up`, then `fjarr-lab open …`, `net lossy`,
  `wire --follow`, `introspect session:…` — the same four commands whether
  a human or an agent is at the keyboard.
- **Third parties**: the lab is part of the repo, so a customer extending
  the product (a new source, a capability view) gets the same rig.

## Implementation notes (slice 3a) {#implementation-notes-slice-3a}

What was built, and the facts about real browsers that shaped it (each
verified in the lab, 2026-09-19):

**The `browser` service** (`docker-compose.yml`, profile `lab`) is the
Playwright image pinned to `@fjarr/e2e`'s `@playwright/test` version,
started by `docker/lab/run-chromium.sh`: full Chromium in `--headless=new`,
fake media devices, `--remote-allow-origins=*`. Chrome binds its DevTools
listener to loopback regardless of `--remote-debugging-address` (Chrome
153, both the full binary and the headless shell), so the script runs a
tiny Node TCP forwarder `0.0.0.0:9222 → 127.0.0.1:9223`; the healthcheck
probes the container's own address, not loopback. Chrome also refuses
DevTools HTTP requests whose `Host` header is a name, so the harness
resolves `browser` to its IP before `connectOverCDP` (from the host use
`http://127.0.0.1:9222/json`). Plain-http origins on the compose network
are not secure contexts (no `getUserMedia`): the script passes
`--unsafely-treat-insecure-origin-as-secure` for the lab page and the demo
dashboard (`LAB_INSECURE_ORIGINS`). The lab page is served by Vite from
`web/e2e/app` on `:5174` and reached as `http://lab-host:5174` — a network
alias of `dev`, because Chrome's HSTS preload list forces https on the
name `dev`; Vite's `allowedHosts` accepts any host for this page.

**`@fjarr/e2e`** (`web/e2e/`): Playwright projects `loopback` (no server),
`stack` (needs `fjarr-server`; the demo profile for the dashboard smoke)
and `spike` (the Chromium-answerer probe). `browser` is a worker fixture
that connects over CDP; every test gets `out`, `cdp`, `loopback`, `stack`,
`dashboard`. Environment (defaults are the compose network as seen from
`dev`; CI overrides the page origin because the runner serves the page):

| Variable | Default | Meaning |
|---|---|---|
| `E2E_BROWSER` | `http://browser:9222` | CDP endpoint (resolved to an IP) |
| `E2E_PAGE_ORIGIN` | `http://lab-host:5174` | what the *browser* navigates to for the lab page |
| `E2E_SERVER_WS` / `E2E_SERVER_HTTP` | `ws://fjarr-server:8080/ws` / `http://fjarr-server:8080` | fjarr-server as the browser / the harness reach it |
| `E2E_DASHBOARD_URL` | `http://demo-dashboard:5173` | the demo dashboard (dev mode exposes `window.__fjarr` and takes `?fjarr_backend=&fjarr_server=` overrides) |
| `E2E_ROBOT_SERVICE` | `demo-robot` | compose service carrying the media path (`tc` runs in it as root) |
| `E2E_DASHBOARD_HTTP` | `E2E_DASHBOARD_URL` | the dashboard as the *harness* probes it, when that differs (CI) |
| `E2E_OUT` | `web/e2e/out/` | artifact root; each test's directory is recreated per run |
| `FJARR_GRANT_HS256_SECRET`, `FJARR_DEV_DEVICE_TOKEN` | the `.env` dev values | the harness mints grants and registers loopback robots with these |
| `E2E_INTROSPECT_HTTP` | `http://demo-robot:7381` (CI: the runner's `localhost:7381`) | the endpoint as the harness reaches it; empty = `curl` inside the robot container |
| `E2E_INTROSPECT_TOKEN` | `FJARR_INTROSPECT_TOKEN`, else the compose default | sent as `Authorization: Bearer` to the endpoint (the demo exposes it with a token, docs/24) |
| `E2E_VIEWER_URL` | `http://demo-robot:7381/` | the served viewer as the *browser* reaches it |

**The lab page** (`web/e2e/app`) exposes `window.__lab` (contract:
`web/e2e/src/lab-page.d.ts`): `setup({mode, robotId, …})`,
`open/close/state/info/waitForState`, `mount("tile"|"grid"|"ptt")`,
`tracks()`, `videos()`, `stats()`, `stamps.watch/summary`, `wire.drain()`,
`agent.*` (the loopback agent's faults and stimuli), `ptt.*`, `vitals()`,
`request()`, `blob(cap, ref)` (a blob reference resolved over the bulk
channel) and `feed.*` (a session pipeline feed under test control:
`start/stop/status/pipelines/snapshot/body/history`).
It creates its client with `wireTap: true` and pushes every wire event to
the harness. `LoopbackAgent` runs in two modes: **in-page** (a fake socket
pair, no server) and **server** (it registers with fjarr-server as a
robot with the dev-token scheme; the client connects with a grant the
harness mints), so the client's rungs run over real sockets.

**Entering the ladder.** Because CDP `offline` leaves an established
socket alone, the `stack` suite enters rung 3 with the agent's
`session-close{retry:true}` while offline (every round after that is a
real, failing connection attempt, counted and backed off), and covers the
docs/15 "signaling socket killed" row by restarting `fjarr-server`
(`docker compose restart`, through the docker CLI the `dev` image now
carries — docker-outside-of-docker, socket mounted, group `DOCKER_GID`).
The same door gives `RobotContainer` its `netem`, `pause`/`unpause`
(SIGSTOP), `restart` and loopback-only `introspect` (a `curl` inside the
robot container).

**`fjarr-lab`** (`pnpm fjarr-lab …` from the repo root, runs on Node's
built-in TypeScript transform) implements `open/pages/close/eval/
screenshot/net/netem/signaling/wire/stats/profile cpu|trace/memory/
vitals/introspect/report`. CDP emulation lives with the DevTools session,
so `net <profile>` holds the browser half while it runs (`--hold <s>` or
Ctrl-C); the netem half persists. Vitals come from in-page
`PerformanceObserver`s (LCP, CLS, worst interaction as the INP
approximation, long tasks) — no dependency.

**What the lab found in its first week** (all fixed in slice 3a, each
with a unit test): the core answered the agent's `recvonly` audio uplink
with `recvonly`, so push-to-talk moved no media in a real browser
([docs/21](21-web-client-architecture.md#audio-uplink-negotiation));
`fjarr-server` turned `FJARR_TURN_URLS=""` into TURN credentials with an
empty URL, which makes `new RTCPeerConnection` throw; the client now drops
such entries with a warning instead of dying; `usePushToTalk` reported a
`TypeError` outside secure contexts; and `robot-offline` during a
reconnect round is now a counted round rather than a fatal error
([docs/21](21-web-client-architecture.md#state-machine)).

## Slice mapping

1. **Slice 3a** (web-side, before the C++ core — [docs/23](23-agent-core-architecture.md#slices-3a-3b-3c-and-their-gates)):
   `browser` service, `@fjarr/e2e` harness with the `cdp` fixture, the
   wire tap in `@fjarr/core`, `LoopbackAgent`, the frame-stamp reader,
   `fjarr-lab` with `open/eval/screenshot/net/signaling/wire/stats/memory/vitals`,
   docs/16 web budgets, component e2e for `<VideoTile>`/`<VideoGrid>`/
   push-to-talk against the loopback agent, `make lab-up` / `make e2e`.
2. **Slice 3b gate**: the ladder and hot-plug scenarios against the real
   agent with the `fjarr.test` stamp; **3c**: the `introspect` command.
3. **Slice 5**: demo scenarios, profiling scenarios in nightly, budgets
   enforced.
4. **Slice 7a**: the robot-lifecycle fault rows
   ([docs/15](15-testing-strategy.md#fault-injection)) against a real
   browser. **Slice 7b**: the glass-to-glass harness on the frame stamp —
   `coturn` joins the `lab` profile so the relay column exists, p50/p95
   under the clean, lossy and relay profiles, and the tracked CSV.
   Input-to-photon waits for a robot-side input path and lands with
   `fjarr.desktop` in M3.
