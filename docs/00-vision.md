---
title: Vision
description: Why Fjarr exists, who it serves, and what it deliberately is not.
---

**Fjarr** (from Swedish *fjärr*, "remote" — as in *fjärrkontroll*, remote
control) is a generic framework for **peer-to-peer connectivity to robots and
IoT devices**: live camera video, remote desktop, sensor streaming, file
transfer, and a remote terminal, delivered over WebRTC, and extensible with
capabilities we haven't thought of yet.

## The problem

Every robot product company rebuilds the same connectivity stack, badly, under
deadline pressure:

- a camera stream bolted onto a dashboard with a hand-rolled signaling server,
- remote support done by asking the customer to install a commercial remote
  desktop tool that knows nothing about robots,
- log retrieval over `scp` through a jump host,
- teleoperation channels with no congestion control, no reconnect, and static
  credentials compiled into the frontend.

We know, because we've built three of them (see [prior art](11-prior-art.md)).
The transport problems — NAT traversal, congestion control, reconnection,
multi-viewer fan-out, credential lifetime, unattended access — are identical
across companies, yet each solves them from scratch because the existing
products are either general-purpose remote desktop tools that can't be
embedded meaningfully (RustDesk, AnyDesk), fleet platforms without real-time
media (most IoT clouds), or media SaaS without a device story (Twilio-style).

## What Fjarr is

Three **embeddable libraries** plus the services around them:

1. **`libfjarr`** (C++/GStreamer) — embeds into the robot's software, or runs
   beside it as the `fjarr-agent` reference daemon.
2. **`fjarr-signaling`/`fjarr-server`** (Rust) — the signaling sidecar a
   company deploys next to their backend, or lets us host (**Fjarr Cloud**).
   Their backend integrates through a small token/REST/webhook contract and
   never touches the media path ([ADR-0015](adr/0015-backend-integration-strategy.md)).
3. **`@fjarr/core` + `@fjarr/react`** (TypeScript) — embeds the operator
   experience into their existing dashboard.

Everything user-visible is a **capability plugin** on a shared core
([extension model](05-extension-model.md)); companies extend Fjarr with their
own capabilities without forking it.

Longer-term, the same connectivity substrate carries **fleet observability**
(versions, metrics, error rates) and **atomic OTA updates** (SWUpdate-based
A/B with rollback) — see [capabilities](06-capabilities.md) and the
[roadmap](17-roadmap.md).

## Personas

| Persona | What Fjarr must give them |
|---|---|
| **Integrating developer** at a robot company | Libraries with small, documented seams; a working demo stack to copy; no forced UI, auth, or backend framework |
| **Operator / support engineer** | Click a robot, see its cameras/desktop in the tool they already use, low latency, survives bad networks |
| **Fleet admin** | Who accessed what and when; credentials that expire; works after reboot with nobody on site |
| **Framework contributor** | Specs that explain *why*; a devcontainer that works on the first try; tests that catch protocol drift |

## Non-goals

- **Not a general-purpose remote desktop product** for PCs/phones — Ubuntu
  robot appliances first; anything else must not complicate the core.
- **Not a robot middleware** — Fjarr does not replace ROS 2, DDS, or the
  company's own control stack; it connects to them through adapters.
- **Not a dashboard** — `@fjarr/react` provides components, never a portal.
  The demo dashboard is a demo.
- **Not modem/VPN management** — Fjarr assumes IP connectivity exists.

## Success criteria

- A robot company integrates all three tiers in **under a week** using only
  the public docs, without talking to us.
- One operator watching one robot camera at 1080p sits within the
  [performance budgets](16-performance-budgets.md) on an Intel NUC.
- A robot 800 km away is reachable **after an unattended reboot**.
- A third party ships a capability plugin we've never seen, against the
  documented API, without patching the core.
- The open-source core is useful alone; Fjarr Cloud is *more convenient*, not
  artificially gated ([product strategy](03-product-strategy.md)).
