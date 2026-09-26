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

Concurrent access policy (from the fleet-daemon lesson, generalized):

- Multiple *viewers* are fine (FrameHub exists for this).
- **Input-bearing** capabilities (desktop input, teleop, terminal) take an
  ownership claim: default policy one owner at a time, later owners read-only
  until transfer.
- Claims are leases: refreshed by the heartbeat, **fail open on staleness**
  (30 s without refresh clears the claim, logged). A dead process must never
  leave a robot unownable — the media plane is the component most likely to
  hang.
- A claim is keyed on the **operator identity in the grant**, not on the
  session: several sessions of the same operator (one per browser window in
  the desktop [presentation mode](22-remote-desktop-client.md#presentation-mode)
  fallback) share one claim and all may send input; a different operator
  is read-only until transfer.

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
  ├─ desktop backend   session user integration per ADR-0006
  └─ fjarr-inputd      SEPARATE minimal binary, owns /dev/uinput (or XTest),
                       speaks a 5-verb protocol over a mode-0700 unix socket:
                       key / button / motion / wheel / release_all
```

The privileged surface is auditable in one sitting (< 500 lines target). It
validates ranges, rate-limits, and refuses when no session claim exists.

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
- **Input-bearing**, so it takes the ownership lease like desktop input.
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
