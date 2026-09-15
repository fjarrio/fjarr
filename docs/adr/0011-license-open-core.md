---
title: "ADR 0011: AGPL open core"
---

- **Status**: accepted
- **Date**: 2026-09-15
- **Supersedes**: the early-planning "proprietary internal" assumption

## Context

Fjarr aims to be an open-source framework with a revenue-bearing
cloud/product around it (docs/03), built largely by one founder who needs
the work to remain defensible.

## Options considered

- **AGPL-3.0 core + commercial license** (RustDesk/Grafana-style): free
  self-hosting; embedding into proprietary offerings or SaaS-ing the code
  requires opening changes or buying a license. Requires a CLA to keep dual-
  licensing rights.
- Apache-2.0/MIT core: maximum adoption; nothing prevents a larger player
  from hosting it against us.
- Source-available (BSL etc.): weaker community trust, license-lawyer
  friction with target customers.

## Decision

AGPL-3.0 for the open core (all three libraries, sidecar, demos, docs);
commercial license available; CLA required for external contributions
(tooling before the first outside PR — open question #13). Fjarr Cloud and
flagship fleet features are closed (docs/03 split).

## Consequences

- **Shipped artifacts must stay GPL-dependency-free** anyway: commercial
  (exception-licensed) builds cannot link GPL code — so `x264enc` stays
  banned (doctor-enforced) and VA-API remains the product encode path.
- Per-file headers + license files land with the first public push (M0.5).
- Some embedded-legal friction is expected; the commercial path is the
  answer, priced in docs/03.
