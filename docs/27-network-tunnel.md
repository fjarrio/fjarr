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
address at boot without talking to anyone. `address` in the capability's
[config](#configuration) pins it when derivation is not wanted, and
`fjarr-agent --net-address` prints whichever applies.

Neither end ever adds a route for the whole range. The robot adds its own
/32 and a /32 for the operator; the operator adds a /32 per attached robot.
A robot whose cellular carrier hands it a `100.64.0.0/10` WAN address
therefore still routes normally, unless the carrier hands out one of those
exact two addresses. `fjarr-agent --check` reports an overlap between the
configured range and an existing route, and `range` changes it.

Two robots that derive the same address cannot be attached at once. The
operator detects the collision, refuses the second link, names both robots
and prints the `address` line that fixes it.

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

- **MTU 1280**, fixed and configurable. It is the IPv6 minimum and a
  well-tested floor. UDP, DTLS and SCTP add roughly 80 bytes, so a 1280-byte
  inner packet sits well inside a 1500-byte path and keeps sitting there
  behind a relay or someone else's tunnel, without inner fragmentation. It
  is not adaptive: the interface's MTU is fixed at creation and everything
  binds to it.
- **Bounded queue, tail-drop.** The pump respects the channel's buffered
  amount and drops when it is full, counting the drop. It never grows a
  queue: a queue would deliver a burst of stale packets after congestion,
  which ruins TCP's round-trip estimate and confuses DDS more than loss does.
- **No buffering while no peer is attached.** The robot's carrier is up
  whenever the agent runs, so something may address the tunnel with no
  operator on the other end. Nothing reads the device between links, so the
  kernel discards what it queued; whatever is still queued when a link opens
  is discarded before the first forwarded packet and counted as
  `dropped_no_peer`, so a link never begins by delivering stale traffic.
  Packets that arrive on the channel for a session with no open link are
  counted the same way.

## Policy and isolation {#isolation}

Both pumps enforce the same two rules on every packet, in userspace, where
they cannot be switched off by a sysctl:

- **Destination must be this end's own tunnel address**, or a multicast address
  ([ADR-0026](adr/0026-multicast-over-the-tunnel.md)). On the robot this makes
  lateral movement into the robot's LAN impossible without changing Fjarr's code.
  IP forwarding is never enabled. The multicast exception exists because DDS
  discovery is multicast and a multicast destination can never be this end's own
  address, so the rule as first written made ROS 2 over the link impossible —
  measured, with the robot logging `refused reason=wrong-destination
  dst=239.255.0.1`. Such a packet still reaches only this end's own stack.
- **Source must be the expected peer.** On the operator host a packet
  arriving on robot A's channel is accepted only if it is from A and
  addressed to the operator. Nothing is ever forwarded from one link to
  another, so robot A cannot reach robot B, and neither learns the other
  exists. **This is the rule that carries the isolation guarantee** — the
  destination rule has an exception and this one does not, so it is the one to be
  careful with.

`allow_ports` optionally narrows the robot side to a port list. A packet
carrying no port to read — ICMP, a later fragment — cannot satisfy a port
list and does not get a free pass through one. The
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

It speaks the ordinary signaling and session protocol and uses **data
channels only, no media**, which is why it needs no GStreamer and ships as
one static binary ([ADR-0024](adr/0024-native-operator-client.md)). It
requires `CAP_NET_ADMIN` to create its interface and add routes, granted by
`setcap` at install or by running it under `sudo`, and uses no other
privilege.

## Finding a robot, and who authorizes it {#discovery}

`fjarr-connect` is a dashboard without a screen. It discovers and authorizes
the way the browser does, so the tunnel adds **no new trust boundary and no
second source of truth**.

| Party | Owns | What the CLI asks it |
|---|---|---|
| The customer's backend | their users, which humans may reach which robots, the fleet list, and presence from the `robot.online` / `robot.offline` webhooks it already receives | "what may I reach" and "mint me a grant for this one" |
| `fjarr-server` (sidecar or Cloud) | grant verification, signaling relay, TURN credentials, session events, metering | nothing extra: it presents the grant and starts a session, exactly as a browser does |
| `fjarr-connect` | one tunnel interface and its routes | — |

**The CLI never holds the tenant API token and never calls the control-plane
REST API** ([docs/09](09-interfaces.md#c-control-plane-rest-customer-backend--fjarr-server)).
That token is a fleet-wide administrative credential. It belongs on a server,
not on a laptop that travels.

The robot list comes from the customer's backend rather than from
`fjarr-server`, even though the server also knows who is connected. Only the
customer's backend knows **who you are**, so only it can return the robots
*you* may reach instead of the whole tenant's fleet. That scoping is the
property worth having, and it is not ours to compute.

### The operator API

Two endpoints, both thin wrappers over code the integrator already has
([docs/09](09-interfaces.md#operator-api)):

| Endpoint | Returns |
|---|---|
| `GET /fjarr/robots` | the robots this human may reach: `robot_id`, `label`, `status`, `last_seen` |
| `POST /fjarr/grants` | a grant for one `robot_id` — the same JWT their dashboard already mints, with whatever capabilities their policy allows |

Their fleet table answers the first, the webhooks they already receive supply
`status`, and the second is the grant-minting code from
[docs/09](09-interfaces.md#a-session-grants-customer-backend--operator-client).

For anyone who would rather add nothing, the CLI's own config may instead
name a command that prints a grant on stdout, and `fjarr-connect --grant
<jwt>` accepts one directly — which is what the first day of any integration
looks like:

```toml
# ~/.config/fjarr/config.toml — the operator's machine, not the robot
[backend]
url = "https://fleet.acme.com"
grant_command = "acme-cli fjarr-grant --robot {robot}"
```

That path works everywhere and costs the integrator nothing, at the honest
price of losing the list and the picker.

### Logging in

A browser dashboard holds a logged-in session; a shell holds nothing. So
`fjarr-connect login` hands off to the browser, the way familiar developer
CLIs do:

1. The CLI opens a listener bound **only to loopback** on a random port and
   opens the customer's dashboard with a callback URL and a random `state`.
2. The page — already inside their authenticated app — posts a credential
   for the signed-in user back to the callback, showing which port it is
   handing to.
3. The CLI checks `state`, caches the credential at mode 0600 under
   `~/.config/fjarr/`, and closes the listener.

The page itself ships as a drop-in `@fjarr/react` component
([docs/21](21-web-client-architecture.md#cli-login)), so the integrator's work
is mounting a route rather than building a flow.

**With no browser to open** — a workstation reached over ssh, which in
robotics is the common case, not the exception — the CLI prints a URL and a
short code, you approve it wherever a browser exists, and the CLI polls until
you do.

Credential lifetime is the **customer's** choice, because it is their
identity system. Fjarr does not dictate it and does not refresh it; expiry
means running `login` again.

### What it feels like

```console
$ fjarr-connect login https://fleet.acme.com
opening browser… approved as anna@acme.com · 12 robots reachable

$ fjarr-connect list
ROBOT      LABEL               STATUS   LAST SEEN
robot-024  Packer 3 · Malmo    online   -
robot-031  Packer 4 · Malmo    offline  3h ago
robot-112  Mower A · Lund      online   -

$ fjarr-connect
? which robot  (type to filter, enter to connect)
> robot-024  Packer 3 · Malmo   online
  robot-112  Mower A · Lund     online
  robot-031  Packer 4 · Malmo   offline

$ fjarr-connect robot-024
robot-024  100.66.18.203  mtu 1280  up in 1.2 s  via relay
  ssh robot@100.66.18.203
^C  link closed · 41 MB up / 3 MB down
```

The argument is optional. Without one you get the picker; with one it
connects; with several it attaches several robots at once, which is the case
the [interface layout](#the-shape) was designed for. The filter matches the
label as well as the id, so `fjarr-connect packer3` works and nobody
memorises identifiers.

**A link lives in the terminal that started it.** Ctrl-C ends it, and
closing the window ends it. For a grant this consequential
([docs/10](10-security.md#network-tunnel)), a link that cannot outlive the
window you are looking at is the safer default, and there is no daemon to
forget about.

So that this does not mean two terminals for everything, the CLI runs a
command with the link up and tears it down when the command exits, with the
address in its environment:

```sh
fjarr-connect robot-024 -- ssh robot@$FJARR_ADDR
fjarr-connect robot-024 -- ros2 topic list
fjarr-connect robot-024 -- scp robot@$FJARR_ADDR:/var/log/robot.log .
```

One mechanism, no per-tool wrappers, and it composes with anything already
installed.

**Offline robots fail fast and specifically.** The grant is valid, so the
refusal comes from signaling as `robot-offline`
([docs/08](08-protocol.md#errors)), which the server already returns today.
The CLI prints when the robot was last seen instead of waiting on a session
that cannot establish.

**Addresses are stable, so `ssh` config is worth writing once.** A robot's
address is derived from its id ([above](#addressing)) and never changes, so a
`Host packer3` stanza can live in `~/.ssh/config` permanently.
`fjarr-connect list --ssh-config` prints the stanzas to paste.

The customer's audit needs nothing new: a tunnel session raises the same
`session.started` and `session.ended` webhooks as a camera session, with
`fjarr.net` among the grant's capabilities.

## ROS 2 over the link {#ros2}

Measured in the same probe, with all direct traffic between the two ends
blocked so DDS could only reach the peer through the tunnel:

- **Multicast crosses a point-to-point tunnel natively.** A multicast
  datagram is an ordinary IP packet and the peer's kernel delivers it to
  sockets that joined the group on that interface. No relay, no discovery
  server, nothing on the robot.
- **Fast DDS, the ROS 2 default, needs no configuration**, on the default
  domain and on a set one. Discovery completed in 1.1–1.3 s. Confirmed over a
  real data channel in slice 4.5d — `ros2 topic list` finds the robot's topic and
  `ros2 topic echo` receives a sample, with every direct path between the two ends
  blocked — but **only after [ADR-0026](adr/0026-multicast-over-the-tunnel.md)**:
  the isolation rule as first written dropped every discovery announcement,
  because a multicast destination is never this end's own tunnel address. The
  spike missed it because its throwaway pump applied no policy at all.
- **Cyclone DDS needs a configuration file**, and with it serves the tunnel
  and the local network at the same time. Stock, it binds one interface
  chosen arbitrarily and usually the wrong one. Two elements are both
  required: `<Interfaces>` with explicit `priority` values, which stops the
  arbitrary choice, and unicast `<Peers>`, which supplies the discovery that
  multicast cannot carry across a point-to-point link. The exact file ships
  in the docs, and `fjarr-agent net setup` offers to write it
  ([docs/26](26-robot-install-and-drivers.md)).

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
limit. Over a real data channel (slice 4.5c) a 1 GiB `scp` runs at **338 Mbps**
on its own and **190-236 Mbps beside a streaming camera**, and the camera is
unaffected either way — 31-33 fps, no lost frames, longest gap 55-70 ms against
a 48-51 ms idle baseline. That closes [question #23](18-open-questions.md): the
tunnel and the video share one peer connection without a separate one for bulk.

**A bulk transfer stalls in roughly half of attempts**, at the onset of the
flow. `ssh` and small requests over the same link never stall. It is
[question #28](18-open-questions.md), it is independent of video and of transfer
size, and it has to be settled before the M4.5 gate can claim a reliable `scp`.

What is known: **usrsctp abandons messages and does not say so where the sender
can see it.** `SCTP_SEND_FAILED_EVENT` arrives asynchronously, after
`send_binary` has returned true and with `buffered_amount` at 0 — so the
tail-drop rule above, which watches the buffered amount, is blind to the one
failure that matters. Any design that treats "the send returned true" as
delivery, or the buffered amount as the only measure of pressure, is building on
that blind spot. The fault is also rate-sensitive enough that logging the SCTP
layer suppresses it entirely, which is worth knowing before measuring it. On the `bad` profile ssh still logs in, slowly, while DDS discovery
does not finish — its handshakes are reliable exchanges that 15 % loss
defeats. That is a property of DDS, not of the tunnel, and the honest
statement is that ROS 2 tooling needs a usable link while `ssh` tolerates a
bad one.

## Configuration {#configuration}

On the robot, in `fjarr.toml`, as an ordinary capability table validated
against the capability's own schema ([docs/23](23-agent-core-architecture.md#configuration)):

```toml
[capabilities."fjarr.net"]
enabled = false              # off unless explicitly turned on
interface = "fjarr0"         # attached to, never created (see the ordering rule above)
range = "100.64.0.0/10"      # both ends must agree
address = "auto"             # derived from the robot id; pin to override
mtu = 1280                   # the interface's own MTU wins if they disagree
allow_ports = []             # empty = every port on this robot's own address
```

`fjarr-agent --net-address` prints the address this robot will use, derived or
pinned, so the installer can create the interface with it before the agent
runs.

On the operator's machine, in `~/.config/fjarr/config.toml`. The cached
credential from `login` lives beside it at mode 0600 and is never written
here:

```toml
[backend]
url = "https://fleet.acme.com"   # where the operator API lives
# grant_command = "…"            # the escape hatch instead of the operator API

[net]
interface = "fjarr0"
range = "100.64.0.0/10"          # must match the robots'
address = "100.64.0.1"           # this machine, the same on every link
```

## Testing {#testing}

Per [docs/15](15-testing-strategy.md):

- **Unit** (slice 4.5a, `agent/tests/test_net.cpp`): address derivation; the
  two policy rules rejecting wrong source and wrong destination, in both
  directions; the port allow-list, including that a packet with no port to
  read does not pass one; tail-drop at the queue bound, and that what was
  dropped is gone rather than queued; MTU enforcement. The packet pump runs
  against a socketpair, so the rules are tested with no device and no
  privileges. Collision detection is the operator's, and lands with it.
- **End to end** (slice 4.5a, `fjarr-opsim --scenario tunnel`): the simulator
  attaches to its own persistent interface and pumps real IP over
  `fjarr:stream:fjarr.net` — an HTTP request to the robot's own introspection
  endpoint on its tunnel address, answered over the link, plus a packet
  addressed into the robot's LAN that the robot refuses and counts. `make
  tun-up` creates both interfaces the way the installer will, before the
  agent starts.
- **Isolation regression** (safety class, never removed): two robots
  attached at once, robot A cannot reach robot B in either direction, and no
  packet crosses between links. Needs two ends, so it lands with
  `fjarr-connect` in 4.5e.
- **Ordering regression**: a participant created while the agent is detached
  does not advertise the tunnel address; created while attached, it does;
  and it keeps advertising across an agent restart. These three are the
  measured facts the whole lifecycle rests on.
- **Integration** (slice 4.5c for the shell and the transfer): the probe's rig
  promoted onto a real data channel. `ssh` and `scp` reach the robot through
  sidecars sharing its network namespace — which is what a robot looks like from
  the far end of a link, sshd being the integrator's package and not the
  agent's — and `fjarr-opsim --scenario tunnel --exec <cmd>` runs them with the
  link up and `FJARR_ADDR` set, the same shape as `fjarr-connect <robot> --
  <cmd>`. `make tunnel-ssh` asserts the shell; `make tunnel-scp` pulls the
  payload and verifies its sha256, and records rather than asserts while
  [#28](18-open-questions.md) stands.
- **ROS 2** (slice 4.5d): `make tunnel-ros` runs `ros2 topic list` and
  `ros2 topic echo` from a sidecar on the operator's namespace against one on the
  robot's, with `docker/lab/dds-isolate.sh` removing every direct path between the
  two containers first — without that the two would discover each other over the
  lab's own bridge and the test would prove nothing. The session uses the relay
  (`--ice-policy relay`) because the direct path is exactly what was blocked. The
  documented Cyclone file and `fjarr-agent net setup`'s offer to write it are
  still owed, as are the docs/25 network profiles.

## Open questions

Tracked in [docs/18](18-open-questions.md) as #22–#26: IPv6 inside the
tunnel, how tunnel traffic and video should share one peer connection under
congestion, Windows support for `fjarr-connect`, whether a forwarded-socket
mode is ever worth adding for hosts that cannot provide a TUN device, and
what to do if a design partner's cellular carrier hands out WAN addresses
inside the default range ([above](#addressing)).
