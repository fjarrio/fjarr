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
| `100.64.0.1` | the operator host, the same on every link (one link at a time today; several operators from a small address pool is [ADR-0030](adr/0030-concurrent-tunnel-operators.md), proposed) |
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

All three are a regression now rather than a paragraph — `make
tunnel-ros-ordering` (slice 4.5d) reproduces them against real participants on
both ends of a real link, including the negative one: a participant created while
the agent is detached stays blind to the interface afterwards, however long it
runs. That is the fact the installer's job and the unit's ordering rest on, and
before 4.5d nothing had re-checked it since the spike.

The operator side has no equivalent constraint, and that is now measured
rather than argued: the operator's ROS 2 daemon, started while the link was
down, still found the robot once the link came up, with Fast DDS and with
Cyclone (slice 4.5e's follow-up).

## The packet path

Packets ride `fjarr:stream:fjarr.net` with `raw` framing
([docs/08](08-protocol.md#datachannel-topology)): **one SCTP message is one
IP packet**, no header, unordered and never retransmitted. TCP inside the
tunnel does its own recovery; an outer retransmission would fight it and
lose on a bad link.

- **MTU 1184: one SCTP chunk** ([ADR-0027](adr/0027-tunnel-mtu-one-sctp-chunk.md)).
  Every WebRTC stack runs SCTP with a 1200-byte path MTU, so the largest packet
  that travels as a single DATA chunk is 1200 − 16. At the 1280 this document
  used to specify — sized against the UDP path, never against SCTP's own — every
  full-size packet was a two-chunk message on a channel that never retransmits,
  and a usrsctp receiver wedged in about half of all bulk transfers
  ([#28](18-open-questions.md), measured as a one-byte edge). It is not
  adaptive: the interface's MTU is fixed at creation and everything binds to it,
  and an agent attached to a larger device says so in its log. IPv6 inside the
  tunnel ([#22](18-open-questions.md)) would need 1280, and with it a larger
  SCTP path MTU.
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
`setcap cap_net_admin+ep` at install or by running it under `sudo`, and uses no
other privilege. Every such change is made **in the client's own process**, over
netlink: a file capability is not inherited by a child process, so a client that
shelled out to `ip` would work under `sudo` and fail under `setcap` with
`Operation not permitted`. Doing it in-process also means the binary needs no
`iproute2` on the host, which is the point of shipping one static file.

An interface it creates is **persistent and owned by the user who created it**
(`TUNSETPERSIST`, `TUNSETOWNER`), for the same reason the robot's is: the interface
and its address outliving any one link is what lets a long-running ROS 2 node keep
working ([above](#the-shape)), and it means every later run attaches with no
privilege at all.

A grant names one robot
([docs/09](09-interfaces.md#a-session-grants-customer-backend--operator-client)), so
attaching several robots takes `--grant` once per robot in the same order, until
`login` fetches them per robot ([below](#discovery)).

Its ICE matches the rest of the stack rather than the usual desktop default.
Candidates **trickle in both directions**: the agent's offer advertises
`a=ice-options:trickle` and carries no candidate lines, so a client that waits to
gather before answering leaves the agent with no path to check, and neither end
connects. There is **no STUN server by default** — host candidates plus the
session's minted TURN credentials are the path that needs no third party, the
same starting point as the agent and `@fjarr/core`. An operator who wants a
server-reflexive candidate passes `--stun <url>` (repeatable, or `FJARR_STUN`);
baking a public STUN server into the binary would send every operator's address
to a server the customer never chose. `--relay-only` offers nothing but TURN
candidates, for a network that forbids direct UDP — and for the lab, where it is
how a robot behind carrier NAT is stood in for ([testing](#testing)).

### `fjarr-connect shell` — the robot's terminal in yours {#shell}

**Planned (decided 2026-09-27): the native client's next small step.**
`fjarr-connect shell robot-024` opens the robot's
[`fjarr.terminal`](08-protocol.md#terminal) pty in the terminal the command runs
in (GNOME Terminal, xterm, any other). It needs no tunnel and no `sshd` on the
robot.

- **Only the terminal channel.** It opens a session whose grant carries
  `fjarr.terminal` and no `fjarr.net`. So it needs no interface and no
  `CAP_NET_ADMIN`, and it runs the same on macOS and Windows as on Linux.
- **The existing protocol, unchanged.** `open` with the local terminal's size
  and `$TERM`; `resize` on every `SIGWINCH`, coalesced; `close` on exit. The
  shell's `exit` event becomes `fjarr-connect`'s own exit status, so it
  composes in scripts. A `busy` or `unavailable` answer, or a grant without the
  terminal (`capability-denied`), is printed as the reason and exits non-zero.
- **The local terminal is restored on every way out**: the shell exiting, the
  link dropping, a signal. It is in raw mode while attached, so a client that
  leaks raw mode leaves the operator's own terminal unusable.
- **Grants and audit as in the browser.** The terminal is input-bearing, so it
  takes the ownership lease; every open and close is audited. `login` asks the
  operator API for a grant that carries `fjarr.terminal`, which the operator is
  given only if their backend allows it.

ssh over the link remains for what the ssh ecosystem brings (VS Code
Remote-SSH, rsync, port forwarding). `shell` is for the operator who wants a
shell on the robot and nothing else to install or open on it.

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
robotics is the common case, not the exception — the CLI asks the backend for a
code (`POST /fjarr/cli-codes`, [docs/09](09-interfaces.md#operator-api)), prints
the dashboard's login URL and the code, and polls with a separate poll token until
the human, wherever a browser exists, opens that URL, confirms the code on screen
matches the terminal, and approves it. `login <site>` derives the operator API
as `<site>/api`; a deployment that serves the two from different hosts passes
`--api <url>`, and the CLI remembers both. The credential comes back through the
poll exactly once; the code is single-use and lives ten minutes. `login --code`
forces this path; without the flag the CLI tries to open a browser and falls back
to the code when it cannot.

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
robot-024  100.66.18.203  mtu 1184  up in 1.2 s  via relay
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
installed. With several robots attached, `FJARR_ADDR` is the first one's address and
`FJARR_ADDRS` carries all of them, space separated, in the order they were
attached.

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
- **Cyclone DDS works with a configuration file** — verified over the real link
  on 2026-09-27 ([#29](18-open-questions.md), closed), and gated nightly. Stock,
  it binds one interface chosen arbitrarily and sees nothing across the link; with
  the file below it passed **22 of 22** through `fjarr-connect`, relay-only with
  the direct path removed, at both MTUs, whether the operator's ROS 2 daemon was
  warm, cold, or started while the link was down. Discovery takes about **0.4 s**
  with a warm daemon and **1.2 s** from cold, the same as Fast DDS.

  Why slice 4.5d called it unverified is worth knowing, because none of it was
  Cyclone: the check asked `ros2 topic list` **once**, the instant the link came
  up, and that command returns what the daemon already knows rather than waiting —
  a race it sometimes lost — and the link then ran at the 1280 MTU that
  [ADR-0027](adr/0027-tunnel-mtu-one-sctp-chunk.md) retired. The check now polls
  and reports how long discovery took.

  **Cyclone does not need the ordering rule** ([above](#lifecycle)). Told which
  interface to use and which peers to talk to, it binds `fjarr0` whether or not
  it has a carrier: a participant created while the agent was detached is visible
  once it attaches, where a Fast DDS one never is. The installer's ordering
  remains right for the ROS 2 default.

  The file, generated per end by `docker/lab/cyclonedds-tunnel.sh`:

  ```xml
  <?xml version="1.0" encoding="UTF-8"?>
  <CycloneDDS xmlns="https://cdds.io/config">
    <Domain id="any">
      <General>
        <AllowMulticast>true</AllowMulticast>
        <Interfaces>
          <NetworkInterface name="fjarr0" priority="10" multicast="true"/>
          <NetworkInterface name="eth0"   priority="1"  multicast="true"/>
        </Interfaces>
      </General>
      <Discovery>
        <ParticipantIndex>auto</ParticipantIndex>
        <Peers>
          <Peer address="100.64.0.1"/>       <!-- the operator, always this address -->
          <Peer address="100.70.118.224"/>   <!-- this robot's derived address -->
        </Peers>
      </Discovery>
    </Domain>
  </CycloneDDS>
  ```

  Both elements are there for different reasons: `<Interfaces>` with explicit
  priorities stops the arbitrary single-interface choice and keeps the local
  network usable at the same time, and the unicast `<Peers>` supply the discovery
  that a point-to-point link's multicast cannot bootstrap. `fjarr-agent net setup`
  can now offer to write it ([docs/26](26-robot-install-and-drivers.md)), and has to
  fill in the robot's real LAN interface name: the file's `eth0` is the lab's, and
  a name that does not exist leaves Cyclone on the tunnel alone.

  **Not verified: Cyclone on the tunnel alone.** Given only `fjarr0` with multicast
  on, Cyclone sends almost nothing — the spike and slice 4.5d both saw it —
  consistent with it treating a point-to-point interface as unable to multicast.
  The spike found a tunnel-only recipe (multicast off, peers by address) that
  worked on its UDP link; it has never run over the real one, so an operator who
  wants DDS on the tunnel and nowhere else is on unmeasured ground.

Topic-name collisions between two robots attached at once are a ROS 2
concern, solved with namespaces or distinct domain ids. Fjarr does not
rewrite traffic.

## Your own remote-desktop client over the link {#byo-remote-desktop}

**Unverified.** This is noted 2026-09-27 and is to be tried on the spike
machine once the agent is installed there (M2.5). The same trial covers the
file-manager equivalent: Nautilus mounting the robot over sftp
(`sftp://<user>@<tunnel address>/`), which needs `sshd` on the robot and
carries the same second-door caveat. Fjarr's own answer for files is the
WebDAV drive of [ADR-0029](adr/0029-robot-files-as-a-webdav-drive.md).

An engineer who already lives in Remmina, or in the Windows Remote Desktop
client, can reach the robot's desktop over the link like any other TCP service.
Run an RDP server on the robot (GNOME's own gnome-remote-desktop, port 3389) or
a VNC server on an X11 kiosk (`x11vnc`, port 5900). Bring the link up with
`fjarr-connect`, and point the client at the robot's tunnel address. ssh and
scp work this way already. This needs nothing from Fjarr but the link.

It is a pattern Fjarr documents, not a feature it ships, and it has costs the
engineer should see up front:

- **It is a second door.** The RDP or VNC server has its own password. Fjarr's
  grants, input lease, audit record and view-only mode govern the link, not
  what happens inside it. Granting the link already means granting network
  access to the robot ([docs/10](10-security.md#network-tunnel)); an RDP server
  makes that access a full desktop.
- **It fights the appliance setup.** gnome-remote-desktop keeps its credentials
  in the login keyring. On an auto-login robot there is none until one is
  created, and creating it puts a dialog on the robot's screen
  ([docs/07](07-desktop-backends.md#decision-2026-09-27)).
- **It is not `fjarr.desktop`.** The native capability needs no client install,
  rides Fjarr's grants and adaptive bitrate, and brings local cursor,
  multi-monitor presentation and the audit record. The link carries whatever
  the RDP or VNC client negotiates, over a path with a 1184-byte MTU
  ([ADR-0027](adr/0027-tunnel-mtu-one-sctp-chunk.md)).

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

`fjarr-connect` measures the same: **246-253 Mbps** for a hash-verified 1 GiB
`scp` (slice 4.5e), against 264 Mbps for `fjarr-opsim` on the same link and
payload. Two implementations of the operator end, one C++ on a GLib loop and one
Rust on tokio, land within 7 % of each other, so the ceiling is the transport
rather than either pump. The client has to be built `--release` before any such
number is believed: unoptimised it manages 53 Mbps, which is the build and not
the design.

**A bulk transfer used to stall in roughly half of attempts**
([question #28](18-open-questions.md), closed). The cause was the MTU: at 1280,
every full-size packet fragmented into two SCTP chunks on this no-retransmit
channel, and a usrsctp receiver wedged — the robot→operator direction of the
association stopping for good while the other kept delivering. The failure
switched on at exactly one byte past a single chunk (1184 green in 10 of 10, 1185
in 4 of 10), and the default is now that chunk ([ADR-0027](adr/0027-tunnel-mtu-one-sctp-chunk.md)):
20 of 20 hash-verified 1 GiB transfers at it, against 5 of 10 at 1280 in the same
session. The lesson worth keeping is how long it hid: it was investigated through
three slices by black-box arms on the SCTP side, and found in an afternoon by
capturing the plaintext IP on both tunnel devices, which touched nothing in the
path it was measuring.

What is known: **usrsctp abandons messages and does not say so where the sender
can see it.** `SCTP_SEND_FAILED_EVENT` arrives asynchronously, after
`send_binary` has returned true and with `buffered_amount` at 0 — so the
tail-drop rule above, which watches the buffered amount, is blind to the one
failure that matters. Any design that treats "the send returned true" as
delivery, or the buffered amount as the only measure of pressure, is building on
that blind spot.

Since slice 4.5e's follow-up it is at least **counted**: GStreamer reports the
event as nothing but a `GST_ERROR` line on the `sctpassociation` category — no
signal, no state change — so the agent installs a log function on that category
at ERROR level and reports the count on `link-stats` as `abandoned`, with
usrsctp's reason as `abandoned_error` ([docs/08](08-protocol.md#datachannel-topology)).
ERROR is level 1 and costs nothing until the event fires. That distinction
matters: `GST_DEBUG=sctp*:3`, which is how the events were first seen, slows the
send path enough to make the fault disappear (8 of 8 passed with it on), so any
instrument on this path has to be checked for changing the result before its
readings are believed. On a partially reliable stream — this channel has
`max-retransmits=0` — an abandonment is what a lost first transmission looks
like, so a trickle of them under loss is the class working as designed; a burst
with the four drop counters at zero is the shape of #28. On the `bad` profile ssh still logs in, slowly, while DDS discovery
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
mtu = 1184                   # one SCTP chunk (ADR-0027); the interface's own MTU wins
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
  privileges. The saturating case is a unit test too: a full batch of 32 reads
  must keep the watch and lose no packet — the path whose missing return value
  aborted the agent in every bulk transfer (slice 4.5e, [#28](18-open-questions.md)).
  The operator end carries the same two rules in Rust (`policy::tests`, slice 4.5e),
  case for case, because an operator that trusted what arrived on a channel would
  carry one robot's packets into another's route.
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
- **Two robots at once** (slice 4.5e, `make tunnel-isolation`, docs/15 safety
  class — never removed): two robots attached to one operator interface, each
  reachable from the operator over its own link, neither reachable from the other.
  The negative half **forces a route into the sending robot's tunnel first** and
  verifies it is there: without that the packet leaves down the robot's default
  route, the attempt times out for a reason that has nothing to do with isolation,
  and the check passes while measuring nothing. What refuses it with the route
  forced is the sending robot's own outbound rule; the operator's mirror of that
  rule cannot be reached by an honest agent, which is why it is a unit test in all
  three implementations rather than a live one.
- **A colliding pair** (slice 4.5e, `make tunnel-collision`): the second robot is
  pinned to the first robot's address on purpose, and the operator refuses the pair
  by name with the `address` line that fixes it, before either link is routed.
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
