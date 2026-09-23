---
title: "Spike: ROS 2 and ssh over a point-to-point tunnel"
description: Does stock ROS 2 discovery work across a virtual network interface carried by a userspace link, and what does it cost? Throwaway probe run 2026-09-23 for the tunnel design discussion.
---

> **Throwaway.** Everything here is a fixture for one question. It is not
> reviewed, not tested, not shipped, and nothing outside this directory
> changed. The output is the findings below.

## The question

Fjarr's peers are browsers today. A developer wants to reach a robot with
the tools that live on their own machine: `ssh`, `scp`, a UDP bridge for a
CAN bus, and `ros2 topic list` against the remote robot. The design fork is
whether the tunnel forwards named sockets (like `ssh -L`) or presents a
**virtual network interface** so the robot has a routable address.

The interface shape is far more capable, and the objection to it was that
ROS 2 discovery uses multicast, which a point-to-point link does not carry.
This probe asked whether that objection is real, for both DDS
implementations, with the domain id left at its default and set.

## The rig

Two `ros:jazzy-ros-base` containers on one docker network, with **all
direct traffic between them dropped** except the two tunnel ports, so DDS
can only reach the peer through the tunnel. Each gets a TUN interface
(`10.77.0.1`, `10.77.0.2`, MTU 1400) and a userspace pump that carries IP
packets as UDP datagrams (`pump.py`). `relay.py` relays the RTPS discovery
multicast group; the port arithmetic is `7400 + 250 × domain`, which is why
the domain id had to be part of the matrix. Impairment is `tc netem` on the
underlying hop, using the profiles from docs/25.

Stage B, replacing the UDP hop with a real Fjarr data channel, was **not
run** — see "What is still unknown".

## Findings

**1. Multicast crosses a point-to-point tunnel natively. The relay is not
needed.** With both relays stopped, Fast DDS still discovered the remote
topic and received data. A multicast datagram is an ordinary IP packet;
the peer's kernel delivers it to sockets that joined the group on that
interface. This contradicts the assumption the design discussion started
from, and it removes a whole component from the design.

**2. Fast DDS works with no configuration at all**, on the default domain
and on domain 42:

| Case | Discovery | Data |
|---|---|---|
| Fast DDS, domain unset | 1.3 s | received |
| Fast DDS, domain 42 | 1.1 s | received |

It binds to every interface and advertises every locator, so the peer finds
the one reachable address by trying them.

**3. Cyclone DDS needs configuration, and with it works over the tunnel and
on the local network at the same time.** Stock Cyclone fails, and its own
log gives the reason:

```
using network interface eth0 (udp/172.19.0.2) selected arbitrarily from: eth0, tun0
```

It binds a **single interface, chosen arbitrarily**, and chose the
unreachable one. Naming only the tunnel does not rescue it either: bound to
`tun0` alone with multicast left on, Cyclone emits **zero packets**,
consistent with it disabling multicast on a point-to-point link. Two
configurations do work:

| Cyclone configuration | Over the tunnel | Local multicast |
|---|---|---|
| stock | no | yes |
| `tun0` only, `AllowMulticast=false`, unicast `Peers` | yes | no |
| both interfaces, no priorities | no | yes |
| both interfaces, explicit `priority`, unicast `Peers` | **yes** | **yes** |

The configuration that gives both is:

```xml
<CycloneDDS><Domain><General>
  <AllowMulticast>true</AllowMulticast>
  <Interfaces>
    <NetworkInterface name="tun0" priority="10" multicast="true"/>
    <NetworkInterface name="eth0" priority="1"  multicast="true"/>
  </Interfaces>
</General><Discovery>
  <ParticipantIndex>auto</ParticipantIndex>
  <Peers><Peer address="10.77.0.1"/><Peer address="10.77.0.2"/></Peers>
</Discovery></Domain></CycloneDDS>
```

Two elements are both required. Listing the interfaces with **priorities**
stops the arbitrary single-interface selection, and the unicast **`Peers`**
supply the discovery that multicast cannot carry across the point-to-point
link. Cyclone is therefore supportable, but only by writing a config file
on the robot, whereas Fast DDS needs nothing.

**4. A DDS stack that is already running does not pick up a tunnel that
appears later.** This is the finding that most shapes the design. With the
publisher started first and the tunnel brought up afterwards, the client
failed to discover it on three consecutive attempts, even with Fast DDS.
Restarting the publisher once the interface existed discovered it
immediately. DDS binds its interfaces when a participant is created, and a
real robot's ROS stack is running long before anyone connects.

**5. ssh and scp work, and the packet path is not a bottleneck.** An
interactive login took 0.33 s and a 20 MB `scp` ran at 366 Mbps through the
userspace pump on an unimpaired link.

**6. Degradation is graceful until it isn't:**

| Profile | Round trip | ssh login | ROS 2 discovery | Data |
|---|---|---|---|---|
| lan | 0.4 ms | 0.33 s | 1.0 s | received |
| 4g | 79 ms | 1.2 s | 1.1 s | received |
| lossy (5 % loss) | 62 ms | 1.0 s | 2.6 s | received |
| bad (15 % loss, 1.5 Mbit) | 212 ms | 14.4 s | not within 20 s | — |

Everything works through `4g` and `lossy`. On `bad`, ssh still logs in
after fourteen seconds but DDS discovery does not complete: its handshakes
are reliable exchanges and 15 % loss defeats them.

## What is still unknown

- **What a Fjarr data channel adds.** The rig used plain UDP. A data
  channel is DTLS and SCTP over the same path: roughly forty more bytes per
  packet, SCTP's own pacing, and the agent's userspace processing. The
  packets must ride an **unreliable, unordered** channel, or inner TCP and
  outer retransmission fight each other on a poor link.
- **Whether a persistent idle interface is acceptable on a robot.** Finding
  4 forces the interface to exist before ROS starts, which means the agent
  creates it at boot and holds it while nobody is connected.
- **Two robots at once.** Only a single link was exercised, and finding 4
  interacts with it: if each robot needs its own interface on the client,
  the client's addresses must be stable across connections too.

## Recommendation

Design the virtual network interface. The objection that motivated the
forwarded-socket fallback — that stock ROS 2 cannot work over a
point-to-point link — is **false for Fast DDS, the ROS 2 default**, and it
needs no relay and no discovery server. Cyclone works too, at the cost of
one config file on the robot.

Carry the design with these constraints, each of which this probe earned:

- The interface is **persistent, created when the agent starts**, not when a
  peer connects. Finding 4 makes a connect-time interface useless to any
  ROS stack that was already running, which on a robot is all of them.
- Its address is **stable**, because the robot's DDS resolves locators at
  participant creation and a Cyclone config file names peers literally.
- Packets ride an **unreliable, unordered** channel.
- A fixed conservative MTU.
- Addresses assigned by the client, since hub-and-spoke needs no central
  allocator.
- No IP forwarding on the robot by default, so the link reaches the robot
  and not the network behind it.
- Cyclone documented with its configuration file, not promised as stock.

## Files

`pump.py` (TUN ↔ UDP), `relay.py` (discovery multicast relay, ultimately
unnecessary), `setup.sh` (one side of the link plus the isolation rules),
`run_case.sh` (one matrix cell), `compose.yml` (the two containers).
