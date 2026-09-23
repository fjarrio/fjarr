---
title: Network Tunnel
description: fjarr.net — a session-scoped, point-to-point IP link between one operator machine and one robot, so ssh, scp, UDP bridges and ROS 2 tooling work against a remote robot without a capability per tool.
---

`fjarr.net` gives an authorized operator a **routable IP address for one
robot**, carried inside the WebRTC session that already carries its video.
The tools a developer already owns then work unchanged: `ssh`, `scp`,
`rsync`, a UDP bridge dumping a CAN bus, `ros2 topic list`, a browser
pointed at a diagnostic page the robot serves on localhost.

It exists because the alternative is a capability per tool, forever. Every
robot company has a list of things they occasionally need to do to a robot
that nobody will ever write a protocol for. This is the escape hatch that
keeps that list from becoming a roadmap.

## What it is, and what it is not

It is a **link**, not a network. One operator, one robot, one path,
existing only while an authorized session is open.

There is no shared address space, no membership, no device-to-device
routing, no policy language, no coordination service holding a graph of who
may reach whom, and no second identity system. Those are the properties of
a mesh VPN, and Fjarr deliberately does not have them:

- **One authorization path.** A company already decides who may watch robot
  42's camera, in their backend, with their SSO
  ([ADR-0015](adr/0015-backend-integration-strategy.md)). The tunnel is the
  same decision, carried by the same grant, in the same audit record. A
  general VPN brings a second identity and policy system to operate beside
  the one that already governs the robot, and the two eventually disagree.
