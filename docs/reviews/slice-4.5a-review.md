---
title: "Slice 4.5a Review"
description: Retrospective review of the network tunnel's agent side — the core's inability to route stream-class data, fjarr.net's packet pump and policy rules, the extension API's second amendment, and the four spec corrections the implementation forced.
---

> Retrospective review of slice 4.5a per docs/13 and docs/20, run after the
> slice landed on `main` (88e6fa5) with every gate green. The slice makes
> `fjarr.net` real on the robot: an operator with a `net` claim gets a
> routable IP address for one robot, carried on
> `fjarr:stream:fjarr.net`. The operator end is still `fjarr-opsim`, not
> `fjarr-connect`, which is 4.5b. One reviewer pass over the C++, the lab
> rig and the specs, plus what the gates found.

## What the harness found

| Found by | Defect | Fix |
|---|---|---|
| reading `on_channel_data` before writing anything | the core could not route inbound stream-class data at all: it cut every binary label at `fjarr:bulk:`'s length, so `fjarr:stream:fjarr.net` became `:fjarr.net`, matched no capability, and the packet was dropped and counted — the tunnel's first packet would have vanished with a counter as the only evidence | one `protocol::parse_binary_label` for both classes, unit-tested against the exact off-by-two; a `StreamSender` so a capability can also *send* on the class it declared |
| the first opsim tunnel run | no `fjarr:stream:fjarr.net` channel existed at all: the simulator's minted grant hard-coded `capabilities: [fjarr.test]`, so `fjarr.net` was never granted and never attached | the grant's claims are an option, and the `tunnel` scenario adds `fjarr.net` — the same shape as `relay-only` setting `ice_policy` |
| the second opsim tunnel run | `GET /healthz -> 404`: the introspection endpoint has no such route | the assertion uses `/stats`. The 404 was itself the proof that IP crossed the tunnel and came back |
| `make tun-up`, first run | the robot had no interface: `tun-up` created the device and *then* recreated the container with the tunnel enabled, which took the device's network namespace with it | the target enables the robot first and creates the device second, and the robot's command waits for the interface before starting the agent — the real ordering, made visible |
| `make e2e` after a tunnel run | `faults.spec.ts` ("the agent is killed mid-session… streams again after a restart") hung: the restarted container came back without the device and waited 60 s for one that could never appear | the wait is bounded at 20 s and then starts anyway, saying that a container restart takes its network namespace with it, which a real robot does not do |
| ASan/UBSan (`make agent-test-asan`) | ten new tests failed on `reference binding to null pointer of type 'const SourceFactory'` — `*static_cast<const SourceFactory*>(nullptr)` to satisfy a parameter `configure()` ignores, copied into the daemon's `--net-address` path too | the tests use a real `SourceRegistry` as the terminal's do; the daemon uses a new static `address_from_config`, which is what that code path actually wanted |
| `make fmt` (the docs/13 gate) | the target reformatted **115 files**: the agent's C++ is hand-written, has lines past 250 characters and has no `.clang-format`, so clang-format ran with its LLVM defaults | reverted; the C++ line is gone from `fmt` (335dd7b) with the reason recorded above it. Machine-formatting the C++ needs a config that matches the code and a commit of its own |

## Reviewer findings

