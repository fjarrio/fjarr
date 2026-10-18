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
  time to that change appearing in the received stream. A sample counts
  only from a **restored baseline**: before each press the decoded picture
  must be back where it started (`fjarr-opsim`: the centre's luma within 8),
  or that press is rejected by name. A capture frozen on the changed
  picture would otherwise "change" at once and time nothing: nine 0 ms
  samples passed on the mini-PC on 2026-10-17. The verdict needs 10 valid
  samples. There is no one-frame floor, because mutter captures on damage
  (CI's samples are about 17 ms).
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
- **updates** ([docs/32](32-fleet-ota.md), M8): a power cut mid-write boots
  the old slot; a release whose health check fails rolls back by itself;
  `take-control` is refused while the update holds `motion`, and the deadman
  stays in force; the not-ready-to-move state stays visible until the
  peripherals are verified; a `safety_relevant` component is refused without
  its policy and its on-site confirmation; a lower release is refused without
  a downgrade authorization; a router configuration that cuts the uplink
  reverts. The A/B legs are tested nightly on real hardware in the lab (the
  ZED Box Mini and the second x86 mini-PC), with power cut by a network-controlled
  socket, because no fixture reproduces a bootloader;
- **the speaker's gate** ([docs/08](08-protocol.md#talk), M3.5 3.5.3): an
  operator's audio is played only between `talk-start` and `talk-stop`, by
  one session at a time, and the gate closes on every session end, audited.
  `test_audio_capability.cpp` pins the floor, `busy`, the cap and the close
  on detach; opsim's `audio-talk` sends audio before `talk-start` and requires
  none of it played, the cap held and nothing after the release; the lab
  browser holds and releases the dashboard's Talk button;
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

- **`lab-desktop-prepare`, 22:15 UTC** (00:15 in Swedish summer time; in
  winter 23:15, and its job on the machine waits for the window to open at
  midnight). A hosted job takes the `.deb`s of
  `main`'s newest green CI run — the files CI tested, not a rebuild
  ([docs/30](30-continuous-integration.md#artifacts-built-once)) — and builds
  `fjarr-server` and `fjarr-opsim` from the same commit. On the lab machine, `tools/fjarr-lab/unattended.sh prepare`
  undoes the previous `setup desktop` (an undo that fails fails the job:
  [below](#the-desktop-test-lab)), installs the build, runs `setup desktop
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
- **`lab-desktop-verify`, 30 minutes later, only reports.** It prints the
  machine's result and passes or fails on its verdict. GitHub's schedule is not
  an order. On the first night both workflows started about 3½ hours late, back
  to back, and verify ran before the reboot. So while the machine has no
  verdict yet but is verifying, verify waits for one (up to 3 minutes; back
  from the reboot, the machine needs about one). When tonight's prepare has not
  even been created (GitHub fires the two crons with their own lateness),
  verify waits up to 8 minutes for it to appear, then queues behind it. When
  there is still no verdict, the machine has not rebooted yet, or tonight's
  prepare has not finished, it dispatches itself again (at most 5 times) and ends as
  rescheduled, never as a failure or a pass. GitHub shows a rescheduled run as a success, so each
  attempt's run name says which run dispatched it, and a night is judged by
  its last attempt.
  Proven by hand on the mini-PC on 2026-10-02: prepare, a reboot, the machine
  verifying itself at boot, and `report` giving the verdict "passed".
  **A verdict counts only for tonight's prepare.** Verify first asks GitHub
  for the newest `lab-desktop-prepare` run on `main` and fails when it did not
  succeed; `report --prepare-run <id>` then refuses a verdict written for any
  other run. On 2026-10-03 prepare failed before reaching the machine and
  verify reported the previous night's "passed" as that night's.
  **Tonight's means it ran tonight**: its job on the machine finished less
  than 12 hours ago, however long before that GitHub created the run. On
  2026-10-16 a prepare created 21.7 hours earlier had waited for the machine,
  which was off; it ran when the next window opened, and verify, 2 minutes
  later, refused it as "no prepare in the last 12 hours" while the machine
  was verifying that very prepare. A newest prepare that ended before
  tonight's 22:15 UTC slot, while that slot is less than 6 hours past, is not
  stale but not created yet (`tools/ci/tonights_prepare.py`).

Both workflows share one concurrency group, `lab-desktop`, **with `queue:
max`**: one lab run at a time, and the others wait their turn, first in,
first out. Verify waits in the group rather than beside prepare, whose first
job is hosted (3½ to 12 minutes): a verify on the idle runner meanwhile would find
prepare unfinished and spend its five attempts in a minute (it would have on
2026-10-02). Without `queue: max` a group keeps only one waiting run, and a
newer one cancels it: on 2026-10-16 the night's prepare cancelled the previous
night's waiting verify, and a verify that dispatched itself while a prepare
waited behind it would have cancelled that prepare. The cost is an outage's
backlog: while the machine is away, the run at the head of the group waits up
to 24 hours for it and two more arrive each night, so the queue grows by about
one run a day. When the machine returns the runs drain in order, and each
queued prepare installs the newest green build of its moment and reboots the
machine again; each old verify finds the newest prepare unfinished, dispatches
itself to the back of the queue and may spend its five attempts. After an
outage of more than a night, cancel the queued lab runs but the newest pair
(`gh run list -w lab-desktop-prepare.yml -s queued`), or let them drain.

GitHub fires these schedules hours late. Over the first 16 nights prepare
started 2 h 06 min to 4 h 46 min after 22:15 UTC, and verify 2 h 00 min to
4 h 25 min after 22:45 UTC. A prepare that starts in the window's last
quarter-hour reboots the machine after the window has closed, and that night
has no verdict. A job that no self-hosted runner takes is cancelled after
24 hours: the mini-PC was off from 2026-10-14 to 10-16, and the 10-15 prepare
ended that way.

**A red night reaches someone.** After the window, `nightly-alert`
([docs/30](30-continuous-integration.md#nightly-alert)) judges the night's
prepare, verify (its last attempt) and Nightly. A run that failed, was
cancelled, never came, or still waits for its runner at the check is red, and
so is a lab run that finished after its window. One issue labelled
`nightly-red` then stays open, with each red run's link and failing step,
until a night is all green. Before it, the verify had been red for a week and
nobody looked ([M3 gate review](reviews/m3-gate-review.md)).

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
| **Headless mutter** (slice 3.0; proven by its spike 2026-09-30): a container with `mutter --headless --virtual-monitor WxH`, its own D-Bus session, PipeWire and WirePlumber, the session helper, and a test window that records what it receives | `ci.yml`, every push, hosted runners | backend E's capture from a PipeWire stream, including the repeated frame on a static screen; input through EIS (the oracle checks keys, text and pointer positions); the session helper's handover and `SO_PEERCRED`, with a stand-in agent (`fixture-helper-check`); the robot's desktop end to end, helper → module E → agent → opsim (`make desktop-e2e`): decoded frames and a still screen (3.1); clicks, physical keys, text typed through the keymap, a combo, an untypable character refused by name, input-to-photon timed by the window turning green while F9 is held, and the key held at session end released (3.2). The window's log is served on `desktop-fixture:8090` for these oracles; monitor hot-plug (3.4): a service on `desktop-fixture:8091` plugs and unplugs mutter **virtual monitors** (`RecordVirtual` from a session of its own, consumed at 1280×720 so it has a size), which mutter reports like any monitor, and `fjarr-opsim desktop-hotplug` asserts the docs/06 criteria (the new track within 2 s, the first monitor's frames uninterrupted, the same `track_id` on re-plug, zero monitors survived); the desktop's sound (M3.5 3.5.2): a null sink is the fixture's speakers, and the same service plays a tone on the desktop (`/play`, with `pw-play`), switches the default output (`/speakers?use=`) and counts the capture streams open on it (`/listening`), so `fjarr-opsim desktop-audio` and the dashboard's Sound test hear the tone, keep hearing it across a change of output, and see the capture open only while the track is enabled; later clipboard, virtual-monitor hot-plug and PipeWire narrowing (the agent must fail to open anything but the granted stream) |
| **The mini-PC** (`fjarr-lab`, labels `desktop` `gnome`) | nightly, in its CI window | the unattended-access test above; ghost screens ([ADR-0032](adr/0032-ghost-screens.md)) beside a real monitor and the 3-monitor DisplayPort chain; hot-plug on real connectors; input-to-photon against the docs/16 budgets on real hardware. Not the mode change: `desktop-control` runs there without a plug service (`--desktop-plug`), so it reports that step as not run, and a mode change on backend E is covered nowhere end to end (backend A's is, by the X11 fixture's `/mode`). `make desktop-e2e` runs `desktop-control` without a plug too, so that report is checked on every push. The **scanout check** ([ADR-0041](adr/0041-desktop-capture-dmabuf.md)): a fullscreen window rendered through the GPU changes colour by itself every few seconds with the pointer hidden, and the decoded video must follow every change; the machine's KMS state must show the window was scanned out directly, or the check reports itself not run, since a notification banner, a menu or the overview stops scanout (the check moves the pointer first to clear an idle session's banner). CI's fixtures have no KMS CRTC and cannot run it. It is `fjarr-opsim desktop-scanout`, which the unattended verify runs after `desktop-control` with the same test window started as `FJARR_TESTWIN_BLINK_MS=2000 FJARR_TESTWIN_CURSOR=none` (its log is the oracle): for 20 s it reads the decoded centre's luma every 10 ms and the DRM debugfs `state` every 50 ms (so it runs as root). The screen it judges is the CRTC that drives the captured monitor: the connector the manifest names for the desk track (mutter's name, so the kernel's `HDMI-A-1` is mutter's `HDMI-1`), whose `crtc=` line in the state's `connector[…]` block picks the CRTC, and the largest framebuffer on it is the screen; the mini-PC has a ghost screen beside a real monitor, two CRTCs of one size, so the largest framebuffer on any CRTC could be the wrong screen. Only when no connector matches does it take the largest on any CRTC, and its log line says so. The window is scanned out directly when that framebuffer was not allocated by `gnome-shell`'s own thread (mutter 50.1 imports a client buffer for scanout from its `KMS thread`, measured with `kmsmon.py`). Each change must show in the decoded video within 1 s; it passes when at least 6 changes were watched and every one followed, with at least 4 under direct scanout; a change not followed fails whether scanned out or composited. With fewer than 4 changes under scanout it reports itself not run (a NOTE, not a failure). Without a KMS CRTC it reports itself not run too, except where it is given `--require-kms`, as the unattended verify gives it: the mini-PC has a CRTC, so none readable means debugfs or root is missing, and that fails. It also asserts that a hidden `cursor` shape arrived while the window hid the pointer, the one check of the hidden pointer against real mutter (not run when the capture fell back to the embedded cursor). A night whose scanout check did not run still passes the unattended test, but its verdict says so (`passed (desktop-scanout not run: …)`) and `lab-desktop-verify` raises its NOTE as a warning; the M3 gate's green night needs the check's PASS line. Its rules are unit-tested (`OpsimScanout.*`), which is all CI sees of it |

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
joins in slice 3.7c. The sound (M3.5 3.5.2, ADR-0034): a `kiosk` account
(uid 10004, in a `fjarr-desktop` group of gid 10003) runs its own session
bus, PipeWire and WirePlumber with the fixture's speakers, and
`fjarr-x11-session`, which hands the agent the sound over a socket shared
with the robot; the service plays, switches outputs and counts captures as
the mutter fixture's does, and `desktop-audio` runs in `make x11-e2e`. The fixture also stages what the mutter fixture
cannot: a monitor that moves (`/move`) or changes mode in place (`/mode`,
where a click must land on the new geometry's pixel; `make x11-e2e` fails
when that step did not run), an owner that never
answers and text over the limit on the clipboard; and its test window
stamps its top edge with a counter at 15 Hz (opsim's frame stamp), so
`desktop-hotplug` checks docs/06's "zero dropped frames on the other
monitors" as no skipped count across the plug and the renegotiation, rather
than as a gap, which cannot tell a lost picture from a late one (M3 gate
review, 2026-10-11).

**Both fixtures start their sound session as systemd starts a robot's.** On a
robot, `pipewire.socket` listens before anyone connects, and
`wireplumber.service` starts after PipeWire and is restarted when it fails.
The fixtures start the daemons by hand, so they keep the same promises
themselves (`docker/desktop-fixture/session.sh`, `docker/robot-sim/start.sh`):

- **PipeWire listens before WirePlumber starts.** The fixture waits up to 10 s
  for PipeWire to answer on the session manager's socket (`pw-cli -r
  pipewire-0-manager info 0`), and fails its start (`PipeWire never
  answered`, exit 1) when it never does. WirePlumber connects once and exits
  for good, with status 69 ("Failed to connect to PipeWire"), when nothing
  listens yet, and PipeWire's client does not retry.
- **WirePlumber is restarted when it fails, and every exit is logged** with
  its status, how long it ran and the time, in `wireplumber.log` and in the
  fixture's output. A clean exit (status 0, as after a deliberate SIGTERM) is
  not restarted, as under `Restart=on-failure`. Once the fixture is ready,
  five failures within 10 s are given up on (`start limit hit`), as under
  systemd's default start limit, rather than a crash loop filling the log.
- **Ready means the desktop has a default output.** With none 20 s (by the
  clock) after the rest of the session is up, the fixture prints `no default
  output`, whether WirePlumber runs and the tails of WirePlumber's and
  PipeWire's logs, and exits 1. `up --wait` fails there, instead of the
  suites failing a minute later with "no frames". Both limits are generous on
  purpose: the healthcheck allows about a minute, and a false fatal on a
  loaded runner would be the very intermittent failure this replaces.

Every desktop suite (`make desktop-e2e`, `desktop-browser`, `x11-e2e`,
`desktop-fixture-test`, the compose gate's desktop step) prints the
WirePlumber exits its fixture logged, or that there were none, when it passes
too: the restart loop would otherwise hide a crash that a passing run
recovered from. Each suite starts a fresh fixture container, so these lines and
the logs below are that run's alone. When a suite fails, it also prints the fixture's internals —
whether WirePlumber runs, the tails of the session daemons' logs, `pw-link -l`
and `wpctl status` (`fixture-session dump` in the mutter fixture, `start.sh
dump` in the kiosk) — and the robot's whole log, not its last lines. When the
fixture fails its own start, they print its log, which `up --wait` does not.

`make desktop-fixture-test` holds both fixtures to the contract with a
test-only fault, `FIXTURE_FAULT` (`docker/lab/fixture-startup-test.sh`):

- `wireplumber-first`: WirePlumber starts a second before PipeWire. The
  fixture must log the failed start (status 69), restart it and become ready
  with a default output, and the dump must show the running WirePlumber and
  leave the fixture healthy. Killed later (SIGKILL), WirePlumber must come
  back with the default output; stopped (SIGTERM), it must stay stopped.
- `pipewire-late`: the usual order, with PipeWire a second late. Only the wait
  keeps WirePlumber from losing the race, so the fixture must become ready
  with no WirePlumber exit at all. Then WirePlumber killed five times within
  10 s must be given up on, and say so.
- `no-session-manager`: WirePlumber never starts. The fixture must exit 1 with
  `no default output` instead of becoming ready.
- `no-pipewire`: PipeWire never starts. The fixture must exit 1, saying that
  PipeWire never answered.

Why (2026-10-17): six CI jobs from 2026-10-03 to 10-14, about 2 % of fixture
starts, failed on a fixture with no working session manager. The capture's
PipeWire stream was never linked ("no frames within 10 s", or the self-test's
"no frame"), and where the run recorded it, the desktop had no default output
either, for as long as anyone looked (11 to 73 s). A rerun on fresh daemons
passed. Starting WirePlumber first, on the old start, reproduces all of it,
down to the fixture turning ready after 15 s instead of 5 s. That shows what a
missing session manager does, not that the race is what happened in CI. The
race is the leading hypothesis, not an observation: the fixtures started both
daemons at once (1.96 ms apart on an idle workstation), and each failing
container took its WirePlumber log with it. The wait means the race cannot
happen again, so no future log can show it. The hypothesis holds if the
signature and every WirePlumber exit stay absent over the next ~150 fixture
starts in CI: were the cause still there at 1.8 % per start, 150 clean starts
would happen only 7 % of the time. A logged exit (its
status and how long it ran), or `no default output` with WirePlumber running,
points elsewhere.

Every lab job starts from a baseline: `setup --undo desktop`, install the build
under test, then `setup desktop` with the job's own settings. The machine's
state after a run is documented, and a person's experiments before it do not
decide its result.

**An undo that fails fails the job.** The prepare logs the undo's output to
`undo-desktop.log` beside its other logs and passes `undo` by name. When the
undo exits non-zero, the job fails as `FAIL undo` and prints the log's end,
which names each change it did not reverse, or the step or check that failed.
It does not go on with a warning, for two reasons:

- the baseline would then be whatever the failed undo left behind;
- `--undo desktop restores it` is an M3 promise that only this step exercises
  on real hardware.

A record entry whose kept copy is gone
([docs/26](26-robot-install-and-drivers.md#the-setup-tool)) fails every night
until a person repairs it, as the undo's message says
([docs/26](26-robot-install-and-drivers.md#the-setup-tool)): put the named
file's original back and run `setup desktop` once by hand, which copies it and
heals the entry; or, when the file is its original, copy it to the kept copy's
name with the command the message prints.
Until 2026-10-18 the prepare discarded the undo's output and status
(`>/dev/null 2>&1 || true`). The mini-PC's GDM kept logging in an account the
undo had deleted, unseen for two weeks
([M3 gate review, finding 18](reviews/m3-gate-review.md)).

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
