---
title: "Slice 4.5e Review"
description: Retrospective review of fjarr-connect — the link the ICE design could not make, the heartbeat nobody sent, and the one-line undefined behaviour that had been aborting the agent in every bulk transfer with the diagnosis already in the build log.
---

> Retrospective review of slice 4.5e per docs/13 and docs/20, written while the
> slice is still open: the operator client carries real IP traffic, and the two
> defects it exposed in code that had already been reviewed are worth recording
> before the rest of the slice buries them. The gate items still outstanding are
> listed under [Remaining](#remaining).

## What the harness found

| Found by | Defect | Fix |
|---|---|---|
| the first attempt at a real link | the peer connection failed with `pingAllCandidates called with no candidate pairs` after the ICE timeout. The client gathered before answering and discarded the agent's trickled candidates as noise, on the reasoning that its own answer already carried every candidate it had. The agent is webrtcbin: its offer advertises `a=ice-options:trickle` and carries **no** candidate lines, so those trickled candidates were the only remote candidates there would ever be | candidates trickle in both directions, as docs/08 and every other tier already do. docs/27 now says so, and the body classifier that dropped them is a pure function with a test named after the failure |
| `send_answer` never being reached | the answer was never sent at all, because `answer()` waited for ICE gathering to complete. webrtc-rs 0.21 **dispatches** `OnIceGatheringStateChangeEvent` in its driver and **never constructs it anywhere**, so `on_ice_gathering_state_change` cannot fire and the wait could never return | the answer goes out the moment it exists. Found by reading the crate's source after a wire trace showed three inbound frames and zero outbound |
| a 1 GiB `scp` over the link | the agent **aborted** — `double free or corruption`, SIGABRT, restarted by the supervisor — within seconds, every time, for `fjarr-connect` and for `fjarr-opsim` alike. The packet pump's fd-watch callback is declared `-> bool`; after its 32-read batch loop ended it returned **nothing**. GLib read the garbage as `G_SOURCE_REMOVE` often enough to unref a source the `SourceGuard` still owned | `return true;`, and `-Werror=return-type` project-wide so it cannot come back. `NetCapability.aSaturatedBatchKeepsTheWatchAndLosesNoPacket` asserts the return value and that the 33rd packet still arrives |
| the same transfer on debug and ASan builds | no abort, but the session ended with `reason="heartbeat"` at about 100 MB, every time. docs/08 requires a `ping` on control every 5 s and the agent ends the session when three are missed; `fjarr-connect` sent none. Every short check — `ssh`, a `curl`, the `open` handshake — fits inside the budget and passed, so the gap was invisible until a transfer outlived it | a 5 s heartbeat in the pump's `select!`, plus `--no-heartbeat` as a deliberate fault-injection switch (docs/15) so the agent's close-under-load path can be exercised on purpose. That switch is what made the abort reproducible 3 for 3 |
| running the gate five times instead of once | the first green run would have closed [#28](../18-open-questions.md) on one sample. Repeated, the gate was green 1 time in 5 | the abort is fixed and measured A/B; the remaining flakiness is recorded as still open, with core-loop starvation ruled out by measurement rather than by argument |

## Reviewer findings

| # | Severity | Finding | Resolution |
|---|---|---|---|
| 1 | **high** | the agent had been aborting in every saturating transfer since the pump was written, and `-Wall` printed `control reaches end of non-void function` on every build of that file the whole time. The build did not fail on warnings, CI mirrored the build, and three slices of tunnel work — including two reviews — read past it. The defect that cost the most to find was the one already written down | `-Werror=return-type` at the root, which turns exactly this class into a build error. The eight remaining warnings are listed under [Deferred](#deferred) rather than silenced |
| 2 | **high** | the first fix attempt did not stop the crash, and the honest reading at that moment was "wrong hypothesis". It was right, and the binary under test was stale. Two more hypotheses (`GBytes` ownership, core-loop starvation) were investigated before an A/B on the same lab settled it | the A/B — same lab, same command, fix stashed and restored, 3 runs each side — is what should have been run first. "It stopped happening" is not a cause; "it happens 3/3 without this line and 0/3 with it" is |
| 3 | medium | three of my own hypotheses were wrong in a row (missing return in release only, `GBytes` transfer semantics, loop starvation), and each was cheap to *rule out* only because each had a measurement attached. The `GBytes` one was settled by reading the GIR annotation, not by reasoning about it | recorded. The pattern that worked: every hypothesis gets an instrument before it gets a fix |
| 4 | medium | the operator end reimplements the two policy rules in Rust, so docs/27#isolation now has three implementations (agent C++, opsim C++, client Rust). A change to one that misses the others is a hole in the isolation guarantee, not a style problem | the Rust tests mirror the C++ cases one for one and say so in the module header. Sharing the rules across the language boundary would mean an FFI seam that [ADR-0015](../adr/0015-backend-integration-strategy.md) deliberately avoids |
| 5 | medium | `fjarr-connect` measured 53 Mbps where `fjarr-opsim` measures 264 Mbps on the same link and payload | the client was built with `cargo build`, unoptimised, and the comparison was never run with `--release`. Noted as a measurement to redo before any throughput claim is written down for the native client |
| 6 | low | no STUN server was configured, and the first version hardcoded a public one. Shipping that would have sent every operator's address to a third party the customer never chose, for a candidate the TURN path already covers | `--stun` / `FJARR_STUN`, empty by default, matching the agent and `@fjarr/core`. docs/27 records why |
| 7 | low | every diagnosis in this slice needed the wire, and each time the instrument was built after the confusion rather than before | every signaling frame is logged verbatim at `trace` in both directions. Telling "the peer never sent it" from "we never read it" is now one environment variable |

## Measured

| Case | Result |
|---|---|
| `fjarr.net open` through `fjarr-connect` | robot `100.70.118.224`, operator `100.64.0.1`, mtu 1280 — the same line `fjarr-opsim` prints |
| HTTP over the link, `-- <command>` with `FJARR_ADDR` | `GET /stats` → 200, 691 bytes |
| `ssh` over the link | `shell:robot@…`, link closed at 5 kB up / 5 kB down |
| 1 GiB `scp`, hash verified, through `fjarr-connect`, no video | 3 runs, 3 verified, ~53 Mbps (unoptimised client build — see finding 5) |
| The agent abort, pre-fix, forced with `--no-heartbeat` | 3 runs, 3 aborts, at 286 kB, 35 MB and 40 MB |
| The same path with `return true;` | 3 runs, 0 aborts; the session ends by heartbeat as designed and the agent survives |
| The full opsim gate after the fix | 11 passed, 0 failed; 1 GiB in 32 s (≈264 Mbps) with video at 32 fps throughout |
| The same gate repeated | green 1 run in 5 — [#28](../18-open-questions.md) is not closed |
| Core-loop health during two failing runs | the introspect endpoint, on that same loop, answered 261 of 261 polls, worst case 1.4 ms |

## Remaining

The slice's gate (docs/17) is not met yet. Outstanding: the two-robot isolation
regression, address-collision detection, the macOS `utun` path and its
cross-compile check, and a `--release` throughput measurement for the client.

## Deferred

| Item | Why it can wait |
|---|---|
| the other eight compiler warnings — three GStreamer `-Wcast-function-type` at signal-connection sites, three `-Wmissing-field-initializers` in `introspect_capability`, one `-Wdangling-else`, one `-Wformat-truncation` in `log.cpp` | none is undefined behaviour, and silencing them in the same change as the crash fix would have mixed a cure with a cleanup. Full `-Werror` wants the GStreamer casts wrapped first |
| the remaining half of [#28](../18-open-questions.md) | narrowed, not closed: the agent is healthy during the failure and the SCTP association is not carrying messages. The next arm is video **plus** a transfer through `fjarr-connect`, which separates usrsctp from the traffic pattern |
| the VA driver's `alloc-dealloc-mismatch` under ASan (`iHD_drv_video.so`, malloc vs `operator delete` inside `vaDestroyContext`) | entirely inside Intel's driver, reached from the encoder probe. It blocks ASan runs of the demo robot until suppressed, which is worth an `asan.supp` entry beside the existing `lsan.supp` when ASan on the full stack becomes routine |
