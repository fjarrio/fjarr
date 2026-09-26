---
title: "Slice 4.5c Review"
description: Retrospective review of the slice that made the tunnel carry something — a shell and a verified 1 GiB transfer through sidecars on the robot's own network stack, question #23 answered, and a 40 % bulk stall found, characterised and left open.
---

> Retrospective review of slice 4.5c per docs/13 and docs/20. The slice gave the
> link something other than the simulator's own assertions to carry: `ssh` and
> `scp` through the tools a developer already owns. Two thirds of the gate are
> met and the third is **blocked on a defect this slice found**, which is
> recorded as [question #28](../18-open-questions.md) rather than worked around.

## What the harness found

| Found by | Defect | Fix |
|---|---|---|
| the first `ssh` over a link | "Connection refused" while the tunnel plainly worked (an HTTP request to the same address answered in 1 ms): `tun-up` recreates the robot, and a sidecar joined to its network namespace with `network_mode: service:` is left pointing at a namespace that no longer exists — reachable enough to answer with a TCP reset, which reads as "nothing is listening" | `tun-up` recreates the namespace-sharing sidecars after the robot, named in one `ROBOT_SIDECARS` variable so the next one is not forgotten |
| the second `ssh` | refused again, intermittently: `robot-services` builds its payload file before it binds sshd, and `tun-up` returned as soon as the *agent* was online — the same readiness race as 4.5a's `robot-offline`, one layer out | `tun-up` waits for something to be serving ssh in the robot's namespace, and says which log to read when nothing does |
| `--exec` with a shell one-liner | `$FJARR_ADDR` arrived as `JARR_ADDR`: the variable was being eaten by make, then bash, then `sh -c` | the gate's commands live in `docker/lab/tunnel-checks.sh` instead of being escaped through three layers, which also makes them readable and reproducible by hand |
| changing the payload size | the size reverted to its default: `tun-up`'s sidecar recreate re-resolved the environment, so `FJARR_LAB_FILE_MB` set on an earlier command was lost — **the third instance of this compose trap in this project**, after TURN/webhook and the encoder (e739216) | `LAB_FILE_MB` is a Makefile variable passed explicitly at the point of recreate |
| the ssh private key | published root-owned, so the operator could not read it, and ssh refuses a key others can read anyway | `robot-services` chowns it to the operator container's uid, which is passed in because the sidecar cannot know it otherwise |

## Reviewer findings

| # | Severity | Finding | Resolution |
|---|---|---|---|
| 1 | **high** | a bulk transfer over the tunnel stalls in roughly **40 %** of attempts (4 of 10 at 64 MiB, 2 of 5 at 1 GiB), at the onset of the flow, and **no counter at either end moves**: zero `dropped_queue`, `dropped_mtu`, `dropped_policy`, `dropped_no_peer`, zero refused sends, zero failed device writes. `ssh` and small HTTP requests over the same link never stall | not fixed, and not worked around: [question #28](../18-open-questions.md), with the ruled-out causes recorded so the next attempt starts further along. CI asserts the shell and records the transfer; docs/17 says the M4.5 gate cannot claim a reliable `scp` until it is settled |
| 2 | medium | I proposed a mechanism for #28 — the stream class is unreliable with no retransmission, so the transport may discard below our 4 MiB watermark without telling the sender — and tested it by lowering the watermark to 256 KiB. **5 of 5 passed, and I nearly recorded that as the fix.** The counters said the new watermark had never triggered, which cannot explain anything; ten more runs gave 6 of 10, the same rate as before | reverted. The lesson is the one 4.5b taught twice: a run of green is not evidence, and an instrument that says "this never fired" outranks a result that says "it worked" |
| 3 | medium | I read `tx_packets` from the first `link-stats` matching a predicate and reported it as a total. It is a per-second event, so that number was a snapshot from the start of the run, and I drew a conclusion from it ("the robot sent 4769 packets and stopped") that the data did not support | a `last_link_stats` helper reads the last sample, and the assertion prints it whole. The wrong reading is recorded here because it was load-bearing for a while |
| 4 | low | question #23 turned out to be two questions wearing one coat: "does a transfer hurt the video" (no — measurably not) and "does a transfer survive" (often not). Measuring the first is what exposed the second | #23 closed with numbers, #28 opened with evidence. Worth remembering that an open question can hide a defect rather than a decision |
| 5 | low | the services the tunnel reaches live in sidecars sharing the robot's network namespace rather than in the agent's image. That is a design decision about the rig, not plumbing: it mirrors a real robot, where sshd is the integrator's package and `fjarr-agent` never knows about it, and it keeps ROS 2 and an ssh daemon out of the container whose job is to be the agent | recorded in docs/17 and docs/27 so 4.5d follows the same shape for ROS 2 |

## Measured

| Case | Result |
|---|---|
| `ssh` to the robot over the link | a shell in ~130 ms, 3 of 3, landing in the robot's own namespace (`addr=172.18.0.7`) |
| 1 GiB `scp`, sha256 verified end to end | 25.4 s / 338 Mbps alone; 36.3-45.0 s / 191-236 Mbps beside a streaming camera |
| The camera during a saturating transfer (#23) | 31-33 fps against a 32-33 fps idle baseline, largest stamp step 1 (nothing lost), 0 undecodable stamps, longest gap 55-70 ms against 48-51 ms idle |
| Robot's counters across a full 1 GiB transfer | `tx_packets` 876993, `tx_bytes` 1.12 GB, every drop counter 0 |
| The stall (#28) | 4 of 10 at 64 MiB, 2 of 5 at 1 GiB; stops after 7.5k-79k packets; `ssh` gives up on keepalives at ~20 s; the control channel sometimes survives and answers `close`, sometimes does not |

## Gate (docs/17 slice 4.5c)

1. **`ssh` login over the link** — met, and asserted in CI.
2. **Hash-verified 1 GiB `scp`** — met when it completes, integrity included; **its reliability is not met**, see #28. Recorded in CI rather than asserted, deliberately and visibly.
3. **Question #23 measured** — met, and closed with numbers.

## Deferred

- **#28 needs SCTP-level evidence.** `GST_DEBUG=sctp*:6`, usrsctp association counters, and the channel's `buffered_amount` sampled during a stall would say whether the transport accepted and discarded, or the peer stopped reading. Black-box runs have given all they can.
- **The second robot exists and registers (`demo-robot-02`) but nothing uses it yet**; the isolation regression it is there for lands in 4.5e with the real operator client.
- **The lab's ssh key is baked into the image** and published through a volume. Fine for a fixture, but it means a rebuilt image invalidates a running operator's copy — `tun-up` recreates both, so it self-heals, and this is noted only so the next confusing "permission denied" is short.
