---
title: Product Strategy
description: The living SaaS business plan — open-core split, pricing meters, go-to-market.
---

> **Living document.** This is the business plan, versioned next to the code
> and iterated as the product is built. The root README summarizes it. Treat
> numbers as hypotheses with dates, not facts.

## One-line pitch

*Fjarr gives robot companies the remote connectivity stack they keep
rebuilding — embeddable camera streaming, remote desktop, file transfer and
fleet tooling — as open-source libraries plus a managed cloud.*

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
and fleet tooling — one protocol, embeddable at every tier.

## Open-core split ([ADR-0011](adr/0011-license-open-core.md), [ADR-0015](adr/0015-backend-integration-strategy.md))

| Free & open (AGPL-3.0) | Paid |
|---|---|
| `libfjarr` agent library + `fjarr-agent` | **Fjarr Cloud**: managed signaling + TURN fleet, multi-tenant, metered |
| `fjarr-signaling` crate + `fjarr-server` sidecar (self-host) | **Fleet observability**: aggregation, retention, dashboards, alerting (M7) |
| `@fjarr/core`, `@fjarr/react` | **OTA campaigns**: staged rollouts, fleet targeting, audit (M8) |
| Protocol spec, SDKs, docs, demos | SSO/SCIM, audit export, support/SLA |
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
   robot in a dashboard in minutes). Content: the build-vs-buy engineering
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

## Brand

**Fjarr** — Swedish *fjärr*, "remote". Chosen 2026-09-15 after an
availability-checked shortlist (styrlina, livlina, servolink, robobridge —
all free; fjarr won on brevity + meaning). Registered same day: domains
**fjarr.io** + **fjarr.dev** (Cloudflare), npm org **@fjarr**, GitHub org
**fjarrio** (`fjarr` was taken) connected to crates.io.
