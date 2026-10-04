---
title: Testing Strategy
description: Test pyramid, fault injection, the latency harness, and safety behaviors.
---

Connectivity software fails in the field through *network weather, process
death, and time* — so those are first-class test inputs here, not
afterthoughts.

## Pyramid

| Layer | Tools | What it proves |
|---|---|---|
| Unit | GoogleTest (C++), `cargo test`, Vitest | pure logic: envelope codecs, backoff math, range/resume bookkeeping, state machines. The translation-boundary style ([prior art](11-prior-art.md#fleet-daemon)) keeps this layer big and cheap |
| Component | same + fakes | capability against a fake core; DesktopBackend against Xvfb; signaling router against an in-process client |
| Integration | docker compose, headless Chromium (Playwright) | agent ↔ fjarr-server ↔ browser, real WebRTC, against robot-sim |
| End-to-end / demo | the `demo` profile | the three-demo stack **is** the e2e suite — demos consuming only public APIs means passing demos prove the customer path |
| Soak | nightly (M1+) | 24 h session churn + transfer loops; memory/fd leak watch |

Protocol conformance: golden envelope fixtures generated from
`protocol/schemas/` are replayed against all three implementations — drift
fails the build ([docs/08](08-protocol.md#versioning)).

## Fault injection {#fault-injection}

A **fault-injecting mock backend/peer is a first-class artifact** (the fleet-daemon
mock's best idea), scripted in integration tests. The web side ships it as
`@fjarr/core/testing` (`MockAgent`: fake signaling socket + fake peer
connection, every fault below as a method) so host dashboards test their
own integration without a browser or a robot; the C++ side ships
`fjarr-opsim` (docs/23) and the [browser lab](25-browser-lab.md) applies
the network rows below as named profiles on both the browser and the media
path. Minimum fault menu:

Rows are implemented as the capability that makes them meaningful arrives;
the four robot-lifecycle rows land in **slice 7a**, the mid-transfer kill
with `fjarr.files` in M4, and monitor hot-plug with `fjarr.desktop` in M3.

| Fault | Expected behavior |
|---|---|
| Signaling socket killed | reconnect with the spec backoff; ICE restart; session resumes or closes cleanly |
| **Server goes silent without disconnecting** | heartbeat detects within budget; teardown + reconnect (the nastiest real-world case) |
| Packet loss 5–20 % on media path (netem) | video degrades, never stalls > budget; input stays responsive |
| Relay-only forced (`iceTransportPolicy: relay`) | everything works through coturn; budgets per docs/16 relay column |
| Agent SIGKILL mid-session | operator sees `peer-gone` ≤ heartbeat budget; supervised restart; robot reachable again < 30 s |
| Pipeline error (encoder reset, capture death — injected via the `fjarr.test` hooks or `GST_DEBUG` fault points) | producer restart with backoff, then a media-plane rebuild: sessions close with `media-restart` (`retry:true`), the signaling socket stays up, the robot stays online ([ADR-0019](adr/0019-agent-process-model.md)) |
| Whole-process hang (SIGSTOP) | the systemd watchdog kills and restarts the process (no in-process defense against a stopped process); operators see `peer-gone`; ownership lease expires (fail-open); robot back < 30 s |
| Mid-transfer network kill | file resume from received ranges; hash verifies |
| Monitor hot-plug during a session (`xrandr --setmonitor`/`--delmonitor` on robot-sim) | renegotiation adds/removes the track; other monitors' frame counters never stall; re-plug restores the same `track_id`; zero monitors then one recovers without reconnect |
| Grant expired / clock skew | clean `grant-expired`, no retry storm (fatal vs retryable taxonomy) |

## Latency harness (glass-to-glass) {#latency-harness}

From M1, a measurement rig — not vibes:

- **Glass-to-glass**: the agent draws a **machine-readable frame stamp**
  (frame counter + sender timestamp as a block pattern — no OCR) on the
  test pattern / a stamped camera track; the [browser lab](25-browser-lab.md)
  reads it per decoded frame via `requestVideoFrameCallback`; Δ = g2g.
  Report p50/p95 under clean, lossy, and relay conditions (the lab's
  network profiles).
- **Input-to-photon**: synthetic click → screen change at a known pixel →
  time to that change appearing in the received stream.
- **Two gates, because the budgets are NUC-class and CI is not.** Shared
  runners with a software encoder cannot hold a p95 honestly, and a gate
  people learn to re-run is worse than no gate. So the CI job fails only
  above a **loose ceiling** — gross regression, not budget — while the
  **nightly job on the prepared runner** gates the
  [docs/16](16-performance-budgets.md) numbers themselves.
- Results append to `web/e2e/latency.csv` — but **only a labelled run
  records** (`make latency LABEL=<hardware>`, or the nightly job with the
  label variable set). The label is mandatory because rows from different
  machines must never be compared. Nothing commits a row automatically: CI
  and the nightly upload theirs as a run artifact, since a job that pushes to
  the repository every night is noise, not history.
- **Every row carries its own trust marker.** A machine that cannot encode
  the source rate is measuring its own CPU, not the path, so the harness
  records the decoded `fps` beside the percentiles and flags a run below 60 %
  of the source rate as CPU-limited. Strict mode *refuses* such a run rather
  than holding it to a budget it was never measuring.
- Input-to-photon needs a robot-side input path and therefore arrives with
  `fjarr.desktop` in M3; glass-to-glass lands in slice 7b
  ([docs/23](23-agent-core-architecture.md#slices-3a-3b-3c-and-their-gates)).
  Together they gate the ADR-0006 decision.

## Safety behaviors {#safety-behaviors}

Learned the hard way (the teleop car's silently-regressed deadman): every safety
behavior is **spec'd, implemented, and covered by a test that fails when it
regresses**:

- input silence deadman on actuation-bearing channels;
- `release_all_input()` on any session end (no stuck modifiers — test:
  disconnect mid-keydown, assert keyup injected);
- control-domain claims ([docs/10](10-security.md#session-ownership)) fail
  open after 30 s without the holder's heartbeat, and end at once with the
  holder's last session; a `motion` takeover, a release or a stale claim runs
  the old holder's `release_all_input` before the new holder's first command
  (`test_control_domains.cpp` pins the rules in virtual time,
  `test_control.cpp` through the real SessionManager and router);
- heartbeat teardown timings;
- **tunnel isolation** ([docs/27](27-network-tunnel.md#testing)): with two
  robots attached at once, no packet crosses from one link to the other in
  either direction, and each end drops anything not addressed to its own
  tunnel address. Isolation is the property customers will ask about, so it
  is a test that fails loudly, not a configuration note. The single-ended
  half of it ships with the agent (slice 4.5a): `test_net.cpp` pins both
  rules in both directions, and the `tunnel` opsim scenario proves the robot
  refuses a packet aimed into its LAN over a real data channel. The
  two-robot half needs two operator ends and lands with `fjarr-connect`.
- **discovery and login** (slice 4.5f, `make tunnel-login`, in the e2e job):
  `fjarr-connect login --code` through the demo dashboard in the lab browser,
  then `list`, `--ssh-config` against the agent's own addresses, a connect by a
  word from the label with a grant from the operator API, `ssh` over the link, and
  the session in the audit log — `session.started` naming `fjarr.net`, matched by
  `session_id` to its `session.ended`. The **loopback** shape is tested in halves:
  its listener in Rust over real TCP (preflight, a wrong `state`, an empty
  credential), the `FjarrCliLogin` component in `@fjarr/react`. The seam between
  them — a browser's `fetch` reaching `127.0.0.1` on the machine running the CLI
  — is not one this lab can cross, since its browser is another container, and
  that is the one part of 4.5f no test runs end to end.
- **tunnel interface ordering**: a DDS participant created while the agent is
  detached must not advertise the tunnel address; created while attached it
  must; and it must keep advertising across an agent restart. These pin the
  three measured facts the tunnel lifecycle rests on — the ordering requirement
  in [docs/27](27-network-tunnel.md#lifecycle), and therefore the installer's
  job and the systemd unit's ordering, is built on nothing else. Implemented in
  slice 4.5d as `make tunnel-ros-ordering`, against real ROS 2 participants on
  both ends of a real link, and run **nightly** after `make tunnel-ros` as its
  positive control: it builds the ROS lab image, restarts the agent and recreates
  participants, which is minutes of work for a property that changes rarely. The
  agent runs under a supervisor in the lab so it
  can be restarted the way systemd restarts it, leaving the interface in place;
  restarting the container instead would take the device with it and the third
  fact would be untestable.

## The robot's log is a gate {#log-gate}

`make agent-log-gate` fails a run whose robot logged a GLib or GStreamer
**CRITICAL**, or a failed assertion. Those mean a refcount or an invariant was
already violated, and a suite can pass straight through one: a double-released
fd watch printed three criticals per lab job for weeks while every test stayed
green, and was found by reading a log for an unrelated failure. Plain warnings
are deliberately not fatal — GStreamer emits benign ones — so the gate stays
worth obeying.

`make agent-log-gate-selftest` feeds it that same historical line and fails if
the gate stays quiet, because a gate that cannot fire is no gate. The same slice
found `opsim-all`'s crash retry had never once fired for the same reason
([docs/17](17-roadmap.md)), so every gate that tolerates something now has to
show it can still refuse.

## Memory safety (C++) {#memory-safety-c}

The agent wraps a C object system, so lifetime bugs get their own ladder
([docs/23](23-agent-core-architecture.md#memory-and-lifetime-discipline-and-the-tooling-that-enforces-it)):

| Layer | Tool | When | Gate? |
|---|---|---|---|
| RAII kit only touches refcounts | grep gate in `/verify`, clang-tidy `-Werror` | every commit | yes |
| Address/Undefined/Leak | `asan` preset (`-fsanitize=address,undefined`, UB non-recoverable, LSan with `lsan.supp`) on unit + loop tests | every commit (CI) | yes |
| Data races in the threading model | `tsan` preset (+ `tsan.supp` for the uninstrumented GLib/GStreamer modules; the RAII kit's hand-offs carry acquire/release pairs so TSan sees them) on unit + loop tests | every commit (CI, in the runner-level e2e job: the container job cannot set the host sysctl, docs/12) | yes |
| Live GStreamer objects per test / scenario | `leaks` tracer checkpoints bracketing every test case and every `fjarr-opsim` scenario | every commit (CI) | 3c (docs/23 slices) |
| Uninitialised reads MSan would need every library rebuilt for | valgrind memcheck (below); MemorySanitizer is deliberately not used — GLib, GStreamer, libsoup and libstdc++ would all need instrumenting | nightly | — |
| Object census + RSS over a soak | `GET /memory` before/after N sessions (`fjarr-opsim soak --cycles N`) | 20 cycles every commit (the e2e job); 200 nightly and at the 3c gate | yes (docs/16 budget) |
| Uninitialised reads, invalid frees sanitizers miss | valgrind memcheck on loop tests | nightly | trend → gate at M3 |
| Allocations on the hot path | heaptrack on a streaming scenario | nightly | docs/16 budget (one buffer header per subscriber per frame, nothing else) |

Two traps the instrumented runs have hit, both found by the nightly hanging
rather than failing:

- **heaptrack deadlocks on `dlclose` under concurrent allocation.** `libsrtp`'s
  first `srtp_init` loads and unloads NSS; `dlclose` holds glibc's
  dynamic-loader lock and frees into heaptrack, while another thread inside
  heaptrack's `malloc` hook holds heaptrack's lock and calls
  `dl_iterate_phdr` for the loader lock. So the heaptrack step profiles the
  streaming hot path only (`LoopMedia.*`) and **no test that completes DTLS
  runs under it** — those (`ConsumerNegotiation.*`, the `Session` DTLS test)
  are memcheck's. Found 2026-09-30 with gdb, [docs/12](12-development-environment.md#debugging-a-hung-process).
- **valgrind is ~30× slower, so a watchdog guards one call, never a loop.**
  A regression test for a hang (`fjarr::testing::Watchdog`,
  `agent/tests/watchdog.hpp`) wraps the single teardown call that used to
  hang; a budget over a whole churn loop tripped on slowness alone in the
  nightly memcheck. Waits in tests are polls with generous bounds, so a fast
  run is not slowed down.

Every tool prints a one-screen verdict and writes JSON, so an AI agent
running `make agent-test-asan` or `curl :7381/memory` gets an answer it
can act on, not a wall of output.

## Unattended-access test (the industrial gate)

Reboot the robot, wait, and assert that an operator session can start, see the
display and type into it **with zero local interaction**. Its phase-1 results
decided ADR-0006. From M3 it is scripted and kept forever on the mini-PC, the
desktop lab machine ([below](#the-desktop-test-lab)). It runs as **two scheduled
workflows**, because a job cannot outlive the reboot it causes, and a second job
in the same run could be handed to the runner in the seconds before the machine
goes down (decided 2026-10-01, on the mini-PC):

- **`lab-desktop-prepare`, 00:15.** A hosted job takes the `.deb`s of
  `main`'s newest green CI run — the files CI tested, not a rebuild
  ([docs/30](30-continuous-integration.md#artifacts-built-once)) — and builds
  `fjarr-server` and `fjarr-opsim` from the same commit. On the lab machine, `tools/fjarr-lab/unattended.sh prepare`
  undoes the previous `setup desktop`, installs the build, runs `setup desktop
  --ghost-screens 1`, and points the agent at a `fjarr-server` on the machine
  itself through a systemd drop-in. The person's own `/etc/fjarr/fjarr.toml` is
  never touched. Then `fjarr-lab reboot` reboots the machine once the runner is
  idle, which is after the job has finished.
- **The machine verifies itself after the reboot.** `prepare` also installs a
  one-shot unit, `fjarr-lab-unattended.service`, which runs `unattended.sh
  verify` once at the next boot and then disables itself. Verify checks that the
  machine really rebooted, then each step in order, so a failure names the
  first one that broke: GDM's automatic login (a session of the account on
  seat0), the session helper (the agent says it connected), the agent's backend
  (a capture ready), and the stream and input (`fjarr-opsim desktop-see` and
  `desktop-control` against the test window, run in the account's session). It
  writes its output and a verdict, tagged with the boot and the prepare run, to
  `/var/lib/fjarr-lab/unattended/`. It ends by removing the drop-in, so the
  agent returns to its own server.
- **`lab-desktop-verify`, 00:45, only reports.** It prints the machine's result
  and passes or fails on its verdict. GitHub's schedule is not an order. On the
  first night both workflows started about 3½ hours late, back to back, and
  verify ran before the reboot. So when verify finds the machine not yet
  rebooted, it dispatches itself again (at most 5 times) and ends as
  rescheduled, never as a failure or a pass.
  Proven by hand on the mini-PC on 2026-10-02: prepare, a reboot, the machine
  verifying itself at boot, and `report` giving the verdict "passed".
  **A verdict counts only for tonight's prepare.** Verify first asks GitHub
  for the night's `lab-desktop-prepare` run and fails when it did not
  succeed; `report --prepare-run <id>` then refuses a verdict written for any
  other run. On 2026-10-03 prepare failed before reaching the machine and
  verify reported the previous night's "passed" as that night's.

`fjarr-opsim` runs on the lab machine itself and decodes with `openh264dec`, which
the robot already has for its software encoder. libav (GPL) is never installed on
a machine under test. Its input-to-photon is not a number of record: the decoder
shares the robot's CPU with the encoder (162 ms on the mini-PC against 53 ms from
an operator on another machine, 2026-10-01). Input-to-photon is measured from
another machine.

First run by hand on the mini-PC, 2026-10-01: prepare installed the build and put
the ghost on DP-2, because the HDMI port had a monitor on it. `fjarr-lab reboot`
rebooted once the job had ended. Verify then passed the automatic login, the
helper, the capture and `desktop-see`. Its one failure was in the oracle: GNOME
Shell's Alt+Tab switcher grabs the keyboard, so the window never sees Alt's
release. The check now also accepts the window getting focus back, which the
switcher gives only once Alt is up. With that, `desktop-control` passed 12/12 on
GNOME Shell.

## The desktop test lab {#the-desktop-test-lab}

`fjarr.desktop` is tested in two places ([ADR-0033](adr/0033-desktop-test-lab-and-shared-runners.md)):

| Where | Runs | Covers |
|---|---|---|
| **Headless mutter** (slice 3.0; proven by its spike 2026-09-30): a container with `mutter --headless --virtual-monitor WxH`, its own D-Bus session, PipeWire and WirePlumber, the session helper, and a test window that records what it receives | `ci.yml`, every push, hosted runners | backend E's capture from a PipeWire stream, including the repeated frame on a static screen; input through EIS (the oracle checks keys, text and pointer positions); the session helper's handover and `SO_PEERCRED`, with a stand-in agent (`fixture-helper-check`); the robot's desktop end to end, helper → module E → agent → opsim (`make desktop-e2e`): decoded frames and a still screen (3.1); clicks, physical keys, text typed through the keymap, a combo, an untypable character refused by name, input-to-photon timed by the window turning green while F9 is held, and the key held at session end released (3.2). The window's log is served on `desktop-fixture:8090` for these oracles; monitor hot-plug (3.4): a service on `desktop-fixture:8091` plugs and unplugs mutter **virtual monitors** (`RecordVirtual` from a session of its own, consumed at 1280×720 so it has a size), which mutter reports like any monitor, and `fjarr-opsim desktop-hotplug` asserts the docs/06 criteria (the new track within 2 s, the first monitor's frames uninterrupted, the same `track_id` on re-plug, zero monitors survived); later clipboard, virtual-monitor hot-plug and PipeWire narrowing (the agent must fail to open anything but the granted stream) |
| **The mini-PC** (`fjarr-lab`, labels `desktop` `gnome`) | nightly, in its CI window | the unattended-access test above; ghost screens ([ADR-0032](adr/0032-ghost-screens.md)) beside a real monitor and the 3-monitor DisplayPort chain; hot-plug on real connectors; input-to-photon against the docs/16 budgets on real hardware |

Backend A (X11 kiosk, slice 3.7) uses robot-sim, the Xvfb service CI already
runs: a 3840×1080 root carved into RandR monitors (`xrandr --setmonitor`;
the root cannot grow, [docs/07](07-desktop-backends.md#simulating-hot-plug)),
a test window that logs what it receives, a service that plugs and unplugs
monitors, and a separate agent account (uid 10003) with the `xhost` grant,
Xvfb started with `-noreset` (it otherwise regenerates when its last client
leaves, wiping the grant) and the robot in the fixture's IPC namespace
(`ximagesrc` reads through MIT-SHM). The `fjarr-opsim` desktop scenarios
`desktop-see`, `-control`, `-hotplug` and `-cursor` run against it
(`make x11-e2e`) on every push; the browser side is the same for every
backend, so the browser tests stay on the mutter fixture. The clipboard
joins in slice 3.7c.

Every lab job starts from a baseline: `setup --undo desktop`, install the build
under test, then `setup desktop` with the job's own settings. The machine's
state after a run is documented, and a person's experiments before it do not
decide its result.

## What CI runs

How the workflows are laid out and kept fast — the job graph, which changes run
what, CI images, caches — is [docs/30](30-continuous-integration.md); this
section is what they test.

Today (M0.5–slice 2): lint, Rust and web unit tests, builds, docs gates.
From slice 3a/3b: lint (clang-tidy, clippy, eslint, markdownlint, lychee)
→ unit + component (C++ under ASan and TSan) → browser-lab e2e on the
compose stack ([docs/25](25-browser-lab.md): real Chromium over CDP,
software encoders; the real agent, `fjarr-opsim` incl. a 20-cycle soak;
VA-API asserted only on GPU runners when available) → docs build.
**Nightly** (`nightly.yml`, from slice 3c; also on demand): the 200-cycle
soak, valgrind memcheck on the loop tests, heaptrack on a streaming
scenario, the `netem-*` scenarios and latency trends, with lab artifacts
(traces, profiles, captures) attached to the run. It runs on the hosted
runner by default and on a **self-hosted GPU runner** when one is
registered (docs/12): the same job, with `media.encoder = auto` engaging
VA-API where `/dev/dri` exists, so the nightly measures the product path
as soon as such a machine is available.
