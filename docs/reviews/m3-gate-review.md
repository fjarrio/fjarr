---
title: "M3 Gate Review"
description: The robot's desktop, promise by promise — met in CI on every push and by hand on the mini-PC with DMA-BUF capture, not yet on a scheduled night; the frozen picture behind green counters and ADR-0041, two harness bugs that made the nightly lie both ways, an --undo that leaves GDM logging in a deleted account, and what needs Erik.
---

> Gate review for M3 ([docs/17](../17-roadmap.md#m3)), started 2026-10-11 after
> slices 3.0–3.8, rewritten 2026-10-18 after a read-only audit of every
> criterion (2026-10-17) and the fixes it led to. Every promise is checked
> against evidence that can be pointed at: a CI run, a scheduled night on the
> mini-PC, or a by-hand run recorded with its date. Bold marks what the review
> itself built or ran.
>
> **Status 2026-10-18: not met.** Met in CI on every push (run 39110004862 on
> becf033, every job green). On the mini-PC, the unattended-access test passed
> whole by hand on 2026-10-18 with the DMA-BUF capture of
> [ADR-0041](../adr/0041-desktop-capture-dmabuf.md), the first pass since
> 2026-10-03. What remains: **a green scheduled night** (none yet; 10-18 failed
> at the prepare's download, fixed in 91455a3); **three checks with Erik**
> (clipboard non-ASCII under GNOME, separate windows in Firefox, two ghosts plus a real
> monitor on the new build); and **Erik's answer to the proposed amendments**
> below, for the criteria this hardware or these browsers cannot meet as
> written; the backend E mode change on the ghost, finding 18's fix with a
> clean second `--undo`, and finding 20's spec corrections
> ([what closes the gate](#what-closes-the-gate)). No release until then ([docs/17](../17-roadmap.md#m3)); the last tag
> is still v0.1.2.

## The gate, promise by promise

Verdicts: **met**; **met in CI** (the fixtures only, no hardware evidence
since the criterion's code changed); **met by hand** (on the mini-PC, not yet
on a scheduled night); **not met**; **unverified**; **amendment proposed**.

| Promise (docs/06, docs/17) | Verdict | Evidence |
|---|---|---|
| the operator controls the desktop end to end (docs/06) | met | CI every push, 39110004862: GNOME fixture `desktop-control` (clicks, Shift, text through the keymap, åäö, Alt+Tab, an untypable `Δ` refused by name); X11 fixture the same; the lab browser, real Chromium typing into the fixture (`dashboard.spec.ts`). The mini-PC by hand 2026-10-18 (build c5dba78, prepare run `by-hand-2026-10-18-dmabuf`): `desktop-control` 12 passed, 1 not run (the mode change, which has no plug there: finding 9) |
| … input-to-photon within budget (docs/06; docs/17: on the mini-PC) | met in CI; **met by hand on the mini-PC from the LAN, for c5dba78** (not yet on a scheduled night, which measures on the robot itself) | CI 39110004862: GNOME fixture p50 18 / p95 27 ms, X11 p50 50 / p95 52 ms (budget 150/250; CI asserts its 500 ms ceiling). **On the mini-PC from the dev machine, 2026-10-18 11:13–11:15, build c5dba78 (DMA-BUF capture), opsim from cbf5490 in the dev container**, the number of record by [docs/15](../15-testing-strategy.md#the-desktop-test-lab): `desktop-control` 5 times, each 12 passed and the mode change not run, each input-to-photon 10/10 samples: p50 42 / p95 61, 50 / 60, 50 / 62, 36 / 84, 58 / 93 ms; **all 50 presses p50 49 / p95 73 ms** (27–93 ms), within 150/250, recorded in [docs/16](../16-performance-budgets.md#measured). The path: a transient `fjarr-server` on the mini-PC on the LAN, the agent pointed at it by a runtime drop-in, the test window as in `unattended.sh`, its log served on the LAN; the session direct (no TURN), both machines on 5 GHz Wi-Fi. The same build measured on the robot itself, p50 200 / p95 202 ms by hand earlier on 10-18, is not a number of record: there the decoder shares the robot's CPU with the encoder (as on 10-01, 162 against 53 ms). `desktop-scanout` from the dev machine once: 10 of 10 changes followed within 1 s, all 10 under direct scanout, the hidden pointer arrived (the mini-PC's debugfs `state` was mirrored over ssh into opsim's container every ~50 ms, since opsim reads KMS locally). Everything was undone after: the agent back on `wss://signal.fjarr.io` as demo-robot-03, capturing by DMA-BUF. History on the robot: 159/160 ms on 10-03; 10/10 but p95 769 ms on 10-04 (the first build with 232dbbf, FAIL); nothing valid from 10-05 to 10-17 (findings 10 and 11). Amendment 6 states what was done here. Met for c5dba78; if amendment 6 is accepted and the gate closes on a later build that changes the capture, encode or input path, repeat this run |
| a reboot brings the desktop back with no local interaction (docs/06 "reboot of the sim"; unattended access) | met by hand on the mini-PC; as written (the sim), amendment 2 | the mini-PC 2026-10-18: rebooted 09:55:36, GDM's automatic login (`desktop` holds seat0), helper connected, capture of HDMI-1 ready, then `desktop-see` 5/5. The machine's own verdict passed this part every night it ran from 10-04 to 10-17; the night's workflow went red on later steps (findings 9–11). The watchdog brought a killed session back in 51 s (3.L); an out-of-memory kill was recovered by GDM within a minute (finding 1). robot-sim is a container and is never rebooted: amendment 2. A scheduled night is still owed |
| a stuck modifier never persists after a disconnect | met | `desktop-release-on-close` on both fixtures every push; the lab browser's Esc; the mini-PC by hand 2026-10-18 and in every nightly verdict since 10-04 |
| hot-plug: with two monitors streaming, a third is shown within 2 s | met in CI, from one monitor to two; by hand on 2026-10-11, **before ADR-0041; not repeated** | GNOME fixture `desk-virtual-2` offered 112 ms after the plug, X11 `desk-right` 409 ms (39110004862). Both scenarios start from exactly one monitor, so two-to-three never runs: amendment 7. CI exercises the shared-memory path only. The mini-PC: a Dell plugged into a live session arrived by renegotiation (2026-10-11), with no time recorded; a plug through the DMA-BUF capture is not yet shown on hardware |
| … with zero dropped frames on the others | met on X11; **unverified on GNOME** | X11 fixture `desktop-others-no-lost-frames`: the untouched monitor's window counter 281 → 320 across the plug, largest step 1 (every push since c53fe14). The GNOME fixture's window carries no stamp: only its longest gap is measured, which cannot tell a lost frame from a late one. [docs/16](../16-performance-budgets.md)'s row "101 ms, no frames dropped on the others" rests on that gap (finding 20). CI exercises the shared-memory path only. Never measured on hardware: amendment 4 |
| unplugging one leaves a placeholder and the rest untouched | met in CI; hardware with exceptions, **before ADR-0041; not repeated** | `desktop-first-still-flows` on both fixtures; the placeholder in `react.test.tsx`. CI exercises the shared-memory path only; an unplug through the DMA-BUF capture is not yet shown on hardware. On the mini-PC, unplugging the DisplayPort MST chain crashed GNOME Shell (SIGSEGV after a failed page flip, 2026-10-04; ADR-0006 measured it once in four in M2), and the X server stalls ~4 s while the chain re-probes (2026-10-05). Both are driver/upstream limits; amendment 1 states them |
| re-plugging restores the same `track_id` | met in CI; by hand on 2026-10-11, **before ADR-0041; not repeated** | `desktop-replug-same-id` on both fixtures; CI exercises the shared-memory path only. The mini-PC 2026-10-11, one Dell kept its id across DP-4, DP-5 and DP-6 (ids by EDID, ADR-0006); a re-plug through the DMA-BUF capture is not yet shown on hardware |
| a mode change keeps input coordinates correct | met on X11; **unverified on GNOME** | X11 fixture `desktop-mode-change`: "1920x1080 -> 1600x900 announced, frames again, a click at 0.25,0.5 landed on 400,450" (39110004862, every push since 9fc4c52). Mutter's virtual monitors cannot change mode, so backend E has only a stub-backend unit test; the mini-PC has no plug service. Its ghost's EDID offers 1600×900, so a scripted mode change there needs no person: amendment 5 |
| unplugging everything, then plugging one back, recovers without a reconnect | met by hand on 2026-10-11, **before ADR-0041; not repeated** | **run by the review** on the mini-PC (2026-10-11): booted with no monitor and no ghost, GNOME logged in, a dashboard session stayed up, and a Dell plugged in arrived in the same session (manifest versions 1 to 16); a plug through the DMA-BUF capture is not yet shown on hardware. No end-to-end test: `desktop-hotplug` never removes its first monitor, though [docs/15](../15-testing-strategy.md)'s fault table says it does (finding 20), and CI's hot-plug exercises the shared-memory path only |
| separate windows: one click, streams, takes input; closing the tab closes it | met | the lab browser every push (`dashboard.spec.ts`): one click opens the window, it streams, a click reaches the test window's log, it closes with the page. Erik on two screens with the mini-PC on demo.fjarr.io (2026-10-04) |
| … fullscreen with Keyboard Lock, Alt+Tab reaching the robot | met by hand (Chrome) | Erik in Chrome, 2026-10-04 (3.6); the spike's table: Alt, Tab, Super and Ctrl reached the page in both windows. CI checks `document.fullscreenElement` only, not `navigator.keyboard.lock()`: carried forward |
| … the same on Firefox | **unverified; amendment proposed** | one line in docs/17 ("and in Firefox", 4b620f8, 2026-10-04) with no record of what was tried. Firefox has no Keyboard Lock, and docs/22's own test plan gives it the degraded path. Needs Erik (below) and amendment 3 |
| clipboard both ways, non-ASCII, in the fixture and on the mini-PC | met in CI; **unverified on the mini-PC under GNOME** | both fixtures every push: "robot says åäö" read, "operator says ÅÄÖ" pasted, PNG both ways; the lab browser through the real clipboard. The mini-PC under GNOME: copying from the robot failed on 10-03 and was fixed (bf7f427), with no recorded re-test. Under the X11 kiosk (10-05) text and images both ways, non-ASCII not stated. Needs Erik (below). The image write's intermittent timeout stays open (finding 23) |
| `apt install fjarr-desktop-wayland` plus `setup desktop`, unattended after a reboot | met by hand | the prepare installs the five CI-built packages and runs `setup desktop --ghost-screens 1`, then reboots (`tools/fjarr-lab/unattended.sh`); by hand 2026-10-18 (the debs of c5dba78 from CI run 39076827413's `debs-amd64` artifact, installed by `unattended.sh prepare` from `~/fjarr-byhand-build`) it passed from install to `desktop-scanout`; the workflow's own download and install is what the scheduled night adds. `fjarr-desktop-wayland` depends on the agent and recommends the session package, so `apt install` resolves to the same set. The signed repository has not served M3 packages (no release since v0.1.2, by the rule) |
| `--undo desktop` restores it | **not met on the mini-PC**; met in the install test | **run by the review** on the mini-PC 2026-10-18 (below): every change setup made was reversed except one: `/etc/gdm3/custom.conf` still logs in the account the undo deleted, because its backup has been gone since 2026-10-05 and the nightly discards the undo's exit status (finding 18). The install test in a clean container passes every push (39110004862), including `--x11` and `--container` |
| the mini-PC headless with two ghosts plus a real monitor, all streaming with distinct ids | met by hand on 2026-10-11; **not repeated since ADR-0041** | **run by the review** (2026-10-11, after finding 1): ghost 1, ghost 2 and a Dell, three pictures, distinct ids, one primary, GPU memory flat at ~70 MB. The nightly uses one ghost, and today only HDMI-1 (the ghost) is connected. The capture path has changed since (c5dba78: DMA-BUF for physical monitors), so a re-run needs a Dell connected: Erik (below) |
| slice 3.4 done-when: the hot-plug criteria with a real monitor, the DisplayPort chain and two ghosts | **not met as written; amendment proposed** | impossible on this machine: its GPU has four display pipes, and two ghosts plus the three-monitor chain is five; mutter lit one monitor, a ghost (finding 7). What hardware has shown is in the rows above. Amendment 1 |
| an X11 kiosk from `fjarr-desktop-x11` | met | the mini-PC by hand, 2026-10-05: LightDM's automatic login into Openbox from the packages and `setup desktop --x11`; every monitor with its GNOME id, input with åäö, the local cursor, clipboard text and images, unplug and re-plug. `make x11-e2e` every push (39110004862), against the dev build rather than the package; the install test covers the package's setup and undo. Backend A is untouched by ADR-0041 |
| a containerized robot's desktop through the reference compose file | met | `make compose-gate` every code push: "compose-gate: PASS — the reference compose file runs a robot, tunnel and desktop included" (39110004862); 33 of 34 runs from 10-11 to 10-17, the one failure the fixtures' "no frames" (finding 16). It asserts seeing, not input |
| the agent cannot open the desktop user's microphone (3.3) | met | `fixture-helper-check` every push: a live microphone through an unnarrowed connection yields audio, through the handed one nothing; `SO_PEERCRED` checked |
| no release until M3 closes | holding | last tag v0.1.2 (2026-09-30); versions still 0.1.2 |

## The `--undo desktop` check on the mini-PC (2026-10-18) {#undo-check}

**Run by the review**, 10:50 to 10:52 CEST, outside the lab window, on the
state the by-hand prepare had left (build c5dba78, `setup desktop --yes
--account desktop --ghost-screens 1 --reboot no`). A snapshot of everything
`setup desktop` writes (`fjarr-setup`'s `desktop.rs` and `display.rs`),
then `sudo fjarr-agent setup --undo desktop`, then the same snapshot, diffed.

| What setup desktop wrote | After `--undo desktop` |
|---|---|
| account `desktop` (uid 1001) with its home, in `fjarr-desktop` | its session ended, the account and its home removed; `fjarr-desktop` keeps only `fjarr` |
| `~/.config/gnome-initial-setup-done`, both initial-setup user units masked, the helper's user unit linked | gone with the home, each also undone by name |
| `/etc/dconf/db/local.d/00-fjarr-desktop`, `/etc/dconf/profile/user`, `dconf update` | both files removed; the compiled `/etc/dconf/db/local` stays (finding 19) |
| `/usr/lib/firmware/edid/fjarr-ghost-1.bin`, `/etc/default/grub.d/fjarr-ghosts.cfg` | removed; `update-grub` ran, and `grub.cfg`'s kernel lines no longer carry `video=HDMI-A-1:…e drm.edid_firmware=…` (the running kernel keeps the ghost until a reboot) |
| `capabilities."fjarr.desktop".helper.user` / `.group` in `/etc/fjarr/fjarr.toml` | removed; the restarted agent logs "no desktop account configured" and reports no backend, by name |
| `fjarr-desktop-watchdog.timer` enabled | disabled and stopped |
| **`/etc/gdm3/custom.conf`: `AutomaticLoginEnable=true`, `AutomaticLogin=desktop`** | **unchanged**: "restoring /etc/gdm3/custom.conf from /var/lib/fjarr/setup-backups/etc%gdm3%custom.conf: No such file or directory — kept in the record". Exit status 1. GDM is at its greeter, configured to log in an account that no longer exists |

Every other entry left the record; the one that failed stays for the next
`--undo`, as designed. The cause is finding 18. **The machine was undone for
the check and set up again by hand afterwards:** at 11:05 `sudo fjarr-agent
setup desktop --yes --account desktop --ghost-screens 1`, then a reboot; at
11:07 the desktop held seat0 and the agent logged "capture of HDMI-1: DMA-BUF
AR24:0x0200000000000901, into the encoder without a copy".

## What the review found

| # | Severity | Finding | Resolution |
|---|---|---|---|
| 1 | **high** | **The session helper looped until the machine ran out of memory.** With two ghosts and a Dell, after the Dell came back under a new connector, mutter reported a ghost and the Dell both primary. The helper's rule ("a ghost is never primary") asked mutter again on every layout change, eight times a second; each layout reallocated every framebuffer, GPU memory reached the TTM pool's limit (7.2 GB) in seconds, and the out-of-memory killer ended the desktop session | the rule is a pure, tested function that does nothing when a real monitor is primary; the helper asks at most once per layout, always sends its monitors, and reports exactly one primary (53151f0) |
| 2 | **high** | **"Welcome to Ubuntu" over a robot's screen.** A fresh account's first login starts `gnome-initial-setup` over whatever the robot shows. Writing `~/.config/gnome-initial-setup-done` (75999c5) only traded it for Ubuntu 26.04's upgrade-mode wizard, which `ConditionPathExists` on that very file switches on, at every boot of the nightly's recreated account (found 2026-10-17). It was taken for the cause of the nightly's input-to-photon 0/10, which it was not (finding 11) | `setup desktop` masks both wizards' user units for the account, recorded for `--undo`; the install test checks both masks and their undo (8735c52; [docs/26](../26-robot-install-and-drivers.md#fjarr-agent-setup-desktop)). On the mini-PC 2026-10-18 both masks were in place, and the undo removed them |
| 3 | **high** (process) | **The nightly was red for two weeks, and nobody looked.** First the verify refused every verdict: the prepare ran under `sudo`, which dropped `GITHUB_RUN_ID`, so every night was recorded "by hand" (10-04 to 10-11). Then, with that fixed, the night stayed red on findings 9–11, and nothing reported it | the run id is an argument (b8e56ce). `nightly-alert` (91a550d) judges each night's prepare, verify and Nightly after the window and keeps one `nightly-red` issue open until a night is all green ([docs/30](../30-continuous-integration.md#nightly-alert)). It first fired on 2026-10-18 at 07:01 UTC and opened #1, "Nightly red since the night of 2026-10-18". Closing it on a green night is still to be seen |
| 4 | medium | `tunnel-ros-ordering` fact B (Cyclone) failed every night since 10-03: the check ran under `set -e`, so the first erroring `ros2 topic list` ended it in 1.7 s | a failing query is one more miss (2f5d07c); the echo is retried under the same deadline. Not an M3 criterion |
| 5 | medium | The soak's RSS budget failed at random: two single readings swung from −0.36 to +6.09 MiB on one commit | the median of the last quarter of cycles against the first quarter's (2f5d07c) |
| 6 | medium | Three gate criteria rested on a stub or a gap: a mode change, zero monitors, dropped frames | built or run for backend A and on the mini-PC (9fc4c52, c53fe14, the 2026-10-11 session); backend E's mode change and dropped frames are still open (amendments 4 and 5) |
| 7 | low | Ghosts and real monitors share the GPU's display pipes: the mini-PC's GPU drives four, and with two ghosts and the three-monitor chain mutter lit one monitor, a ghost | documented in [docs/26](../26-robot-install-and-drivers.md#ghost-screens); slice 3.4's done-when cannot be met as written (amendment 1); `display add-ghost` could warn: carried forward |
| 8 | low | The X11 fixture could not change a monitor's mode (Xvfb refuses `--setmonitor` on an existing name), and the hot-plug scenario's last unplug raced the next run | delete and add in one `xrandr` run; the scenario waits for the agent to see its unplug |
| 9 | **high** | **The review's own mode-change test failed the mini-PC by construction.** 9fc4c52 skipped `desktop-mode-change` only on HTTP 404. The mini-PC runs opsim without `--desktop-plug`, so the request URL was the relative `"/mode"`, libsoup refused it, the status was 0, not 404, and the step ran and failed: deterministic on 10-13, 10-14 and 10-17 | it runs only with `--desktop-plug`, prints "NOTE desktop-mode-change: not run" otherwise and on 404, and fails on any other answer; `make desktop-e2e` runs `desktop-control` without a plug as the mini-PC does, so the NOTE path is checked every push; `make x11-e2e` requires the PASS (5b864a3) |
| 10 | **high** | **input-to-photon passed a frozen picture.** After each F9 release opsim waited for the centre to return to its baseline and ignored the result, so once the picture stuck on green every later press "lit" at once: on 10-17 the machine reported PASS with p50 0 ms (p95 201). On 10-13 and 10-14 the same freeze gave 0/10 | a sample needs the centre back within 8 of the baseline first; rejected presses are named and the frame dumped. The rules moved into `opsim/checks.{hpp,cpp}` with 16 unit tests that fail on the old logic (5b864a3) |
| 11 | **high** | **The operator saw a frozen picture behind green counters.** Since the local cursor (232dbbf, 2026-10-03), mutter keeps the hardware cursor, so it scans a fullscreen GPU window directly out, and mutter 50.1 then records nothing into a shared-memory monitor stream. The reader's keepalive re-sent the last frame every 100 ms, so `desktop-frames` and `desktop-still-screen` passed on a picture that no longer followed the screen. The first failing night was the first build with 232dbbf | measured on the machine ([spikes/capture-scanout](../../spikes/capture-scanout/upstream-report.md)): DMA-BUF streams record every scanned-out frame. Erik chose DMA-BUF capture without a copy into VA ([ADR-0041](../adr/0041-desktop-capture-dmabuf.md), 4b656f3), built in c5dba78 with shared memory as the named fallback; `fjarr-opsim desktop-scanout` (a self-blinking fullscreen window the decoded video must follow, judged on the captured connector's CRTC) runs in the unattended verify with `--require-kms`; a NOTE becomes a run annotation (48027d6). **Verified by hand 2026-10-18**: "capture of HDMI-1: DMA-BUF AR24:0x0200000000000901, into the encoder without a copy", `desktop-scanout` 10 of 10 changes followed, 9 under direct scanout, a hidden pointer as a `hidden` shape. Upstream fixed the SHM side in mutter 50.5/51.0, not in Ubuntu 26.04's 50.1 ([#49](../18-open-questions.md)); the SRU request is drafted for Erik |
| 12 | medium (process) | **The verify called a late prepare stale.** It measured 12 h from the prepare run's creation, so on 10-16 it refused a prepare that had waited in GitHub's queue and that the machine was verifying at that moment | it judges by when the prepare's mini-PC job ran, waits while tonight's prepare is not yet created, and stops polling when the machine has not rebooted (91a550d; `tools/ci/tonights_prepare.py`, tested in `tools/ci/test.sh`) |
| 13 | medium | **The night of 10-12 failed at install**: 33fd7d5 moved `pipewire-client.conf` from `fjarr-desktop-wayland` to `fjarr-agent` without `Replaces`, and every build is 0.1.2, so dpkg refused | `Replaces` (cad1259) |
| 14 | medium | **The prepare's download was truncated without an error.** On 10-18 the mini-PC's download of `lab-desktop-build` over its Wi-Fi ended "successfully" with 4 of its 7 files, and the night failed at `chmod` | `tools/ci/lab-artifact-check.sh` checks the five packages (readable by `dpkg-deb`) and both binaries by name, fetches once more when short, and fails naming what is missing (91455a3) |
| 15 | medium | **Desktop notifications kept the test window from being scanned out.** NetworkManager tried DHCP on the ZED Box Mini's USB gadget network (its recovery cable on the mini-PC) every few minutes and posted "Connection failed" banners, which composite over the fullscreen window, so the scanout check could not exercise the path it exists for | on the mini-PC, by hand 2026-10-18: `/etc/NetworkManager/conf.d/90-fjarr-lab-zedbox-usb.conf` leaves those two MACs unmanaged. It is not in the repository's lab setup: carried forward |
| 16 | medium | **The desktop fixtures' intermittent "no frames"**, about 2 % of fixture starts in CI from 10-03 to 10-14. It was not ICE (the `selected_pair=""` opsim printed is a field the agent never fills); the capture's PipeWire stream was never linked because no session manager worked. The fixtures started WirePlumber and PipeWire at the same instant, and WirePlumber 0.5.13 exits for good when its one connect fails; starting it first reproduces the whole signature | WirePlumber starts once PipeWire accepts connections, restarts with each exit logged, and the fixture fails readiness without a default output; "no frames" now says whether the robot sent RTP at all (5b864a3, `docker/lab/fixture-startup-test.sh`). The race cannot happen on the new start, so the hypothesis holds if the signature and every WirePlumber exit stay absent over ~150 fixture starts: 8 CI runs with the desktop jobs since, "no WirePlumber exit logged" in each. Carried forward until the count is reached |
| 17 | medium | **Two nights lost to a power cut.** The mini-PC was off from 10-14 21:17 to 10-16 21:50 with no shutdown in its journal: its firmware does not power on when power returns | [docs/12](../12-development-environment.md#lab-machines-and-fjarr-lab) (91a550d): lab machines power on after AC loss; the mini-PC's firmware setting is pending (Erik) |
| 18 | medium | **`--undo desktop` leaves GDM logging in a deleted account on the mini-PC.** On 2026-10-05 an undo interrupted during `update-grub` (Ctrl-C) had already restored `custom.conf` and removed its backup without saving the record. 26453b3 made the undo save the record after each change, but did not repair the entry already stranded. Since then `setup desktop`'s `write_file` sees the existing record entry and never takes a new backup, so every undo fails on it, unseen because `unattended.sh prepare` discards the undo's output and status (`>/dev/null 2>&1 \|\| true`). Shown by the review's run above: exit 1, `AutomaticLogin=desktop` with no `desktop` | open (fix in `fjarr-setup` and in `unattended.sh`'s handling of the undo's status, then a clean second `--undo`) |
| 19 | low | The undo removes `/etc/dconf/db/local.d/00-fjarr-desktop` and the profile, and runs `dconf update`, which leaves the compiled `/etc/dconf/db/local` it built earlier. Inert, as no profile names it any more, but a file setup made that undo does not remove | open: record the compiled database as a file setup created when it did not exist before |
| 20 | medium | **The specs claim more than the tests do.** [docs/15](../15-testing-strategy.md#the-desktop-test-lab) says the mini-PC nightly covers ghosts beside a real monitor, the three-monitor chain, hot-plug on real connectors and input-to-photon against the budgets; it runs see, control and scanout on one ghost, and its photon is not a number of record by docs/15's own rule. docs/15's fault table says the fixture recovers from zero monitors; `desktop-hotplug` never removes its first. [docs/16](../16-performance-budgets.md)'s "no frames dropped on the others" rests on a GNOME gap measurement | open: correct the three texts to what runs, or build what they claim |
| 21 | low | **The Nightly's netem `wifi-ok` verdict depends on when the agent demotes.** opsim expects the active tier for the whole 5 s window (≥ 100 encoded units); the agent demotes to the 5 fps thumbnail tier when opsim's receiver over-reports loss under jitter, which the table's own comment calls right. Demoted at 3.0 s on 10-17: 90 units, red; at 4.2 s on 10-15: 126 units, wifi-ok green (that night was red on `lossy`); 10-18 (39106826888): 149 units, green. Not an M3 criterion, but it reds the night and keeps #1 open | open |
| 22 | medium | **"Add a screen" can stay "requested".** CI 38838322402 (10-16): the operator's virtual monitor, created in metadata cursor mode, streamed only cursor buffers, the documented case of a monitor that has not painted; module E restarts a real monitor in embedded mode there, but `restart_capture` returns early for virtual ones | open |
| 23 | medium | **The image clipboard write times out intermittently**: about 1 in 15 CI runs in October, 4 of 8 back to back on a development host (2026-10-12), and on the X11 kiosk too, so the write is lost inside the agent before any backend; seen again in c5dba78's local runs | open; opsim waits 15 s and reports the agent's reason |

Findings 1, 2 and 11 share a shape: each was invisible to every test that
ran, because no test had the mini-PC's combination (two ghosts and a real
monitor re-plugged; a freshly created account; a GPU that scans a fullscreen
window out). Findings 3, 9, 10 and 18 are the other half: the one test that
had the combination was not believed or not heard. It failed on its own
bookkeeping, failed by construction, passed a frozen picture, and threw away
the exit status of the step that undoes it. A machine test is only evidence
when it can fail, does fail for the right reason, and someone hears it.

## Proposed amendments (for Erik) {#amendments}

Proposals only: docs/17 and docs/06 are unchanged until Erik accepts them.

1. **Slice 3.4 done-when** (docs/17). Now: "the docs/06 hot-plug criteria on
   the mini-PC with a real monitor, the DisplayPort chain and two ghosts, each
   with its own stable id". Proposed: "`desktop-hotplug` in CI; on the
   mini-PC, two ghosts plus one real monitor streaming with distinct stable
   ids (the GPU has four display pipes, so two ghosts and the three-monitor
   chain cannot be lit together, docs/26#ghost-screens), and the three-monitor
   chain without ghosts: a monitor plugged into a live session shown within
   2 s, one unplugged leaving the others streaming, re-plugged under the same
   id. Unplugging the MST chain as a whole can crash GNOME Shell (upstream,
   ADR-0006); there the criterion is that the desktop comes back by itself."
2. **"reboot of the sim"** (docs/06). Proposed: "a reboot of the robot (the
   mini-PC; robot-sim is a container) brings the desktop back with no local
   interaction".
3. **"the same on Firefox"** (docs/06, slice 3.6). Firefox has no Keyboard
   Lock. Proposed: "in Firefox the window opens with one click, streams, takes
   input and goes fullscreen, and closes with the tab; Keyboard Lock, and so
   Alt+Tab reaching the robot, is promised in Chromium only", matching docs/22's
   degraded path.
4. **"zero dropped frames on the other two"** (docs/06). Either keep it and
   stamp the GNOME fixture's test window as the X11 one is (preferred: no
   amendment), or state it as "counted on backend A; on backend E by the
   longest gap until its fixture is stamped".
5. **"a mode change keeps input coordinates correct"** (docs/06). Mutter's
   virtual monitors cannot change mode. Proposed: "in CI on backend A; on
   backend E on the mini-PC, by a scripted `ApplyMonitorsConfig` on the
   ghost (1920×1080 to 1600×900) and a click at a normalized point", a
   check that needs no person.
6. **"input-to-photon within budget on the mini-PC"** (docs/17 gate). Proposed
   to add: "measured from another machine on the LAN (docs/15), on the build
   that closes the gate", so the 10-01 number is not carried across two
   rewrites of the capture path.
7. **"with two monitors streaming, adding a third"** (docs/06). Both fixtures
   start from one. Either extend `desktop-hotplug` to plug a second monitor
   before the measured one (preferred), or state it as "with monitors
   streaming, adding one more".

## What closes the gate {#what-closes-the-gate}

**Needs Erik at the mini-PC** (on the DMA-BUF build, c5dba78 or later):

- **The clipboard under GNOME, non-ASCII both ways**: åäö copied in a GNOME
  application pastes in the browser, and ÅÄÖ pasted in the browser pastes in
  a GNOME application; one PNG each way while there.
- **Separate windows in Firefox**: one click opens the window, it streams,
  takes input, goes fullscreen and closes with the tab; and what Alt+Tab does
  there (amendment 3).
- **Two ghosts plus a real monitor**: a Dell connected or powered on (only the
  ghost is connected today), `fjarr-agent display add-ghost` to two, a reboot,
  all three streaming with distinct ids and one real primary; the Dell
  unplugged (the ghosts keep streaming) and plugged back (shown within 2 s,
  same id), the first plug and unplug through the DMA-BUF capture; then, per
  amendment 1, the chain without ghosts: plug into a live session (shown
  within 2 s), unplug one (others untouched), re-plug (same id).
- **The mini-PC's firmware**: restore on AC power loss = Power On (finding 17).
- Not at the machine: accept or change amendments 1–7; file the Launchpad SRU
  for mutter's fix (#49, `spikes/capture-scanout/upstream-report.md`).

**Needs a green scheduled night** (no person; the first chance is the night of
2026-10-19):

- `lab-desktop-prepare` and `lab-desktop-verify` green on schedule: the
  checked download (91455a3), install, `setup desktop --ghost-screens 1`, the
  reboot, the automatic login, `desktop-see`, `desktop-control`, and
  `desktop-scanout --require-kms` under DMA-BUF, judged by when the prepare
  ran (91a550d). This is the review's closing condition, as M1's was.
- `nightly-alert` closing #1 on an all-green night, which also needs the
  Nightly green (finding 21).

**Needs no person, not yet done:** the backend E mode change on the ghost
(amendment 5); finding 18's fix and a second `--undo` run that leaves nothing
behind; the corrections of finding 20. When the gate is met, docs/17's
status table gets its M3 row linking this review.

**Done 2026-10-18, no person needed:**

- Input-to-photon from the dev machine over the LAN on the DMA-BUF build, as
  in 3.L (amendment 6): p50 49 / p95 73 ms over 50 presses, a new docs/16 row
  (the promise's row above). Met for c5dba78; if amendment 6 is accepted and
  the gate closes on a later build that changes the capture, encode or input
  path, repeat this run.

## Carried forward

- **Keyboard Lock in CI**: the separate-window test checks fullscreen only;
  assert that `keyboard.lock()` was called, or that a combo typed in the
  fullscreen window reaches the oracle.
- **The lab machine's NetworkManager file** (finding 15) belongs in the lab
  setup, with the ZED Box's MACs as its input, rather than by hand.
- **The fixtures' WirePlumber hypothesis** (finding 16) until ~150 clean
  fixture starts in CI.
- **The offer a browser never answered** (2026-10-05): the trigger is gone and
  the page recovers on its own; the cause is not known.
- **An X11 kiosk with the agent in a container** is specified, not built; the
  X11 fixture runs the dev build, not the package.
- **`display add-ghost` does not count display pipes** (finding 7).
- ~~**A red nightly reaches someone**~~: built (finding 3).
