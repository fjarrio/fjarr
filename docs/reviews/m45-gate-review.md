---
title: "M4.5 Gate Review"
description: The network tunnel's milestone gate, promise by promise — every claim made through fjarr-connect on the relay path, three product defects the gate run itself found, the Cyclone criterion amended out, and what M4.5 carries forward.
---

> Gate review for M4.5 (docs/17), run 2026-09-27 as `make m45-gate` on the
> host, after slices 4.5a–4.5f. Every claim is made through **`fjarr-connect`,
> the product**, with the client relay-only and the direct path between the
> containers removed — the lab's stand-in for a robot behind carrier NAT. One
> criterion was amended out rather than met, and the reason is below.

## The gate, promise by promise

| Promise (docs/06, docs/17) | Verdict | Evidence |
|---|---|---|
| a link up in under 3 s, and `ssh` logs in | met | 0.1 s direct, 1.6 s relay-only (TURN allocation included); `tunnel-ssh-connect` |
| a hash-verified 1 GB `scp` to a robot behind carrier NAT | met | `tunnel-scp-connect`, 5 of 5 over TURN at 268–304 Mbps; the gate asserts that no host candidate was offered, so a pass cannot be the docker bridge |
| `ros2 topic list` with Fast DDS unconfigured | met — after a fix | `tunnel-ros-connect`: list and echo over the relay. It failed first; finding 1 |
| `ros2 topic list` with the documented Cyclone file | **amended out** | [#29](../18-open-questions.md): the file fixes Cyclone's hang, but discovery does not complete across the link. 4.5d measured that as a ROS 2 integration question, not a tunnel one, so docs/06 and docs/17 now state it as #29 where the criterion is stated |
| login through the dashboard, then list, pick and connect with nobody typing a robot id; the same with no browser | met | `tunnel-login` (4.5f), in CI; the code shape end to end, the loopback shape in halves ([docs/15](../15-testing-strategy.md)) |
| a second robot attached at once is unreachable from the first, in both directions | met | `tunnel-isolation`, with a route forced into each robot's tunnel before a negative is believed |
| the agent upgraded without the robot's ROS stack restarting | met | `tunnel-ros-ordering` — facts A, B and C, nightly in CI |
| every open and close audit-logged | met | `tunnel-login` correlates `session.started` (naming `fjarr.net`) with its `session.ended` by `session_id` |

## What the gate run found

| # | Severity | Finding | Resolution |
|---|---|---|---|
| 1 | **high** | **`fjarr-connect` dropped multicast.** The pump chose a link by destination, and DDS discovery is addressed to a group, so ROS 2 had never worked through the product — only through opsim, whose single-link pump forwards everything. ADR-0026 had admitted multicast at the policy layer; the routing layer above it had not heard | the pump fans multicast out to every link, from the operator only, never link to link; `route_for` is a pure function with a test |
| 2 | medium | `fjarr-connect -- <cmd>` exited 0 whatever the command did, so a failed check read green — which is how the ROS gate reported `PASS` on its first run while printing that the topic was missing | the command's exit status is the CLI's, once the links are down. The gate fixture already keyed its verdict on that exit code — it was right and the client under it was not |
| 3 | medium | `tun-up ROS=1` created the robot's ROS participant before the agent had attached, so the participant never saw the tunnel — docs/27's fact A, violated by the lab that tests it | the participant is created last |
| 4 | medium | a `docker compose up` from inside `dev` hands the host daemon `/workspace` as the bind source, and every recreated service came up with an empty workspace | recreating targets run on the host and `m45-gate` refuses to run in `dev`; CLAUDE.md and docs/12 |

Findings 1 and 2 are the pattern this milestone kept teaching: the second
implementation of a rule (opsim's pump, then the client's) is where the rule
goes missing, and a check whose failure cannot reach the verdict passes.

## Carried forward

- **[#28](../18-open-questions.md)** — a usrsctp-to-usrsctp association wedges
  under load with video beside it. The failing operator is the lab's opsim;
  neither real operator is usrsctp, and the gate's `scp` claim is made through
  `fjarr-connect`. Next: read both ends of a wedged association.
- **[#29](../18-open-questions.md)** — Cyclone DDS, when a design partner uses it.
- **[#31](../18-open-questions.md)** — whether a view-only session should take the input lease.
- macOS is written and type-checked in CI, and has never run on macOS hardware
  ([docs/04](../04-supported-platforms.md)).
