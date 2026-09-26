---
title: "ADR 0026: multicast crosses the tunnel, on the strength of its source"
---

- **Status**: accepted
- **Date**: 2026-09-26

## Context

[docs/27](../27-network-tunnel.md#isolation) states two rules that both ends
enforce on every packet: the destination must be this end's own tunnel address,
and the source must be the expected peer. They are what makes robot-to-robot
isolation structural rather than configured, and they are cheap to reason about.

The same document states, from the [spike](../../agent/spikes/ros2-tunnel/README.md),
that multicast crosses a point-to-point tunnel natively and that Fast DDS — the
ROS 2 default — needs no configuration to work over one.

**Those claims contradict each other, and implementing them proved it.** A
multicast destination (DDS discovery uses `239.255.0.1`) is never this end's own
tunnel address, so the destination rule drops every discovery announcement. With
the direct path between the two ends removed, `ros2 topic list` saw nothing, and
the robot's log said exactly why:

```text
inbound packet refused reason=wrong-destination dst=239.255.0.1 src=100.64.0.1
inbound packet refused reason=wrong-destination dst=224.0.0.22  src=100.64.0.1
```

The spike never hit this because its throwaway pump forwarded whatever it read
with no policy at all. The rule is ours, and so is the breakage: ROS 2 over the
link is one of the tunnel's headline reasons to exist ([docs/00](../00-vision.md)),
and as specified it could not work.

## Options considered

**Require unicast DDS configuration on both ends.** Fast DDS accepts an initial
peers list; Cyclone already needs a file ([docs/27](../27-network-tunnel.md#ros2)).
This keeps the destination rule absolute. But it makes "point it at a robot and
your ROS tooling works" false for every DDS implementation, turns the documented
Cyclone file from a workaround into a requirement, and asks every integrator to
edit DDS XML before `ros2 topic list` works once. It also does not help anything
else that uses multicast, which on a robot is more than DDS (mDNS, LLDP-style
discovery, anything a vendor ships).

**Relay multicast in userspace.** The spike wrote such a relay, then measured that
it was unnecessary and deleted it from the design. Re-adding it would put a
component back that the spike's own finding removed — and it would have to know
the RTPS port arithmetic, so it would carry DDS and nothing else.

**Let multicast through on the strength of its source.** Keep the source rule,
drop the destination rule for multicast destinations only.

## Decision

**A packet with a multicast destination (224.0.0.0/4) is accepted if and only if
its source is the expected peer.** Both ends apply it, symmetrically, and the
source rule is unchanged for everything else.

What this does not change:

- **Nothing is forwarded.** IP forwarding stays off, so a multicast datagram
  reaches the robot's own stack and no further. It cannot enter the robot's LAN.
- **Robot A still cannot reach robot B.** The source rule is what enforced that,
  and multicast from anyone but the peer is refused — there is a test for exactly
  that case.
- **The blast radius is not widened in practice.** The tunnel already grants the
  operator every port the robot binds on its tunnel address ([docs/10](../10-security.md#network-tunnel)).
  Multicast from that same operator is strictly less reach than that already is.

## Consequences

Stock Fast DDS works over the link with no configuration, as docs/27 always
claimed — measured after this change: `ros2 topic list` finds the robot's topic
and `ros2 topic echo` receives a sample, with the direct path between the two
ends blocked so the tunnel is the only route.

The destination rule is no longer absolute, which costs a sentence of explanation
wherever it is described, and `dropped_policy` no longer implies "not addressed
to us". The rule that carries the isolation guarantee is now the source rule, and
it should be the one anyone changing this code is most careful with.

Cyclone DDS still wants its documented file: multicast reaching it is necessary
but, per the spike, not sufficient, because it binds one interface arbitrarily.
That file is still owed (docs/27#ros2).
