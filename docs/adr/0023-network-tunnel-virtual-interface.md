---
title: "ADR 0023: The network tunnel is a virtual interface, not forwarded sockets"
---

- **Status**: accepted
- **Date**: 2026-09-23

## Context

Robot companies keep a list of things they occasionally need to do to a
robot that nobody will write a protocol for: log in and look, copy a file,
point a UDP bridge at a CAN bus, run ROS 2 tooling against the robot's
graph. Modelling each as a capability is an unbounded roadmap, and the
tools already exist on the developer's machine.

Two shapes can carry them. **Forwarded sockets** tunnel named ports, the
way `ssh -L` does: the operator gets `localhost:2222` and the agent opens a
TCP connection to port 22 on the robot. **A virtual interface** gives the
robot a routable address on the operator's machine and carries whole IP
packets.

The objection to the interface was that ROS 2 discovery uses multicast,
which a point-to-point link was assumed not to carry, and that an interface
needs privileges a forwarded socket does not.

A throwaway probe on 2026-09-23 (`agent/spikes/ros2-tunnel/`) measured
both assumptions against two DDS implementations over a userspace
TUN-to-UDP link with all direct traffic between the ends blocked.

## Options considered

**Forwarded sockets.** Cheapest and least privileged: no kernel interface,
no address management, no root at any point, and an explicit per-port grant
that is easy to reason about and to audit.

It cannot carry the motivating use cases. DDS discovery is multicast to a
well-known group, and DDS data flows over ports chosen at runtime, so there
is no port list to forward. A CAN-over-UDP bridge is UDP to an arbitrary
port. Forwarding covers `ssh` and `scp` and stops there — and `ssh` alone
can be had without Fjarr.

**A virtual interface.** The probe removed both objections:

| Question | Measured |
|---|---|
| Does multicast cross a point-to-point tunnel? | Yes, natively. No relay, no discovery server. |
| Does stock Fast DDS work over it? | Yes, unconfigured, 1.1–1.3 s discovery |
| Does Cyclone DDS work over it? | Yes, with interface priorities plus unicast peers; serves the LAN at the same time |
| Does the agent need `CAP_NET_ADMIN`? | No. A persistent device created once at install is attached by an unprivileged user. |
| ssh / scp | 0.33 s login, 366 Mbps |

It costs a persistent interface, an address scheme, a boot ordering rule
([docs/27](../27-network-tunnel.md#lifecycle)), and a native operator
client, because a browser cannot create an interface
([ADR-0024](0024-native-operator-client.md)).

**Both.** Rejected as premature: two mechanisms, two policy surfaces and
two sets of failure modes, to serve one use case the interface already
covers.

## Decision

The `fjarr.net` capability presents a **virtual network interface** at each
end, per [docs/27](../27-network-tunnel.md).

The interface is **persistent and created when the agent starts**, not when
a peer connects. This is forced by measurement: DDS binds its interfaces
when a participant is created, so a tunnel that appears at connect time is
invisible to a ROS stack that was already running — which on a robot is all
of them. Three consequences are load-bearing and pinned by regression tests:
the installer creates the device, the agent's unit orders before the robot's
software, and an agent restart does not require a ROS restart.

The operator's address is fixed and the robot's is derived from its id, so
neither end needs an allocator or state in the signaling server. Isolation
between robots is structural — each link terminates at the operator and the
pump forwards nothing — rather than configured.

## Consequences

Fjarr gains a capability whose grant is equivalent to network access to the
robot from inside. It is off by default, needs an explicit claim in the
session grant, and is audited like the terminal
([docs/10](../10-security.md#network-tunnel)).

Fjarr also gains a first non-browser operator and therefore a new shipped
artifact and a second platform matrix, decided in ADR-0024.

The persistent interface is visible to the robot's own software at all
times, so the robot advertises a tunnel address to its LAN peers that they
cannot reach. This costs them a little discovery time, the same way a
container bridge already does on every developer machine.

Cyclone DDS is supported with a documented configuration file rather than
stock, which is a support cost recorded honestly rather than a promise.

Nothing here precludes adding a forwarded-socket mode later on the same
capability for hosts that cannot provide a TUN device; it is listed as an
open question, not built.
