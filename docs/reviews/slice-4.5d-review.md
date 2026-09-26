---
title: "Slice 4.5d Review"
description: Retrospective review of ROS 2 over the tunnel — the isolation rule that made it impossible, the test that first proved nothing, the three lifecycle facts turned into a regression, and Cyclone left honestly unverified.
---

> Retrospective review of slice 4.5d per docs/13 and docs/20, run after the slice
> landed on `main` green (766ee6a). `ros2 topic list` and `ros2 topic echo` now
> reach a robot over the tunnel, the three facts docs/27's lifecycle rule rests on
> are a regression rather than a paragraph, and Cyclone is documented as
> unverified rather than claimed as supported. One item left the slice on purpose:
> [question #29](../18-open-questions.md).

## What the harness found

| Found by | Defect | Fix |
|---|---|---|
| the first `ros2 topic list` over a link | nothing crossed, and the robot said why: `refused reason=wrong-destination dst=239.255.0.1`. **docs/27 contradicted itself** — the isolation rule requires a packet's destination to be this end's own tunnel address, a multicast destination never is, and DDS discovery is multicast. ROS 2 over the link, one of the tunnel's headline reasons to exist, could not work as specified | [ADR-0026](../adr/0026-multicast-over-the-tunnel.md): multicast is admitted on the strength of its source alone, with the source rule now carrying the isolation guarantee by itself. docs/08, docs/10 and docs/27 say so, and a test pins that a second robot's announcements are still refused |
| checking my own isolation | the first version blocked DDS's port range and multicast group, and discovery **kept working** — I was one step from recording "ROS 2 works over the tunnel" on a test that proved nothing. Blocking all direct traffic made the topic vanish, which showed the earlier pass had crossed the lab's own bridge | `dds-isolate.sh` removes every direct path between the two containers, and the tunnel session uses the relay because the direct path is what was removed. The negative control — no link, no topic — is checked before the positive one is believed |
| the ordering regression, first run | it reported facts B and C broken, both of which I had proved by hand minutes earlier: the helper read the *simulator's exit status* rather than the scenario's `exec` verdict, and `pipefail` plus an unrelated timeout assertion made a passing check read as a failing one | the helper reads the verdict and prints the failing assertions whenever it claims no topic, so the next false reading explains itself |
| trying to test "an agent restart keeps participants working" | untestable as the lab stood: restarting the container takes its network namespace and the tunnel device with it, which is exactly what a real robot does not do | the lab agent runs under a small supervisor, so it restarts the way systemd restarts it and re-attaches to a device that was there all along. A hold file keeps it down long enough to create a participant while detached, which is how the negative fact is set up |
| `--force-recreate` on a ROS sidecar | compose recreated `demo-robot` as a dependency, wiping the tunnel device and leaving both sidecars pointing at a container that no longer existed — visible only as "connection refused" from something that should have been listening | `--no-deps` wherever a namespace-sharing sidecar is recreated, and `make tun-up ROS=1` owns the ordering: robot, then device, then sidecars |
| measuring Cyclone instead of trusting the spike | stock Cyclone does not merely choose the wrong interface as the spike reported — it leaves `ros2 topic list` hanging with no output at all | the documented file fixes the hang; discovery still does not complete, so Cyclone is recorded as unverified with the numbers, and `net setup` is not built until the file is known-good |

## Reviewer findings

| # | Severity | Finding | Resolution |
|---|---|---|---|
| 1 | **high** | the spec's two claims about the tunnel — "destination must be this end's own address" and "multicast crosses natively, Fast DDS needs no configuration" — were mutually exclusive, and both had been written down as measured facts. Implementing them is what surfaced it; reading them had not, across two reviews | ADR-0026. The general lesson is narrower than "write better specs": a rule stated as an invariant and a behaviour stated as a measurement should be checked against each other when the same document holds both |
| 2 | medium | I nearly recorded the spike's headline finding as an artifact. The spike ships a multicast relay and a route sending multicast to eth0, which looked like its result had leaked through the bridge; reading further, the README states it measured the finding with both relays **stopped**, so the finding stood and the fault was mine | no change, recorded here. Checking the fixture before doubting the conclusion would have been quicker than the reverse |
| 3 | medium | `dropped_policy` no longer means "not addressed to us" now that multicast is admitted, and the counter is what a support engineer reads first (docs/08) | the reason is already in the log line beside every refusal, and docs/27 says which rule carries the guarantee. Splitting the counter per reason is worth doing if a support case ever hinges on it |
| 4 | low | the Fast DDS profile generator I wrote while hunting the multicast problem was never needed, and its XML was rejected by 2.14.6 anyway | deleted. It existed for twenty minutes and would have been a maintained fixture for a problem that turned out to be ours |
| 5 | low | the lab's ROS image is built rather than pulled, which adds a build to a stack that already builds three | it installs Cyclone, which `ros-base` does not carry, and without Cyclone the slice could only have assumed what docs/27 claims about it. Worth the layer |

## Measured

| Case | Result |
|---|---|
| Fast DDS over the link, stock, direct path removed | `ros2 topic list` finds `/fjarr/robot_heartbeat`, `ros2 topic echo` receives `demo-robot-01`; ~2.5 s end to end, 138 packets out and 79 in on the operator's side |
| The multicast the isolation rule used to drop | `dst=239.255.0.1` (RTPS discovery) and `dst=224.0.0.22` (IGMP membership), both from the peer |
| Lifecycle fact B — participant created while attached | reaches the peer over the link |
| Lifecycle fact C — agent restarted under the supervisor | still reaches it, with no ROS restart and the interface untouched |
| Lifecycle fact A — participant created while detached | never reaches it, which is the negative fact the whole ordering rule exists to respect |
| Cyclone, stock | `ros2 topic list` hangs with no output |
| Cyclone, with the documented file | returns and lists local topics; discovery across the link does not complete, with 78 discovery packets arriving at the robot |

## Gate (docs/17 slice 4.5d)

`ros2 topic list` against the robot with Fast DDS unconfigured: **met**. The
Cyclone file written into docs/27 for the first time: **met**, with its measured
status. The three ordering facts as a regression: **met**. `fjarr-agent net
setup`'s offer to write the file: **deliberately not met**, and moved to
[question #29](../18-open-questions.md) with the reason.

## Deferred

- **Question #29**: Cyclone over the link, and the `net setup` offer that waits on it.
- **The ordering regression is not in CI.** It restarts the agent and recreates participants, which takes a couple of minutes and wants the ROS profile up; it belongs in the nightly rather than every push, and nothing schedules it yet.
- **The `ros` profile is not part of CI at all**, so `tunnel-ros` is a local gate today. It should join the nightly with the ordering regression.
- The lab's ROS participant publishes one topic at 2 Hz. Nothing has measured ROS 2 over the link under the docs/25 network profiles, which is where the spike's `bad`-profile finding (DDS discovery does not complete at 15 % loss) would be re-checked.
