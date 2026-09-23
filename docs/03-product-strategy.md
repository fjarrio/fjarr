---
title: Product Strategy
description: The living SaaS business plan — open-core split, pricing meters, go-to-market.
---

> **Living document.** This is the business plan, versioned next to the code
> and iterated as the product is built. The root README summarizes it. Treat
> numbers as hypotheses with dates, not facts.

## One-line pitch

*Fjarr gives robot companies the remote connectivity stack they keep
rebuilding — embeddable camera streaming, remote desktop, file transfer,
direct developer access and fleet tooling — as open-source libraries plus a
managed cloud.*

## Market hypothesis

Target customer: **robot/IoT product companies (5–200 engineers) without a
dedicated connectivity team** — AMRs, agriculture, inspection, logistics,
outdoor robotics. They all need: live video, remote support access, log/file
retrieval, fleet health, OTA. Today they choose between rebuilding it
(months, fragile — see [prior art](11-prior-art.md)) or stitching together
tools that don't embed (RustDesk/AnyDesk for desktop, an IoT cloud for
telemetry, scp for files).

Analogous positioning: **LiveKit** (media infra, open server + cloud twin),
**Mender/Balena** (fleet OTA), **Temporal** (open core + managed). Fjarr's
differentiator is the *bundle for robots*: real-time media, desktop, files,
and fleet tooling — one protocol, embeddable at every tier — plus a
developer experience no media stack offers: **live pipeline introspection**
([docs/24](24-pipeline-introspection.md)) that lets an integrator, a
support engineer or an AI coding agent see exactly what the media plane is
doing, on the robot and from the dashboard, and an install that puts only
the drivers a robot actually needs on it ([docs/26](26-robot-install-and-drivers.md)).

### Why the network tunnel is not a worse mesh VPN {#tunnel-positioning}

[`fjarr.net`](27-network-tunnel.md) looks superficially like the mesh VPN
products a developer already knows, and the difference decides whether it is
a feature or a liability.

Those products build a **network**: a shared address space where many devices
become mutually reachable, governed by their own identity system, their own
policy language and their own coordination service. Fjarr builds a **link**:
one operator, one robot, one path, alive only while an authorized session is
open.

Five consequences make the narrow thing the better product *for this use
case*, and we should say so plainly in the marketing rather than pretend to
compete on breadth:

1. **One authorization path instead of two.** A robot company already decides
   who may reach robot 42, in their backend, with their SSO. The tunnel is
   that same decision, carried by the same grant and landing in the same
   audit record ([ADR-0015](adr/0015-backend-integration-strategy.md)). A
   general VPN adds a second identity and policy system to operate alongside
   the one that already governs the robot — and the two will disagree, at the
   worst possible moment, about who may touch a machine that moves.
2. **Isolation that cannot be misconfigured.** Customers ask "can a
   compromised robot reach my other robots?" In a mesh product the answer is
   "not if the policy is right". Here it is "there is no path": each link
   terminates at the operator and nothing is forwarded between links. A
   structural answer survives an audit; a configurable one costs a meeting.
3. **Nothing extra to deploy.** It is a capability inside the agent the
   company already ships — no second daemon, no per-device seat licence, no
   second thing to explain to their customer's IT department. For a robot
   sold into hospitals and factories, "one agent" is a sales argument.
4. **Access that expires by construction.** The link lives and dies with a
   session grant that already expires. A mesh VPN's purpose is permanent
   reachability, which is exactly what a support tool should not have.
5. **One connection to support.** NAT traversal, relay fallback and
   reconnection are solved once for video and the tunnel alike. A separate
   VPN duplicates all of it and then fails independently — two things that
   can be down is materially harder to support than one.

The boundary is also a product decision: this is **developer and support
access**, not the production data plane. Production traffic belongs in a
capability with a declared wire shape and backpressure. Holding that line is
what keeps Fjarr from drifting into being a general network with a robot logo
on it.

## Open-core split ([ADR-0011](adr/0011-license-open-core.md), [ADR-0015](adr/0015-backend-integration-strategy.md))

