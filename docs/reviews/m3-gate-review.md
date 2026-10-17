---
title: "M3 Gate Review"
description: The robot's desktop, promise by promise — what each gate criterion rests on, the defects the review found (a helper loop that exhausted memory, a welcome wizard over the robot's screen, a nightly that had refused its own verdict for a week), and what M3 carries forward.
---

> Gate review for M3 ([docs/17](../17-roadmap.md#m3)), started 2026-10-11 after
> slices 3.0–3.8. Every promise is checked against evidence that can be pointed
> at: a CI scenario, a nightly run on the mini-PC, or a hardware session with
> Erik recorded in the roadmap. Where the evidence was a stub or a gap, the
> review built the test. **Status: pending the nightly of 2026-10-12**, the
> first whose verdict the lab can report (finding 3); with it green, the gate
> is met.

## The gate, promise by promise

| Promise (docs/06, docs/17) | Verdict | Evidence |
|---|---|---|
| the operator controls the desktop end to end, input-to-photon within budget | met in CI and on the mini-PC; the nightly pending | fixtures every push: GNOME and X11 `desktop-control`, input-to-photon p50 39–50 ms, p95 49–54 ms (budget 150/250). The mini-PC: p50 52, p95 59 ms in 3.L. Its nightly measured 0/10 on every night it ran from 2026-10-05 to 10-14, first put down to GNOME's welcome wizard over the test window (finding 2). It was not the wizard (corrected 2026-10-17): the first-login wizard also ran on 10-02 and 10-04, nights with ten real samples; on 10-13 and 10-14, with the upgrade-mode wizard running, the input still reached the test window; and the 10-14 frame dump is the full-screen test window, not the wizard. The cause is still open |
| a reboot brings the desktop back with no local interaction | met; the nightly's report pending | the mini-PC every night: GDM's automatic login, the helper, frames and input after the reboot, in the machine's own log. The workflow reported failure for a week on bookkeeping (finding 3). The out-of-memory kill of finding 1 also showed recovery: GDM logged the account back in within a minute |
| a stuck modifier never persists after a disconnect | met | `desktop-release-on-close` on both fixtures every push, and nightly on the mini-PC |
| hot-plug: a third monitor shown within 2 s | met | X11 fixture 104–482 ms, GNOME fixture 101 ms; the mini-PC's Dells on the DisplayPort chain |
| … with zero dropped frames on the others | met on X11; GNOME by gap | **built by the review**: the X11 test window stamps a 15 Hz counter, and the untouched monitor's never skips across the plug and the renegotiation (largest step 1, every run). Before, only the longest gap was measured, which cannot tell a lost picture from a late one. The GNOME fixture's window has no stamp yet |
| unplugging one leaves the rest untouched; re-plugging restores the same `track_id` | met | `desktop-hotplug` on both fixtures; the mini-PC, where one Dell kept its id across DP-4, DP-5 and DP-6 |
| a mode change keeps input coordinates correct | met on X11 | **built by the review**: the X11 fixture changes LEFT's mode in place, and a click at a normalized point lands on the new geometry's pixel. Before, only a unit test against the stub backend. Mutter's virtual monitors cannot change mode |
| unplugging everything, then plugging one back, recovers without a reconnect | met | **run by the review** on the mini-PC (2026-10-11): booted with no monitor and no ghost, GNOME logged in, a dashboard session stayed up, and a Dell plugged in arrived by renegotiation in the same session (manifest versions 1 to 16) |
| separate windows, fullscreen with Keyboard Lock, closing with the tab, in Firefox | met | the lab browser test every push; Erik on two screens in Chrome and Firefox (3.6) |
| clipboard both ways, non-ASCII, in the fixture and on the mini-PC | met | both fixtures every push, PNG images included; the mini-PC under GNOME (3.5) and under the X11 kiosk (3.7) |
| `apt install` plus `setup desktop`, unattended after a reboot, `--undo desktop` restoring it | met | the nightly prepare does exactly this each night; the install test in a clean container; restoring GNOME from the kiosk found three `setup` bugs, fixed the same evening (26453b3) |
| the mini-PC headless with two ghosts plus a real monitor, all streaming with distinct ids | met — after finding 1 | **run by the review** (2026-10-11): ghost 1, ghost 2 and a Dell, three different pictures, one primary |
| an X11 kiosk from `fjarr-desktop-x11` | met | the mini-PC with LightDM and Openbox (3.7) |
| a containerized robot's desktop through the reference compose file | met | `make compose-gate` step 7 in CI: the reference file's agent with the GNOME image beside a desktop host (3.8) |

## What the review found

| # | Severity | Finding | Resolution |
|---|---|---|---|
| 1 | **high** | **The session helper looped until the machine ran out of memory.** With two ghosts and a Dell, after the Dell came back under a new connector, mutter reported a ghost and the Dell both primary. The helper's rule ("a ghost is never primary") asked mutter again on every layout change, eight times a second; each layout reallocated every framebuffer, GPU memory reached the TTM pool's limit (7.2 GB) in seconds, and the out-of-memory killer ended the desktop session | the rule is a pure, tested function that does nothing when a real monitor is primary; the helper asks at most once per layout, always sends its monitors (it had left the agent with none when mutter made no change), and reports exactly one primary (53151f0) |
| 2 | **high** | **"Welcome to Ubuntu" over a robot's screen.** A fresh account's first login starts `gnome-initial-setup`, in the middle of the screen, over whatever the robot shows, until someone at the machine clicks through it. Every robot set up by `setup desktop` would have shown it. Erik saw it on the mini-PC's screen. It was also taken for the cause of the nightly's input-to-photon 0/10, which it was not (corrected 2026-10-17, see the first table) | `setup desktop` writes `~/.config/gnome-initial-setup-done`, recorded for `--undo` (75999c5). That alone traded the first-login wizard for Ubuntu's upgrade-mode one, which then started at every boot of the nightly's account (found 2026-10-17). `setup desktop` now masks both modes' user units for the account, also recorded ([docs/26](../26-robot-install-and-drivers.md#fjarr-agent-setup-desktop)) |
| 3 | **high** (process) | **Both nightlies had been red for a week, and nobody looked.** The lab's verify refused every verdict since 2026-10-04: the prepare job ran the script through `sudo`, which dropped `GITHUB_RUN_ID`, so every night was recorded "by hand", hiding the machine's own result | the run id is an argument (b8e56ce). A red nightly needs to reach someone: carried forward |
| 4 | medium | `tunnel-ros-ordering` fact B (Cyclone) failed every night it ran since 2026-10-03: the check ran under `set -e`, so the first `ros2 topic list` that errored ended it in 1.7 s, defeating the 30 s poll written for exactly that | a failing query is one more miss (2f5d07c); the nightly is the proof, as this host's relay session would not connect. The nightly of 2026-10-12 still failed fact B under Cyclone, now in the second step: the topic was listed (the daemon remembered the recreated participant) and `ros2 topic echo` errored at once instead of waiting; it is retried under the same deadline too |
| 5 | medium | The soak's RSS budget failed at random: two single readings swung from −0.36 to +6.09 MiB on one commit | the median of the last quarter of the cycles against the first quarter's (2f5d07c); the census stays the exact leak check |
| 6 | medium | Three gate criteria rested on a stub or a gap: a mode change, zero monitors, dropped frames | built or run, as the table says (9fc4c52, c53fe14, the 2026-10-11 hardware session) |
| 7 | low | Ghosts and real monitors share the GPU's display pipes: the mini-PC's GPU drives four, and with two ghosts and the three-monitor chain mutter lit one monitor, a ghost | documented in [docs/26](../26-robot-install-and-drivers.md#ghost-screens); `display add-ghost` could warn: carried forward |
| 8 | low | The X11 fixture could not change a monitor's mode (Xvfb refuses `--setmonitor` on an existing name), and the hot-plug scenario's last unplug raced the next run | delete and add in one `xrandr` run; the scenario waits for the agent to see its unplug |

Findings 1 and 2 share a shape: each was invisible to every test that ran,
because no test had the mini-PC's combination (two ghosts and a real monitor
re-plugged; a freshly created account). Finding 3 is why they stayed hidden
a week longer: the one test that had the combination reported on its own
bookkeeping instead.

## Carried forward

- **A red nightly reaches someone** — an issue opened, or a message, when a
  scheduled run fails; a week of red went unseen.
- **The image clipboard-write stall** — about 1 in 15 runs in CI in October,
  before the backend is reached; opsim now waits 15 s and reports the agent's
  reason. Run back to back on a development host it is far more frequent: 4 of
  8 `desktop-clipboard` runs on 2026-10-12, the same with and without 3.5.2's
  changes; the helper never receives the write, and the agent logs nothing.
  It stalls on the X11 kiosk too (2026-10-12), where no helper is involved:
  the write is lost inside the agent, before any backend.
- **The offer a browser never answered** (2026-10-05) — the trigger is gone and
  the page recovers on its own; the cause is not known.
- **The desktop fixtures' intermittent "no frames"**, first seen in the compose
  gate's desktop step (75999c5): six CI jobs from 2026-10-03 to 10-14, about
  2 % of fixture starts, in the compose gate, `desktop-e2e`, the X11 fixture's
  sound and the fixture self-test. Each passed again on fresh daemons. It was
  not ICE: ICE, DTLS and the control channel connected in every failing
  session, and the `selected_pair=""` opsim printed is a field the agent never
  fills (opsim now says "no RTP sent on any track"). The fixture had no working
  session manager. The capture's PipeWire stream was never linked, and where
  the run recorded it, the desktop had no default output either. Starting
  WirePlumber before PipeWire reproduces the whole signature (2026-10-17): no
  frames within 10 s, no virtual monitor, no `desktop-audio` track, and the
  fixture ready only after 15 s, as in the failing `desktop-e2e` run. That
  WirePlumber lost this start race in CI is the leading hypothesis, not yet
  observed, because the failing containers took their logs with them. 293c49c
  read the X11 case (15c608f) as a fixture ready too early, but its output
  never came in 30 s. The fixtures now start as systemd does on a robot, log
  every WirePlumber exit, and fail their own start without a default output.
  Every desktop suite prints the WirePlumber exits, and a failed one also the
  fixture's internals and the robot's whole log
  ([docs/15](../15-testing-strategy.md#the-desktop-test-lab)). The race cannot
  happen on the new start, so no log will show it: the hypothesis holds if the
  signature and every WirePlumber exit stay absent over the next ~150 fixture
  starts in CI, and a logged exit, or no default output with WirePlumber
  running, points elsewhere. Carried forward until then.
- **The GNOME fixture's test window has no frame stamp**, so lost frames are
  measured on X11 only.
- **An X11 kiosk with the agent in a container** is specified, not built.
- **`display add-ghost` does not count display pipes.**