| # | Severity | Finding | Resolution |
|---|---|---|---|
| 1 | medium | docs/08 said `open` answers `error{code:"forbidden"}` when the grant carries no `net` claim. It cannot: the core answers `capability-denied` before the capability is attached, so the request never arrives. This is the **same invention corrected for `fjarr.terminal` in M2** — twice now, which makes it a pattern: a capability spec that names an authorization error for its own request is describing something the core already handled | docs/08's row rewritten around what the capability can actually answer (`unavailable`, `busy`) and what the core answers first. Worth checking the remaining capability specs for the same sentence |
| 2 | medium | docs/27 put the robot's settings in a top-level `[net]` table, which is not how any capability is configured — the core validates `[capabilities.X]` against X's own schema (docs/23) | `[capabilities."fjarr.net"]`, with docs/10's two references corrected to match. The operator-side `[net]` in `~/.config/fjarr/config.toml` is `fjarr-connect`'s own file and stays |
| 3 | medium | the plan said "the bounded tail-dropping queue", but the spec forbids a queue outright — a queue delivers a burst of stale packets after congestion. There is no queue in the capability at all | the channel's own buffered amount is the bound: `send_binary` refuses above the watermark and the packet is dropped and counted. The unit test asserts the stronger property — that what was refused is *gone*, and the next accepted packet is the new one |
| 4 | medium | `link-stats` is specified per second **while the link is open**, and the extension API had no timer. Driving the event off traffic would have shown nothing on an idle link — which is exactly when a support engineer needs to read the drop counters | `SessionContext::every`, the API's second amendment, after `watch_readable` in M2. Both amendments came from the capability that was *not* the reference implementation, which is the argument docs/17 makes for scheduling unrelated capabilities early |
| 5 | low | docs/27 said packets arriving while no peer is attached "are dropped and counted". They are dropped, but not counted: with no session there is nothing reading the device, so the kernel discards its own queue. Counting them would need an agent-level read loop, and a capability's watches are per-session | spec amended to say what happens: the kernel discards them, and whatever is still queued when a link opens is discarded before the first forwarded packet and counted as `dropped_no_peer`. The alternative — an agent-level hook — is a bigger API change than the counter is worth |
| 6 | low | docs/08 gives the stream class two framings, `raw` and `framed`, but the C++ enum is `BulkFraming{Raw, Blob}`: reusing `Blob` to mean `framed` would put a word on the wire vocabulary that does not exist there | registration refuses a stream channel declaring anything but `Raw`, naming ADR-0018, so the chunker stays unbuilt and nothing silently mis-frames. The enum is renamed when a capability actually picks `framed` |
| 7 | low | the packet pump cannot be tested without a TUN device, and the dev container has neither `/dev/net/tun` nor `CAP_NET_ADMIN` by default | a documented `test_attach_fd` seam (precedent: `Session::test_silence`) pumps a socketpair, so both policy rules, the MTU bound and the tail-drop are unit-tested with no device and no privileges. The lab proves the device path separately |

## Measured

| Case | Result |
|---|---|
| `fjarr-opsim --scenario tunnel` | 8/8. `GET http://100.70.118.224:7381/stats -> 200`, 503 bytes in 1 ms, 5 packets out and 4 in — a real TCP conversation with the robot's own introspection endpoint over its tunnel address |
| Attach | the agent attaches to a device created by someone else, as an unprivileged user with no `CAP_NET_ADMIN`, and the carrier comes up: `attached to the tunnel interface interface=fjarr0 address=100.70.118.224 peer=100.64.0.1 mtu=1280` |
| Derived addressing | `demo-robot-01` → `100.70.118.224`, identical from `fjarr-agent --net-address` and from the running agent, with no allocator and no state anywhere |
| Refusal | three injected packets addressed to `192.168.1.5` never reach the kernel; `link-stats` reports `dropped_policy=4` — the fourth is the operator kernel's own IPv6 traffic on the interface, refused as not-IPv4 (open question #22) |
| Tests | 101/101 under plain, ASan+UBSan+LSan and TSan; RAII gate clean; 20-cycle soak 17/17 with the object census back to baseline |

## Gate (docs/17 slice 4.5a)

1. **IP reaches the robot over a real data channel** — the HTTP 200 above, over `fjarr:stream:fjarr.net` with `ordered=0 max-retransmits=0` as the channel log confirms.
2. **A packet not addressed to the robot's own tunnel address is dropped** — the opsim scenario over the wire, and `test_net.cpp` for both rules in both directions, plus the port allow-list.
3. **The queue tail-drops instead of growing** — `aFullChannelTailDropsInsteadOfGrowingAQueue`, which also asserts that nothing stale survives the congestion.

## Deferred

- **Two-robot isolation** (docs/15 safety class): needs two operator ends, so it lands with `fjarr-connect`. The single-ended half is tested now.
- **The ordering regression** — a DDS participant created while the agent is detached must not advertise the tunnel address, must when attached, and must keep advertising across an agent restart. Nothing in the stack has ROS 2 yet; the spike used throwaway `ros:jazzy-ros-base` containers.
- **`allow_ports`** is implemented and unit-tested but never exercised over a real link; no configuration uses it.
- **Address collision detection** is the operator's job (docs/27#addressing) and lands with it.
- **IPv6 inside the tunnel** (question #22): the kernel emits it on the interface and it is refused as not-IPv4 and counted under `dropped_policy`, which is honest but coarse. A separate counter is only worth adding if the noise ever hides a real refusal.
- **Question #23** — how tunnel traffic and video share one peer connection under congestion — is untouched: nothing has yet sent enough through a link to disturb a camera. 4.5b's 1 GB `scp` is the measurement.