- **Robots are leaves, structurally.** Robot A must never reach robot B. In
  a mesh product that is a policy you configure, and therefore a policy you
  can misconfigure. Here every link terminates at the operator and the
  packet pump forwards nothing between links, so isolation costs nothing to
  maintain and cannot be switched off by accident ([below](#isolation)).
- **A capability, not a daemon.** It ships inside the product the company
  already deploys. No second agent to install, package, update or license
  per device, and nothing extra to explain to their customer's IT team.
- **The session is the unit, not the device.** The link is up while a
  session is open and gone when it closes. Access is time-boxed by a token
  that already expires. A mesh VPN's purpose is to make a device
  permanently reachable; that is the opposite of what a support tool wants.
- **It rides the path that already works.** NAT traversal, relay fallback,
  reconnection and congestion behaviour are solved once, for the video and
  the tunnel alike. A separate VPN duplicates all of it and then fails
  independently, which is strictly worse to support than one connection
  that is either up or down.

The scope is **developer and support access**, plus machine-to-machine
traffic a developer deliberately points at it. It is not the production
data plane: production traffic belongs in a capability with a declared wire
shape, backpressure and an audit story ([docs/05](05-extension-model.md)).

## The shape

```text
operator host                                    robot
┌───────────────────────────┐                   ┌──────────────────────┐
│ ros2 / ssh / scp / can2udp│                   │ sshd, ROS 2, …       │
│            ↓ kernel       │                   │        ↑ kernel      │
│ fjarr0  100.64.0.1        │                   │ fjarr0  100.x.y.z/32 │
│            ↓              │                   │        ↑  peer .0.1  │
│ fjarr-connect  ──── fjarr:stream:fjarr.net ──── fjarr-agent          │
└───────────────────────────┘   (unreliable,    └──────────────────────┘
                                 unordered)
```

Each end owns one TUN interface. The robot's is point-to-point with exactly
one peer, the operator. The operator's carries a **/32 route per attached
robot**, so `fjarr-connect` is a router with one destination per link and no
path between them.

One interface on the operator host, not one per robot, is deliberate: routes
come and go as robots attach, while the interface and its address never
change, so a long-running ROS 2 node on the developer's machine keeps
working as robots come and go ([lifecycle](#lifecycle)).

## Addressing {#addressing}

The default range is **`100.64.0.0/10`**, the carrier-grade NAT block. It is
reserved, and unlike `10.0.0.0/8` it is almost never used inside a company
network.

| Address | Role |
|---|---|
| `100.64.0.1` | the operator host, the same on every link |
| `100.64.0.0/24` | reserved for future fixed roles |
| everything above | robot addresses |

The operator's address is **fixed and well known** so that a robot's DDS
configuration can name one peer literally, forever, whoever connects.

A robot's address is **derived deterministically from its robot id** —
masked from a SHA-256 of the id into the range, skipping the reserved /24.
No allocator, no state in the signaling server, and the robot knows its own
address at boot without talking to anyone. `net.address` in `fjarr.toml`
pins it when derivation is not wanted.

Neither end ever adds a route for the whole range. The robot adds its own
/32 and a /32 for the operator; the operator adds a /32 per attached robot.
A robot whose cellular carrier hands it a `100.64.0.0/10` WAN address
therefore still routes normally, unless the carrier hands out one of those
exact two addresses. `fjarr-agent --check` reports an overlap between the
configured range and an existing route, and `net.range` changes it.

Two robots that derive the same address cannot be attached at once. The
operator detects the collision, refuses the second link, names both robots
and prints the `net.address` line that fixes it.

## Lifecycle, and the ordering rule {#lifecycle}

**The interface is persistent and exists before the robot's software
starts.** This is the hard constraint of the whole design, and it comes from
measurement rather than taste. DDS binds its interfaces when a participant
is created; a tunnel that appears later is invisible to everything already
running, which on a robot is everything.

A throwaway probe (2026-09-23, `agent/spikes/ros2-tunnel/`) measured the
behaviour of a persistent TUN device:

| Agent state | Carrier | Tunnel address advertised by Fast DDS |
|---|---|---|
| device up, agent not attached | down | no, across 18 announcements |
| agent attached | up | yes |
| agent attached, then exits | down | yes — the bound participant keeps it |

Three rules follow:

1. **The installer creates the device**, once, owned by the `fjarr` user
   (`ip tuntap add … user fjarr`), with its address and MTU set
   ([docs/26](26-robot-install-and-drivers.md)). The agent then attaches to
   it as an unprivileged user with **no `CAP_NET_ADMIN` at all** — measured.
2. **The agent's systemd unit orders before the robot's software.** Carrier
   is down while nothing is attached, and a participant created then ignores
   the interface permanently. This is an ordering dependency, not a nicety.
3. **Agent restarts are safe.** The device and its address survive the agent
   exiting, and participants that bound earlier keep advertising the tunnel
   address straight through the restart. No ROS 2 restart is needed to
   upgrade the agent.

The operator side has no equivalent constraint, because a developer starts
`ros2` after connecting — and the single-interface design
([above](#the-shape)) removes it for the case where they do not.

## The packet path

Packets ride `fjarr:stream:fjarr.net` with `raw` framing
([docs/08](08-protocol.md#datachannel-topology)): **one SCTP message is one
IP packet**, no header, unordered and never retransmitted. TCP inside the
tunnel does its own recovery; an outer retransmission would fight it and
lose on a bad link.

- **MTU 1280**, fixed and configurable. It is the IPv6 minimum, a
  well-tested floor, and leaves roughly 80 bytes of UDP, DTLS and SCTP
  headroom under a 1500-byte path, so the tunnel survives relay and other
  tunnels without inner fragmentation. It is not adaptive: the interface's
  MTU is fixed at creation and everything binds to it.
- **Bounded queue, tail-drop.** The pump respects the channel's buffered
  amount and drops when it is full, counting the drop. It never grows a
  queue: a queue would deliver a burst of stale packets after congestion,
  which ruins TCP's round-trip estimate and confuses DDS more than loss does.
- **No buffering while no peer is attached.** The robot's carrier is up
  whenever the agent runs, so the kernel will hand the agent packets with
  nobody to send them to. They are dropped and counted, never held.

## Policy and isolation {#isolation}

Both pumps enforce the same two rules on every packet, in userspace, where
they cannot be switched off by a sysctl:

- **Destination must be this end's own tunnel address.** On the robot this
  makes lateral movement into the robot's LAN impossible without changing
  Fjarr's code. IP forwarding is never enabled.
- **Source must be the expected peer.** On the operator host a packet
  arriving on robot A's channel is accepted only if it is from A and
  addressed to the operator. Nothing is ever forwarded from one link to
  another, so robot A cannot reach robot B, and neither learns the other
  exists.

`net.allow_ports` optionally narrows the robot side to a port list. The
default is every port on the robot's own tunnel address, because the
motivating use cases need dynamic ports, and because a grant that must be
edited for every tool is a grant nobody uses.

**Granting `net` is equivalent to granting network access to the robot from
inside.** Anything the robot binds becomes reachable to the operator. It is
as consequential as [`fjarr.terminal`](06-capabilities.md), is off by
default, requires an explicit capability claim in the session grant, and is
audited at open and close — see [docs/10](10-security.md#network-tunnel).

## The operator client

A browser cannot create a network interface, so this is Fjarr's **first
non-browser operator**: `fjarr-connect`, a small native binary in the
`fjarr-tools` package.

```console
$ fjarr-connect robot-42
robot-42  100.66.18.203  mtu 1280  up in 1.2 s
$ ssh robot@100.66.18.203
```

It speaks the ordinary signaling and session protocol and uses **data
channels only, no media**, which is why it needs no GStreamer and ships as
one static binary ([ADR-0024](adr/0024-native-operator-client.md)). It
requires `CAP_NET_ADMIN` to create its interface and add routes, granted by
`setcap` at install or by running it under `sudo`, and uses no other
privilege.

## ROS 2 over the link {#ros2}

Measured in the same probe, with all direct traffic between the two ends
blocked so DDS could only reach the peer through the tunnel:

- **Multicast crosses a point-to-point tunnel natively.** A multicast
  datagram is an ordinary IP packet and the peer's kernel delivers it to
  sockets that joined the group on that interface. No relay, no discovery
  server, nothing on the robot.
- **Fast DDS, the ROS 2 default, needs no configuration**, on the default
  domain and on a set one. Discovery completed in 1.1–1.3 s.
- **Cyclone DDS needs a configuration file**, and with it serves the tunnel
  and the local network at the same time. Stock, it binds one interface
  chosen arbitrarily and usually the wrong one. Two elements are both
  required: `<Interfaces>` with explicit `priority` values, which stops the
  arbitrary choice, and unicast `<Peers>`, which supplies the discovery that
  multicast cannot carry across a point-to-point link. The exact file ships
  in the docs, and `fjarr-agent setup` offers to write it.

Topic-name collisions between two robots attached at once are a ROS 2
concern, solved with namespaces or distinct domain ids. Fjarr does not
rewrite traffic.

## Degradation

From the probe, over the network profiles in [docs/25](25-browser-lab.md):

| Profile | Round trip | ssh login | ROS 2 discovery |
|---|---|---|---|
| lan | 0.4 ms | 0.33 s | 1.0 s |
| 4g | 79 ms | 1.2 s | 1.1 s |
| lossy, 5 % loss | 62 ms | 1.0 s | 2.6 s |
| bad, 15 % loss | 212 ms | 14.4 s | did not complete |

`scp` ran at 366 Mbps unimpaired, so the userspace packet path is not the
limit. On the `bad` profile ssh still logs in, slowly, while DDS discovery
does not finish — its handshakes are reliable exchanges that 15 % loss
defeats. That is a property of DDS, not of the tunnel, and the honest
statement is that ROS 2 tooling needs a usable link while `ssh` tolerates a
bad one.

## Configuration

```toml
[net]
enabled = false              # off unless explicitly turned on
interface = "fjarr0"
range = "100.64.0.0/10"      # both ends must agree
address = "auto"             # derived from the robot id; pin to override
mtu = 1280
allow_ports = []             # empty = every port on this robot's own address
```

## Testing {#testing}

Per [docs/15](15-testing-strategy.md):

- **Unit**: address derivation and collision detection; the two policy rules
  rejecting wrong source and wrong destination; tail-drop at the queue
  bound; MTU enforcement.
- **Isolation regression** (safety class, never removed): two robots
  attached at once, robot A cannot reach robot B in either direction, and no
  packet crosses between links.
- **Ordering regression**: a participant created while the agent is detached
  does not advertise the tunnel address; created while attached, it does;
  and it keeps advertising across an agent restart. These three are the
  measured facts the whole lifecycle rests on.
- **Integration**: the probe's rig promoted onto a real data channel — ssh
  login, a 1 GB `scp` verified by hash, `ros2 topic list` with Fast DDS
  unconfigured and with the documented Cyclone file, under each docs/25
  network profile.

## Open questions

Tracked in [docs/18](18-open-questions.md): IPv6 inside the tunnel, how
tunnel traffic and video should share one peer connection under congestion,
Windows support for `fjarr-connect`, and whether a forwarded-socket mode is
ever worth adding for hosts that cannot provide a TUN device.
