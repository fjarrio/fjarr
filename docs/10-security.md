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
