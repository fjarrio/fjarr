---
title: "ADR 0035: Fjarr Orchestrates Releases; OS Appliers Are Narrow Adapters"
description: Fjarr owns fetching, readiness, ordering, health, peripherals and reporting for an update; the engine that writes the OS slot (SWUpdate, RAUC, systemd-sysupdate, NixOS) sits behind a narrow adapter, one per recipe, chosen by measurements on the reference hardware.
---

- **Status**: accepted
- **Date**: 2026-10-12
- **Supersedes**: [ADR-0016](0016-swupdate-ota.md)

## Context

ADR-0016 (proposed, 2026-09-15) chose SWUpdate as the base of `fjarr.ota`,
with "SWUpdate vs RAUC" left open until M8 (open question #6). The M8
brainstorm of 2026-10-12 ([docs/32](../32-fleet-ota.md)) widened the scope.
Updates must work on Yocto and NixOS images from the start, on a Jetson
(the ZED Box Mini) and on x86, plus an Ubuntu A/B image and plain Ubuntu with
apt. A release spans the OS, applications, configuration and peripherals.
And the update has to wait for readiness conditions and a prepare action,
and report every step.

A survey of the update engines (SWUpdate, RAUC, OSTree/bootc,
systemd-sysupdate, the Mender client, NVIDIA's L4T OTA) found that they all
apply an OS slot well and that none has fleet logic. Their weak spots are
networking: RAUC's streaming install fails on mobile links and does not
resume (RAUC issue #1517); SWUpdate's zchunk delta has no fallback and no
documented resume; systemd-sysupdate has no delta. SWUpdate's handlers are
forward-only and file-ordered, and report no per-component versions, which
does not fit peripherals with read-back and rollback. On Jetson, meta-tegra's
redundant layout uses SWUpdate as its A/B reference, while RAUC on Orin has
only partial bring-ups. That evidence is thin: nobody on the project has run
either on the target hardware.

## Options considered

1. **One engine for everything** (SWUpdate, as ADR-0016 proposed). Pros:
   one integration. Cons: SWUpdate does not fit NixOS's model; its handlers
   are the wrong shape for peripherals; its networking would have to be
   avoided anyway.
2. **Fjarr orchestrates; each recipe's OS engine is a narrow adapter.** Fjarr
   owns fetching (over its own resumable chunk transport), readiness,
   ordering, health checks, peripherals and reporting. The adapter does only
   install-to-inactive-slot, set-next-boot, slot-state, mark-good and
   rollback-now, and installs only from the local cache. Pros: the same
   orchestrator and release format on every OS and board, and the engines'
   network weaknesses no longer matter. Cons: Fjarr builds and maintains the
   orchestration, and one adapter per recipe.
3. **Adopt a full platform's client** (Mender). Pros: a complete state
   machine. Cons: it assumes Mender's server, which competes with Fjarr
   Cloud, and it still has no robot-aware readiness or peripheral ordering.

## Decision

**Option 2.** `fjarr-update` orchestrates every release; the OS-slot work
is behind a narrow applier adapter, one per recipe. Which engine backs which
recipe is decided by the hardware spikes of M8 slice 8.0, by measurement.
SWUpdate on meta-tegra's A/B layout is the starting point for Yocto on
Jetson; NixOS generations within an A/B image layout for NixOS; RAUC or
systemd-sysupdate for the x86 and Ubuntu images; apt for the non-atomic
Ubuntu path. Peripherals are Fjarr-level components applied by handlers
([docs/32](../32-fleet-ota.md#peripherals)), not engine plugins.

The engines run as separate processes, invoked and never linked, so
SWUpdate's GPL-2.0 and RAUC's LGPL-2.1 do not reach Fjarr's code
([ADR-0011](0011-license-open-core.md)). Whether a Fjarr-*published* image
containing them is a shipped artifact is open question #43.

## Consequences

- Open question #6 is closed: the engine is a per-recipe choice made by
  measurement, not a project-wide bet.
- Fjarr maintains the orchestrator, the adapter contract and one adapter per
  recipe, and each needs CI on real hardware (the ZED Box Mini and the x86
  mini-PC in the lab, nightly).
- The engines never touch the network, so their streaming and delta
  limitations do not constrain Fjarr; Fjarr's chunk transport
  ([ADR-0037](0037-release-content-over-https-chunks.md)) is the delta.
- Revisit if an engine grows a fleet-quality network path that beats Fjarr's
  transport, or if one engine proves itself on every recipe.
