---
title: "ADR 0005: Web library split"
---

- **Status**: accepted
- **Date**: 2026-09-15

## Context

Customers embed Fjarr into existing dashboards (React today, maybe not
forever); we must ship a library, not an app — and prove the boundary.

## Options considered

Single React package (locks logic into React) · **core + React bindings**
(logic reusable for Vue/Svelte wrappers later; headless-first components) ·
app-first with extraction later (the teleop-car path: one 806-line component,
never extractable).

## Decision

`@fjarr/core`: dependency-free session/state/protocol logic. `@fjarr/react`:
bindings + headless-first components. `demos/demo-dashboard` consumes only
the published API (the demo-as-integration-test rule, docs/02).

## Consequences

Slightly more plumbing now; framework portability and honest APIs forever.
Core carrying zero runtime deps is a stated design goal (docs/14).
