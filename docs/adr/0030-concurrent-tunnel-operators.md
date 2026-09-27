---
title: "ADR 0030: Several operators on one robot's tunnel, from a small pool of operator addresses"
---

- **Status**: proposed (2026-09-28). Decided that tunnels are concurrent; the
  addressing below awaits review, and is implemented in a slice after M3.
- **Date**: 2026-09-28
- **Supersedes**: — (amends [docs/27](../27-network-tunnel.md#addressing)'s
  single operator address; [ADR-0023](0023-network-tunnel-virtual-interface.md)
  stands)

## Context

[docs/10](../10-security.md#session-ownership) now says terminal, tunnel and
files are never exclusive: two engineers may each have a link to the same robot.
The tunnel cannot do that today, and for a structural reason, not a policy
one. The robot's `fjarr0` is point-to-point with a single peer, and every
operator is that peer: the operator address is fixed at `100.64.0.1` on every
link ([docs/27](../27-network-tunnel.md#addressing)). Two operators at once would
be two hosts with one address. A reply from the robot would have no way to say
which of them it is for.

The fixed address was chosen on purpose, and the reason still binds: **a
robot's DDS configuration names its peers literally**. The documented Cyclone
file lists `100.64.0.1` as a unicast peer, and the ordering rule (ADR-0023)
wants every address and route in place before the robot's ROS stack starts.
Whatever replaces the single address must be knowable at boot, and small enough
to list.

## Options considered

1. **Keep one link per robot** and refuse the second as `busy`, naming the
   holder. Nothing to build, but it contradicts docs/10's rule, and two
   engineers supporting one robot is a normal day.
2. **One shared operator address, rewritten on the robot** (a NAT in the pump).
   Operators keep `100.64.0.1`, and the robot rewrites sources per link. This
   breaks DDS: RTPS carries locator addresses *inside* its payload, so the
   robot's participants would still answer `100.64.0.1`. It also needs
   connection tracking for TCP port collisions. Rejected.
3. **An operator address derived from the operator's identity** across a large
   range, the way robot addresses are derived. It needs no allocation, but DDS
   configurations cannot list an unbounded set of peers, and two operators can
   collide. Rejected.
4. **A tunnel device per concurrent operator on the robot** (`fjarr0`,
   `fjarr1`, …). Routing is simple, but every device must exist at boot for
   DDS to bind it, and every DDS interface allow-list grows with it. Close,
   but heavier than the next option for the same result.
5. **A small pool of operator addresses on one device, assigned per link by
   the robot.** Chosen, below.

## Decision

**A pool of operator addresses: `100.64.0.1` to `100.64.0.N`,** inside the
`/24` docs/27 already reserves for fixed roles. `N` defaults to 8 (config
`operator_slots`).

- **At boot**, the installer gives `fjarr0` the robot's own /32 plus a /32
  route for **every** slot, so every address and route the robot will ever use
  exists before ROS starts. The ordering rule holds unchanged. The device stops
  being point-to-point with one peer; the policy rules (destination self,
  source a slot) and ADR-0026's multicast rule cover the pool.
- **At `open`**, the robot assigns the lowest free slot and returns it in the
  existing `peer_address` field of the result
  ([docs/08](../08-protocol.md#net-packets)), so no wire shape changes. When
  every slot is taken, the answer is `busy`, naming who holds them.
- **The operator** (`fjarr-connect`) adds its slot address for that robot to
  its interface, and routes the robot's /32 with that address as the source.
  An operator attached to two robots may hold different slots on each; an
  interface with several addresses and per-route sources handles that, still
  in-process over netlink (docs/27).
- **The pump** sends a unicast packet to the link that owns its destination
  slot, and a multicast packet to every open link. On the way in, it accepts
  only packets from a link's own slot address to the robot or to multicast. So
  **operators cannot reach each other** through the robot, just as robots
  cannot reach each other through the operator.
- **DDS.** Fast DDS discovers through multicast, which now fans out to every
  open link. The documented Cyclone file lists the pool as its peers, `N`
  addresses instead of one. A slot with no link costs one periodic discovery
  packet, which the pump counts as having no link, separately from real drops.

**Compatibility.** Slot 1 is `100.64.0.1`. A single operator sees exactly
today's addresses, and an existing Cyclone file naming only `100.64.0.1` keeps
working for whoever holds slot 1. docs/27 will say so.

## Consequences

- docs/27's addressing table, the installer's routes, the Cyclone file and
  `net setup` change when this is implemented. So do the lab's gates: two
  operators attached to one robot at once, each reaching the robot, neither
  reaching the other, and ROS discovery working for both.
- Robot-side services see a distinct source address per operator, so their own
  logs can tell engineers apart. The link audit records which operator held
  which slot.
- The pool size is a real limit. Raising it costs one route, one policy entry
  and one Cyclone peer per slot. Revisit if a deployment needs more than a
  handful of simultaneous engineers per robot, which is not support work any
  more.
