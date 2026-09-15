---
title: "ADR 0015: Backend integration strategy"
---

- **Status**: accepted
- **Date**: 2026-09-15

## Context

Customer backends run Go, Node, Python, Java, .NET… The backend tier must
integrate with all of them, and the same tier is the commercial engine
(docs/03).

## Options considered

- **FFI bindings** (napi-rs/cgo embedding the Rust service): a network
  server holding thousands of async sockets embedded in a foreign runtime —
  cgo/thread-model pain, per-language support burden forever, shared failure
  domain with the customer's process. Right tool for codec libraries, wrong
  tool for services. Rejected.
- **Managed-only backend**: fastest to revenue, kills the open-source
  adoption engine, contradicts ADR-0011. Rejected.
- **Unified contract, sidecar + managed twin** (LiveKit/Temporal analog):
  one stack-agnostic HTTP/JSON contract — session-grant JWTs minted by the
  customer, a small control-plane REST API (OpenAPI), signed webhooks.
  `fjarr-server` (AGPL, self-hosted container) and Fjarr Cloud implement it
  identically; thin generated SDKs (TS → Go → Python) replace bindings.

## Decision

The unified contract (normative in docs/09). Customer backends stay out of
media and signaling hot paths. Usage metering is built into the server in
both editions. The demo-backend is TypeScript to demonstrate
stack-agnosticism ("keep your backend, add two endpoints").

## Consequences

Zero-rework migration between self-hosted and Cloud (trust story + upsell
funnel); the contract becomes a compatibility surface we version carefully;
`fjarr-signaling` remains an embeddable bonus for Rust shops, not the
primary path.
