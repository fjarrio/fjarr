---
title: "ADR 0004: Rust signaling server"
---

- **Status**: accepted
- **Date**: 2026-09-15

## Context

The signaling tier holds thousands of long-lived WebSockets, verifies
tokens, mints TURN credentials, meters usage — and ships to customers as a
container they run forever.

## Options considered

Node/TS (shared toolchain with web, weaker for a hardened long-running
sidecar) · Go (fine, another toolchain either way) · **Rust** (small static
deployable, predictable memory, strong WS/HTTP story with axum/tokio, and
the crate doubles as an embeddable library for Rust shops).

## Decision

Rust with axum + tokio; library crate (`fjarr-signaling`) + thin binary
(`fjarr-server`), per ADR-0015.

## Consequences

A third toolchain in the repo (accepted; devcontainer pins 1.89.0). The
embeddable-crate story is a bonus, not the primary integration path
(ADR-0015's contract is).
