---
title: "Slice 4.5b Review"
description: Retrospective review of the slice that fixed the gates rather than the agent — a dead crash retry, a congested viewer that had quietly moved to the relay, a baseline taken from the encoder's startup overshoot, and the memory bug all of it had been hiding.
---

> Retrospective review of slice 4.5b per docs/13 and docs/20, run after the
> slice landed on `main` green (9bbd091). The slice had one job: make the
> gates trustworthy before 4.5c starts measuring media through a tunnel.
> Five gates were examined and **every defect was in a gate or a rig; none
> was in the agent.** Two of the five had been wrong since the day they were
> written.

## What the harness found

| Found by | Defect | Fix |
|---|---|---|
| checking CI on my own push | the lab job was already red on `main` (`toggle-keyframe`), and had been for at least one commit before the slice began. A red gate is a place defects hide, which the next row demonstrates | both addressed below; `main` is green as of 9bbd091 |
| reading that red job's log | three `g_source_unref_internal: assertion 'old_ref > 0' failed` per run, one per session close that had opened a pty: `add_fd_watch` unreffed the source after attaching it *and* handed it to `SourceGuard::attached`, whose contract is to take the caller's reference. `cancel()` then destroyed the source, freeing it, and unreffed freed memory — on the seam `fjarr.terminal` and `fjarr.net` both read through | cd81195, one line, plus the regression test the seam never had. It counts GLib criticals through a swapped default handler, since `g_log_set_writer_func` may be called once per process |
| CI on d33e76b | `silent-operator` passed every assertion and then died of SIGSEGV in opsim's own teardown — the exact case `opsim-all`'s retry guard existed for, which had **never once fired**: the loop ran each scenario through a nested `$(MAKE) opsim`, and make reports its own exit code 2 when a recipe dies of a signal, so `rc >= 128` could not be true. Measured: 139 from `docker compose exec` directly, 2 through make | 0dc749a: both targets share one command so the loop sees the child's status, and the retry now discriminates — a crash *after* a clean verdict is retried once, a crash *before* one fails. Both branches verified against stubs |
| iptables counters on the robot's egress | the three-viewer rate-control test impaired the robot's egress toward the viewer's container *before* that viewer connected, so ICE settled on the TURN relay: **14 MB of the "congested" viewer's media went to coturn against 3 packets down the impaired path**, and the test waited for a congestion response that could not come. Excluding TURN operator-side does not help — the robot offers its own relay candidate | d33e76b: the viewer is relay-only and the impairment sits on the robot's egress toward coturn. Direct viewers stay clean, measured: 34493 packets to the browser against 7384 to coturn |
| the test's own artifact | the same test compared the clean viewers against their own opening seconds. A fresh stream overshoots — 4.9 and 5.9 Mbps on a 4 Mbps target — then converges, so convergence read as a 15-20 % regression while every sample was `active` and isolation was never broken | judged against the configured target now, with the categorical half (never demoted) unchanged |
| 20 CPU spinners | `hotplug` fails only under load, with the identical signature (`test-second-flows`, 25 undecodable stamps). `smoke`-then-`hotplug` is green on an idle machine | no product change: the failure was my own concurrent sanitizer builds. The frame assertions now report the robot's view of the tracks on failure (`why_no_frames`), which is the evidence the investigation lacked |
| `make agent-log-gate-selftest`, against a reverted cd81195 | the terminal suite passes **4 of 4** while the robot logs three criticals — proof that a suite passes straight through memory corruption | 7f1c733: the gate refuses such a run, and its selftest fails if the gate stays quiet |

## Reviewer findings

| # | Severity | Finding | Resolution |
|---|---|---|---|
| 1 | medium | I reported `hotplug` as failing "deterministically after `smoke`" and wrote that into the plan. It was not: the determinism came from a machine loaded by my own builds, on a host that had been up six days and has since rebooted. The claim survived because I reproduced it several times in a row without varying the machine | corrected in docs/17. The lesson is the cheap one I skipped: vary the environment before calling a failure deterministic, and capture the robot's log at the time of failure rather than afterwards |
| 2 | medium | I suspected the fd-watch fix explained `hotplug`, because the suite went green right after it. I tested it by **re-introducing the bug**: 3 criticals confirmed, `opsim-all` still green — hypothesis refuted | recorded here because the correlation was strong enough to have been believed. The negative control cost one rebuild |
| 3 | medium | three timeouts were raised in this slice, which is normally how gates get weakened | each old number came from a design estimate that measurement contradicts (8-9 s against 11.9-20.4 s; 4 s against a documented 1 s throttle plus a GOP), every categorical assertion is untouched, and the measurements are in docs/16 so the next person argues with data rather than with a constant |
| 4 | low | I added an `--ice-policy host` option to opsim mid-investigation, then found the relay route strictly better | reverted (docs/13 KISS). An unused knob on a test tool is a knob that rots |
| 5 | low | `/stats` reports `selected_pair` as an empty string. It is the one field that answers "which path is this session on", and its absence is why the relay took an afternoon and a netfilter chain to find | deferred: worth populating, and a natural companion to the tunnel work in 4.5c/d where paths matter again |
| 6 | low | `netemToward` accumulates duplicate `u32` filters when called twice, because `tc qdisc replace` on an existing root keeps its filters. Harmless today — the duplicates match identically — but it makes the qdisc hard to read while debugging, which is when it is read | deferred, noted here so the next reader of `tc filter show` is not puzzled |
| 7 | low | the three-viewer test captures the simulator's stdout into `opsimOut` and only surfaces it on some paths, so a run that failed before that point recorded nothing about the subprocess it depended on | deferred with 4.5c's rig work, where opsim becomes the operator for ssh and ROS 2 |

## Measured

| Case | Result |
|---|---|
| Keyframe on re-enable, worst per 20-cycle run | 757, 905, 937, 949 ms on 14 cores with VA-API; ~2.9 s on CI's 2-core software encoder. Floor is the documented 1 s per-tier request throttle plus a GOP |
| Demotion of a viewer behind a bad link from the moment it connects | 11.9, 14.9, 16.4 s (relay, three runs); 18.9, 20.4 s (direct); the design estimate was 8-9 s |
| Where a "congested" viewer's media actually went | before the fix: 14 MB to coturn, 3 packets down the impaired path. After: 7384 packets to coturn while the two direct viewers sent 34493 to the browser |
| Clean viewers while a third is demoted | every sample `active`; steady state 4.00-4.05 Mbps against a 4 Mbps target, after an opening overshoot to 5.9 Mbps |
| Signal propagation | `docker compose exec` 139, the same through a nested make 2 |
| Suites | 102/102 unit (plain, ASan+UBSan+LSan, TSan), 8/8 `opsim-all`, 52/52 `loopback` + `stack`, three-viewer test 5/5 consecutive |

## Gate (docs/17 slice 4.5b)

Both suites green locally and in CI, with the causes understood rather than
tolerated, and the standing question answered by a gate that is proven able to
fire. `main` is green at 9bbd091.

## Deferred

- **opsim's post-verdict SIGSEGV** in its peer teardown (the upstream DTLS race, docs/23 slice 5b) is still there. It is now announced and retried once instead of masked by make's exit code, which is the honest state until someone owns the upstream fix.
- **The log gate watches `demo-robot` only.** `fjarr-server` and the browser have their own logs; extending it is cheap and should follow the first time one of them hides something.
- **Nothing stops a loaded machine from failing `hotplug` again.** The assertions now say which end starved, which is the useful half; a load guard would be guessing at a threshold.
- Findings 5, 6 and 7 above.
