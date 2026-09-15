---
title: "ADR 0003: Polyglot monorepo"
---

- **Status**: accepted
- **Date**: 2026-09-15

## Context

The protocol (docs/08) is implemented three times (C++, Rust, TS); specs,
schemas, and demos must move in lockstep with implementations.

## Options considered

Monorepo · per-tier repos (protocol drift across repos — exactly the fleet-daemon
schema-drift failure) · schema-repo + impl repos (submodule staleness,
observed first-hand).

## Decision

Monorepo: `agent/`, `signaling/`, `web/`, `demos/`, `website/`, `docs/`,
`protocol/`. One PR changes a protocol shape and all its implementations.

## Consequences

Simple atomic changes and CI; requires per-artifact release tooling from M1
(publishing npm/crates/images from one repo). Demo/library boundaries are
enforced by convention + review (docs/13), not repo walls.