| Free & open (AGPL-3.0) | Paid |
|---|---|
| `libfjarr` agent library + `fjarr-agent` | **Fjarr Cloud**: managed signaling + TURN fleet, multi-tenant, metered |
| `fjarr-signaling` crate + `fjarr-server` sidecar (self-host) | **Fleet observability**: aggregation, retention, dashboards, alerting (M7) |
| `@fjarr/core`, `@fjarr/react` | **OTA campaigns**: staged rollouts, fleet targeting, audit (M8) |
| Protocol spec, SDKs, docs, demos | SSO/SCIM, audit export, support/SLA |
| Pipeline introspection on the robot + in the dashboard ([docs/24](24-pipeline-introspection.md)) | fleet-wide pipeline history, search and retention in Cloud (M7) |
| The [network tunnel](27-network-tunnel.md) and `fjarr-connect`, unlimited, self-hosted | **Access governance** around it: time-boxed and approval-gated grants, per-user policy, session recording, audit export (M5+) |
| | **Commercial license** for companies that can't ship AGPL |

Principles: the open core must be *genuinely usable alone* (a company can run
everything themselves); paid features are things that are **inherently
hosted-value** (aggregation, retention, campaigns, TURN capacity) or
enterprise process (SSO, SLA) — never artificial crippling.

## Revenue streams

1. **Fjarr Cloud subscriptions** — usage-metered (below).
2. **Commercial licenses** (AGPL exception) — flat per-product/year.
3. **Support & integration contracts** — early revenue while the funnel grows.

### Usage meters

Meter what tracks our real cost and the customer's real value:

| Meter | Tracks | Notes |
|---|---|---|
| Robots connected / month | fleet size | primary tier axis |
| Session-minutes | operator usage | media plane load |
| TURN relay GB | our bandwidth cost | the natural WebRTC meter; relay-only is common in the field |
| (M7+) metric series retained | storage | observability tier |
| (M8+) OTA campaign devices | campaign scale | OTA tier |

Metering is built into `fjarr-server` in **both** editions — billing input in
Cloud, plain observability when self-hosted.

### Pricing sketch (hypothesis, revisit at M5)

- **Free**: self-host everything; Cloud dev tier (≤3 robots, community).
- **Team** (~€49/robot/mo, decreasing with volume): Cloud signaling + TURN
  quota + webhooks + basic fleet view.
- **Scale** (custom): observability + OTA campaigns + SSO + SLA.
- **Commercial license** (from ~€10k/yr): AGPL exception, self-host.

## Go-to-market

1. **Open-source adoption engine** (M0.5→): landing at **fjarr.io**, honest
   docs, the three-demo stack as the "aha" (clone → `make demo-up` → see a
   robot in a dashboard in minutes). The tunnel is the second "aha" and aimed
   at the person who actually picks the framework: the integrating developer
   evaluates by asking "how do I debug this thing in the field", and
   `ssh` to a robot behind carrier NAT, with no jump host and no VPN to
   provision, answers it in one command. Content: the build-vs-buy engineering
   story we lived ([prior art](11-prior-art.md)).
2. **Pilot on Cloud, scale anywhere**: because sidecar and Cloud implement
   the same contract, evaluation friction is near zero and self-host is
   always available — trust, not lock-in, sells the robots-at-scale tier.
3. **Design-partner phase** (M3–M5): 2–3 robot companies (starting with our
   own network) integrate free with hands-on help in exchange for roadmap
   input and case studies.
4. **Expand per fleet**: revenue follows the customer's fleet growth via the
   robot/relay meters.

## Risks & watch items

| Risk | Mitigation |
|---|---|
| Big infra player bundles "robot connectivity" | AGPL + move fast on the robot-specific 20% they won't do (desktop backends, OTA, ROS adapters) |
| AGPL scares embedded legal teams | commercial license path, clean per-file headers, CLA from day one |
| TURN bandwidth cost underestimated | meter from M1; relay GB is priced through |
| Solo-founder bus factor | docs-first development *is* the mitigation |
| The tunnel grant is shell-equivalent — one mis-issued token exposes a robot's internals | off by default, explicit claim, audited both ends, no lateral movement by construction ([docs/10](10-security.md#network-tunnel)); the governance around it is the paid tier, so the safe path is also the commercial one |
| "Isn't this just a VPN?" in every evaluation | answer it in the docs and on the landing page, not in the call ([above](#tunnel-positioning)) |

## Brand

**Fjarr** — Swedish *fjärr*, "remote". Chosen 2026-09-15 after an
availability-checked shortlist (styrlina, livlina, servolink, robobridge —
all free; fjarr won on brevity + meaning). Registered same day: domains
**fjarr.io** + **fjarr.dev** (Cloudflare), npm org **@fjarr**, GitHub org
**fjarrio** (`fjarr` was taken) connected to crates.io.
