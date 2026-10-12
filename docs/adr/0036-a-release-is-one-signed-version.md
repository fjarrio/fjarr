---
title: "ADR 0036: A Release Is One Signed Version"
description: A robot always runs exactly one release, whose signed manifest pins every component (OS image, apps, configuration, peripherals) with rollback classes and hardware variants; signing follows TUF, with keys held by the robot company in production and a Fjarr-managed key as an easy start.
---

- **Status**: accepted
- **Date**: 2026-10-12
- **Supersedes**: —

## Context

A robot is an OS image, applications, configuration and peripherals
(MCU firmware, motor controllers, a GNSS receiver, a router, a PLC). Erik
wants statistics compared across versions over years, updates that sometimes
carry a whole new image and sometimes only a configuration, and peripherals
programmed as part of an update ([docs/32](../32-fleet-ota.md)). Whoever
can sign an update can command every robot in a fleet of moving machines.

## Options considered

1. **Per-component versions**: each component versioned on its own, robots
   free to mix. Pros: flexible. Cons: the number of combinations makes
   comparing versions meaningless; nobody knows what a robot "is".
2. **One release version with a manifest pinning every component.** Pros: a
   robot is always exactly one release; statistics compare releases; a light
   release reuses the previous image's hash and installs nothing of it. Cons:
   every change, however small, is a release.
3. **A release plus recorded per-robot overrides.** Pros: hotfixes without a
   release. Cons: overrides erode the comparison; deviation is better
   detected than designed in.

For signing: keys held only by Fjarr (simple, but a compromised Fjarr could
push code to every customer's fleet), keys held only by the company (safe,
but a hard first step for a prototype), or both, with company keys as the
production setup.

## Decision

**Option 2, one signed release version** (Erik, 2026-10-12). The manifest
pins each component by content hash and declares `requires`, `hardware`
variants, a `rollback` class (`atomic`, `restorable`, `forward-only`),
`migrates_data`, `safety_relevant`, and whether the release is `security`.
A robot reports its release and a version vector read back from its hardware
on every connect, so hand-made deviations show instead of hiding.

Signing follows TUF and Uptane: an offline root key, release keys that sign
manifests, and online keys in Fjarr Cloud that sign only timestamps and
targeting. **Company-held keys** are the production setup, optionally with
two signatures; **Fjarr-managed keys**, one set per tenant, are an easy start,
labelled as such, and moving between them is an ordinary rotation (Erik,
2026-10-12).

## Consequences

- Every metric and error in M7 is keyed on the release version; the
  release history of every robot is kept indefinitely.
- A compromised Fjarr Cloud can delay updates but cannot forge one when the
  company holds its keys; with Fjarr-managed keys it could, which the UI
  says.
- Safety-relevant components are not installed over the air by default, and
  security releases follow the security channel's rules
  ([docs/32](../32-fleet-ota.md#security-channel)).
- Revisit if customers need independent cadences per component that
  releases make too heavy, for example peripherals owned by another
  company.
