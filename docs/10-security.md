---
title: Security
description: Threat model, device identity, session tokens, TURN credentials, privilege separation, and license policy.
---

Fjarr grants remote eyes, hands, a shell, and (later) software installation
on machines that move in the physical world. Security is architecture here,
not review.

## Threat model (condensed STRIDE)

Assets: robot control/input injection, camera/desktop imagery, customer
files, fleet metadata, update artifacts (M8 — the supply-chain crown jewel).

| Threat | Vector | Mitigation |
|---|---|---|
| Spoofed robot | stolen/guessed device identity | per-device credentials from enrollment; no fleet-shared secrets ([below](#device-identity)) |
| Spoofed operator | leaked/forged grant | short-lived signed JWTs, capability-scoped, tenant-keyed; server verifies before the agent ever hears about a session |
| Media interception | on-path attacker | DTLS-SRTP end-to-end (relay forwards ciphertext); WSS for signaling |
| TURN abuse | harvested credentials | ephemeral HMAC creds (`use-auth-secret`), TTL ≤ session; never static ([teleop-car lesson](11-prior-art.md#teleop-car)) |
| Robot-side privilege escalation | compromise of the agent process | agent runs unprivileged; injection helper is minimal and separate ([ADR-0009](adr/0009-privilege-separation.md)); **no arbitrary-shell escape hatches, ever** (fleet-daemon FIFO anti-lesson) |
| Terminal/file abuse | over-broad grants | per-capability grant params (`view_only`, `read`/`write`, path allow-lists); audit every session |
| Wire tap leakage | a host page enabling the `@fjarr/core` wire tap sees every envelope, including desktop keystrokes and clipboard text | opt-in per client, never enabled by the library or `@fjarr/react`, documented as a diagnostics-only switch ([docs/21](21-web-client-architecture.md#wire-tap)) |
| Internals disclosure | pipeline graphs reveal device paths, encoder settings, session ids | `fjarr.introspect` only with an explicit grant (developer/support roles); the local endpoint binds to loopback unless a token is configured; snapshots never contain credentials ([docs/24](24-pipeline-introspection.md)) |
| Replay/tamper on webhooks | forged callbacks | HMAC-signed, timestamped, `event_id` idempotency |
| Stuck control | dead operator/agent | heartbeat teardown + `release_all_input()`; deadman on actuation channels ([docs/15](15-testing-strategy.md)) |

## Device identity {#device-identity}

Explicitly rejecting the prior art (MAC address + fleet-shared bearer token):

1. **Enrollment**: `PUT /v1/robots/{robot_id}` returns a one-time bootstrap
   token; the agent redeems it for a **per-device credential** (Ed25519
   keypair generated on-device, public key registered; private key
   `0600`, TPM-backed where available — [open question](18-open-questions.md)).
2. **Authentication**: agent `hello` signs a server nonce; no long-lived
   bearer tokens on the wire.
3. **Revocation**: single API call kills a device's access; rotation
   supported without touching the robot (re-enroll flow).
4. The customer's `robot_id` is carried in signed messages, not only in
   connection metadata, so audit trails survive proxies.

## Session authorization

Covered by the grant contract ([docs/09](09-interfaces.md#2-backend-tier--the-integration-contract-adr-0015)).
Security-relevant rules:

- Grants authorize **session creation**, not indefinitely: ≤ 5 min validity
  to start; server may cap session duration per tenant policy.
- The agent re-checks capability names against its local config — a grant
  can never enable a capability the integrator didn't compile/configure in.
- `session.started`/`ended` webhooks + agent-side audit log give the fleet
  admin the "who/what/when" view; terminal sessions additionally log
  start/end with operator identity from day one.

## Session ownership {#session-ownership}

Concurrent access policy (decided 2026-09-28, answering
[#31](18-open-questions.md)). The rule: **people who could fight over the same
thing take turns; everyone else works side by side.**

- **Viewers never block anyone.** Any number of sessions may watch any track
  (FrameHub exists for this). Watching claims nothing.
- **Control domains.** An input-bearing capability declares a `control_domain`
  in its manifest ([docs/09](09-interfaces.md)). Each domain has at most one
  holder, and the domains are independent. Someone on the desktop, someone
  driving and someone in a terminal all work at once.

  | Domain | Members | One holder because | Handover |
  |---|---|---|---|
  | `desktop` | `fjarr.desktop` input | two people moving one pointer is chaos | **5 s after the holder's last input** control is free, and the next to type or move the pointer takes it; any operator may also **take control** at once, and the holder is told |
  | `motion` | teleop; `fjarr.test`'s drive in the demo | two people steering one robot is dangerous | **never on idle.** Only `release-control`, disconnect, or another operator's **take control**, which first stops the robot: the capability's `release_all_input` runs for the old holder before the new holder's first command is accepted, so nobody inherits a robot in motion |
  | none | `fjarr.terminal`, `fjarr.net`, `fjarr.files` | nothing is shared: each session has its own pty, link or transfer | not applicable. Concurrent and audited. (Concurrent *links* need per-operator addresses, [ADR-0030](adr/0030-concurrent-tunnel-operators.md), accepted; until it is built, a second link is refused as `busy`, naming who holds the first) |

- **A claim is taken on engagement, not on session open.** It happens at the
  session's first input in that domain (a pointer or key event, a drive
  command) or at an explicit `take-control`
  ([docs/08](08-protocol.md#fjarr-core)). Opening a session with an
  input-bearing grant claims nothing.
- **`view_only: true` in a grant** means that session can never claim or take
  control, whatever its client does.
- **The clipboard.** Writing the robot's clipboard is input in the `desktop`
  domain: it claims, and a non-holder is answered `control-held`. Reading it
  claims nothing, but a `view_only` session gets no offers and its reads are
  `capability-denied`. Clipboard text is often a password: a viewer is given
  the screen, not what the robot copied.
- A claim is keyed on the **operator identity in the grant**, not on the
  session: several sessions of the same operator (one per browser window in
  the desktop [presentation mode](22-remote-desktop-client.md#presentation-mode)
  fallback) share one claim and all may send input.
- **Input from a non-holder** is dropped and counted (events), or answered
  `control-held`, naming the holder and since when (requests). Every session
  is told who holds each domain whenever that changes (`control-state`), so a
  client can show "Anna has been controlling the desktop for 3 min".
- Claims still **fail open on staleness**: a holder with no live session, or
  with no heartbeat for 30 s, loses the claim (logged). A session closed to be
  retried (a media or ICE restart) keeps its operator's claim across the gap,
  still bounded by the 30 s. A dead process must
  never leave a robot uncontrollable.

**What the lease does not cover.** A terminal or a tunnel can move a robot
outside the `motion` domain, for example `ros2 topic pub /cmd_vel` over the
link. Two things stand between that and harm: the robot's own motion
interlocks, and the customer's choice of who gets terminal and tunnel grants.
The lease is about operators not fighting over a shared input. It is not a
motion-safety system.

## TURN

coturn in `use-auth-secret` mode only. `fjarr-server` mints
`username = expiry:session_id`, `credential = HMAC-SHA1(secret, username)`
with TTL bound to the grant. Relay usage is metered per session (billing +
abuse detection). The dev compose ships this exact mode so no code path ever
sees a static TURN password.

## Robot-side privilege layout ([ADR-0009](adr/0009-privilege-separation.md))

```text
fjarr-agent            unprivileged user "fjarr"
  ├─ media plane       same user; GPU via render group
  ├─ desktop backend   agent-side module; the desktop is reached through
  │                    fjarr-desktop-session (ADR-0028), which runs as the
  │                    desktop user and hands over PipeWire + EIS descriptors
  └─ fjarr-inputd      SEPARATE minimal binary, owns /dev/uinput (or XTest),
                       speaks a 5-verb protocol over a mode-0700 unix socket:
                       key / button / motion / wheel / release_all
```

The privileged surface is auditable in one sitting (< 500 lines target). It
validates ranges, rate-limits, and refuses when no session claim exists.

**On GNOME, the desktop user's account is the boundary**
([ADR-0006](adr/0006-desktop-backend-selection.md)). Mutter's `RemoteDesktop`
and `ScreenCast` interfaces ask for no consent from a process on the session
bus. The portal's consent is a record in the user's permission store, which the
same user can write. So anything running as the auto-login user can watch and
drive the screen.

The agent is therefore **not** that user
([ADR-0028](adr/0028-desktop-session-helper.md)). A small helper,
`fjarr-desktop-session`, runs in the session as the desktop user. It talks to
mutter or the portal, and hands the agent a PipeWire descriptor for capture and
an EIS descriptor for input over `/run/fjarr/desktop.sock`. The agent accepts
that connection only from the configured desktop user's uid. There are three
reasons, in order of weight:

1. **Embedding.** libfjarr runs inside the customer's software, under the
   customer's service account. That account has to reach the desktop from the
   outside anyway, and the helper is how.
2. **The device key.** It stays `0600` under the agent's account. If the agent
   ran as the desktop user, every app in the desktop session (a kiosk HMI, a
   browser) could read it and impersonate the robot.
3. **Lifetime.** The desktop session is not durable: the spikes saw it end on a
   GNOME crash. The agent, and with it the camera, terminal and tunnel, must
   outlive it.

The installer creates the auto-login account without a password (auto-login
needs none) and without remote login. None of the chosen backends needs
`fjarr-inputd`. On X11 kiosks, the session grants the agent's account with
`xhost +si:localuser:<agent user>` instead of sharing an authority file.

## Network tunnel ([docs/27](27-network-tunnel.md)) {#network-tunnel}

`fjarr.net` gives an operator a routable address for one robot. **Granting
it is equivalent to granting network access to the robot from inside**:
anything the robot binds becomes reachable, including services that assume
they are only reachable on localhost or on a trusted LAN. It is the most
consequential grant in the catalog alongside `fjarr.terminal`, and it is
treated as such:

- **Off by default.** Nothing is attached unless `capabilities."fjarr.net".enabled` is set.
- **Explicit claim.** The session grant must carry `net` among its
  capabilities ([ADR-0015](adr/0015-backend-integration-strategy.md)); the
  agent refuses `open` otherwise. The customer's backend decides who gets
  it, with the identity system that already governs the robot.
- **Audited** at open and close, with the session, the operator identity and
  the byte counters — the same treatment the terminal gets.
- **No lateral movement.** Both ends drop any packet not addressed to their
  own tunnel address, in userspace. IP forwarding is never enabled, so the
  link reaches the robot and not the network behind it. An operator holding
  two links cannot route between them, and the robots cannot see each other.
- **Multicast from the peer is accepted** even though its destination is not the
  robot's own tunnel address ([ADR-0026](adr/0026-multicast-over-the-tunnel.md)):
  DDS discovery is multicast, and the rule as first written made ROS 2 over the
  link impossible. It reaches only the robot's own stack — forwarding is off — and
  it is strictly less reach than the operator already has, since the grant exposes
  every port the robot binds on that address. The source check is unchanged, so a
  second robot's announcements are still refused.
- **Optional port allow-list** (`capabilities."fjarr.net".allow_ports`) for deployments that want
  the surface narrower than "this robot's own address".
- **No privilege gain.** The device is created once at install and the agent
  attaches to it as the unprivileged `fjarr` user with no `CAP_NET_ADMIN`
  (measured — [docs/27](27-network-tunnel.md#lifecycle)).

The residual risk is honest and documented rather than engineered away: an
operator with `net` can reach a robot's internal services. The control is
who gets the claim, for how long, and the audit record afterwards.

## Terminal ([docs/06](06-capabilities.md)) {#terminal}

A shell on the robot, as the account the integrator names. With the network
tunnel it shares the top of the risk table, and the same treatment:

- **Off unless configured**, and configuration means naming a user. There is
  no default account, because defaulting to the agent's own user would be a
  security decision taken by omission — it is chosen for running the agent,
  not for being a shell anyone should have.
- **The named user is verified, never assumed.** The agent is unprivileged
  and cannot switch accounts, so a configured user it is not running as makes
  the capability `unavailable` and says so. The failure mode this removes is
  the quiet one: a robot that was meant to give a restricted shell handing out
  the agent's own instead.
- **Explicit claim** in the session grant, refused otherwise.
- **Audited** at open and close with the operator identity, and an I/O
  recording hook for deployments that need the transcript.
- **Input-bearing** but in no control domain: every session has its own pty,
  so terminals run side by side ([session ownership](#session-ownership)).
- **No orphan shells**: the pty is closed by `release_all_input` on any
  session end, which is a safety behaviour with a regression test
  ([docs/15](15-testing-strategy.md#safety-behaviors)), not a best effort.

What the account can do is the integrator's decision and the whole of the
policy. Fjarr does not sandbox the shell, and says so rather than implying a
containment it does not provide.

## Update integrity (forward reference, M8)

OTA artifacts are signed (SWUpdate signing + our manifest); agents verify
before flashing; A/B + health-check rollback bounds the blast radius.
Detailed in the M8 spec revision of this document.

## Dependency & license policy ([ADR-0011](adr/0011-license-open-core.md))

- Shipped artifacts: permissive or LGPL-dynamic dependencies only; **no GPL**
  (`x264enc` is the canonical example — doctor enforces its absence).
  Rationale: dual-licensed commercial builds cannot carry GPL code.
- Every dependency is recorded with its license in [docs/14](14-dependencies.md);
  additions require a docs/14 row in the same PR.
- External contributions require a CLA (needed for dual licensing).

## Secrets hygiene

No secrets in the repo, images, or frontend bundles — `.env` (gitignored)
locally, secret stores in deployment. The teleop-car frontend shipping TURN
passwords as build-time defaults is the canonical anti-example.
