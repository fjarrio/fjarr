---
title: "ADR 0040: The Ubuntu A/B Image on x86 Uses SWUpdate and EFI Boot Guard"
description: The x86 Ubuntu A/B recipe writes its slots with SWUpdate and boots them through EFI Boot Guard, the engine the Jetson Yocto recipe already plans; it supersedes ADR-0039's systemd-sysupdate and systemd-boot after both were built and power-cut tested on the lab NUC.
---

- **Status**: accepted
- **Date**: 2026-10-16
- **Supersedes**: [ADR-0039](0039-x86-ubuntu-ab-sysupdate-systemd-boot.md)

## Context

ADR-0039 chose systemd-sysupdate with systemd-boot for the Ubuntu A/B image
on x86 after slice 8.0's rehearsal and 60 power cuts. Its comparison
covered RAUC on paper and dismissed SWUpdate in a line. SWUpdate was then
built and tested the same way, with EFI Boot Guard (EBG) as its boot loader
on x86, as the CIP project's isar-cip-core does
([docs/32](../32-fleet-ota.md#x86-swupdate-ebg)). Both designs passed every
case in QEMU and on the lab NUC: SWUpdate with EBG passed 80 power cuts,
including cuts during the confirmation write.

They differ in three ways that matter:

- **One engine across recipes.** The Yocto recipe on the Jetson already
  plans SWUpdate (meta-tegra's A/B reference). With SWUpdate on x86 too,
  Fjarr has one engine, one package format and one set of failure
  behaviours to adapt, test and document.
- **The loader's watchdog.** EBG arms the hardware watchdog before it
  starts the kernel. A trial kernel that hung before its watchdog driver
  loaded was reset by EBG's watchdog and rolled back by itself, in 88
  seconds on the NUC. With systemd-boot nothing arms a watchdog that early,
  and the robot waits for a person.
- **Tries.** EBG gives a new version one try: any reset during its first
  boot, a power cut included, rolls it back. systemd-boot gave three.

## Options considered

1. **Keep ADR-0039** (systemd-sysupdate + systemd-boot): three tries, all
   in Ubuntu's main archive, no second key, LGPL. No recovery from an early
   hang, and an engine used by no other recipe.
2. **SWUpdate + EFI Boot Guard.** The consistency and the early-hang
   recovery above. Costs, measured: one try; 53 MiB of dependencies the
   applier does not use; Ubuntu 26.04's EBG 0.19 lacks a rollback fix from
   0.20, so the loader is taken from 0.22; Ubuntu's SWUpdate refuses
   unsigned packages, so the package carries a signature of its own;
   SWUpdate and EBG, `libebgenv` included, are GPL-2.0-only; EBG's stock
   tools mount every FAT partition on the disk read-write.
3. **RAUC + GRUB** (ADR-0039's comparison): one try like EBG, but no
   loader-armed watchdog and a boot-state file rewritten in place, and a
   third engine. Not pursued.

## Decision

The Ubuntu A/B recipe on x86 uses **SWUpdate with EFI Boot Guard**
(option 2), chosen by Erik on 2026-10-16 for the consistency of one update
engine across the Jetson and x86 recipes; the early-hang recovery measured on
the NUC supports it.

The layout keeps ADR-0039's slots, rescue system and reflash, and adds one
EBG environment partition per slot (`fjarr-boot0`, `fjarr-boot1`) holding
that slot's environment and UKI. Each UKI finds its root by filesystem label,
because a raw slot write carries the filesystem's label and not the GPT's.
Fjarr's applier keeps the five verbs: SWUpdate writes the free slot and its
UKI from a signed package, the applier writes the environments one partition
at a time with `bg_setenv`, and a confirmation unit marks the trial good once
Fjarr's health check is ordered before `boot-complete.target`.

## Consequences

- One try per update. A power cut or reset in a new version's first seconds
  rolls back, and the update is offered again; the run log says so.
- The images arm the watchdog in the loader and set
  `watchdog.handle_boot_enabled=0`, so the kernel does not feed it before
  userspace takes over. A board whose watchdog EBG cannot drive gets a
  watchdog timeout of 0 in its environments, and the board package says so.
- SWUpdate and EBG run as separate tools, never linked into Fjarr
  (GPL-2.0-only); they are in images built from Fjarr's recipes, not in
  Fjarr's packages ([docs/14](../14-dependencies.md)).
- The package signature: Ubuntu's SWUpdate requires one, so Fjarr either
  signs packages with a key from the release's key set or builds SWUpdate
  without that requirement; slice 8.1 decides it with the release format.
- Until Ubuntu ships EBG 0.20 or later, the recipe takes the loader from a
  newer build, pinned by hash; a trimmed SWUpdate build (without its web
  server, network clients and Lua) is part of slice 8.5a.
- The applier never uses EBG's tools' own partition discovery, which
  mounts the rescue partition read-write.
- Revisit if: EBG cannot drive a supported board's watchdog; the one-try
  rollbacks show up as a real cost in the field; or the Jetson recipe ends
  up without SWUpdate, which removes the consistency this decision rests on.
