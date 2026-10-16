---
title: "ADR 0039: The Ubuntu A/B Image on x86 Uses systemd-sysupdate and systemd-boot"
description: The x86 Ubuntu A/B recipe writes its slots with systemd-sysupdate from a local directory and boots unified kernel images through systemd-boot's boot counting, behind Fjarr's applier; RAUC with GRUB was the comparison. Chosen on slice 8.0's QEMU rehearsal and the lab NUC's 60 power cuts.
---

- **Status**: superseded by [ADR-0040](0040-x86-ubuntu-ab-swupdate-efibootguard.md)
- **Date**: 2026-10-15
- **Supersedes**: —

## Context

[ADR-0035](0035-releases-orchestrated-appliers-narrow.md) puts each recipe's
OS engine behind a narrow applier (install to the free slot, set the next
boot, read the slot state, mark good, roll back now) and leaves the choice of
engine to slice 8.0's measurements. For x86 and Ubuntu,
[docs/32](../32-fleet-ota.md#appliers) named RAUC or systemd-sysupdate.

Slice 8.0 built the x86 test image ([docs/32](../32-fleet-ota.md#x86-test-image)):
Ubuntu 26.04 by mkosi, two root slots pinned by `PARTLABEL` from each
slot's unified kernel image (UKI), a rescue system, and an applier spike over
systemd-sysupdate 259. It passed every failure case in QEMU and then on the
lab NUC: an install that does not activate, set-next and bless, roll back
now, a failed health check, a kernel panic and a missing root on the trial
boot, every slot exhausted, a hung kernel reset by the firmware watchdog, and
60 power cuts at random moments of the install, the switch and the trial
boot, all of which came back by themselves on the expected slot. RAUC was
compared on paper from its source and documentation, as planned.

## Options considered

1. **systemd-sysupdate + systemd-boot** (UKIs on the ESP, boot counting in
   the file name). Pros: three boot tries; the counter, blessing and the
   fallback order are systemd-boot's own, so a rescue entry sorts between
   healthy and exhausted slots with nothing extra; the slot label is the
   commit point and the UKI is written last, which the power cuts confirmed;
   nothing depends on the firmware's boot entries; Ubuntu 26.04 ships every
   piece (systemd 259, mkosi 26); it builds on the same mkosi pipeline as the
   rescue system and makes reproducible images. Cons: sysupdate has no verb
   to roll back now or to install without activating, so the applier renames
   UKIs itself (a short script); it activates on install, which leaves a
   window of a few milliseconds where a power cut boots the new slot early,
   with its three tries; it is marked experimental upstream, its D-Bus
   service is unshipped in 26.04 and being replaced by Varlink, and offline
   acquire needs systemd 260; Ubuntu ships its kernels as files, not UKIs, so
   the image must be built (mkosi), not installed.
2. **RAUC + GRUB** (RAUC 1.15.1 in 26.04, kernels inside each slot, the
   state in `grubenv`). Pros: every applier verb is built in (install, mark
   good, mark bad, mark active) with a D-Bus API; Ubuntu's own kernels work;
   signed bundles with dm-verity. Cons: one boot try, so a power cut or a
   watchdog reset during the trial boot rolls back an update that would have
   come up; `grub-editenv` rewrites `grubenv` in place, so a cut during
   activation or mark-good can leave it unreadable; a second trust root (the
   bundle's X.509 signature) next to the release's; GRUB is GPL-3, which the
   images may carry but which adds a licence question for locked devices
   ([docs/18](../18-open-questions.md) #44).
3. **RAUC with its EFI backend.** No boot counter at all, and the state lives
   in the firmware's boot variables, whose handling on the NUC's 2022
   firmware is the least known part of the machine. Rejected.
4. **RAUC with a custom backend over systemd-boot.** Keeps RAUC's verbs and
   bundles and systemd-boot's counting, but the backend is ours to write and
   maintain until RAUC's systemd-boot support (its PR #1367) lands. Kept as
   the fallback if option 1's gaps grow.
5. **SWUpdate on x86.** It belongs to the Yocto recipe on Jetson (meta-tegra's
   reference); on x86 Ubuntu it brings neither boot counting nor verbs that
   option 1 lacks.

## Decision

The Ubuntu A/B recipe on x86 uses **option 1**: systemd-sysupdate writes the
root slot and its UKI from a directory `fjarr-update` has already verified,
and systemd-boot counts three tries. Fjarr's applier wraps it in the five
verbs, parks a newly installed UKI as exhausted (`+0-0`) until `set-next`,
renames it to roll back now, and lets `systemd-bless-boot` mark good once
Fjarr's health check is ordered before `boot-complete.target`.

The deciding facts are measured, not argued: three tries against one, a
commit point that survived 60 power cuts, and no dependence on the firmware's
boot variables. RAUC's built-in verbs were the main thing given up, and the
applier replaces them in a script of a few dozen lines.

The image carries what the rehearsal showed it needs ([docs/32](../32-fleet-ota.md#x86-test-image)):
each UKI pinned to its slot by `PARTLABEL`, a bounded wait for the root
device and an initrd that reboots instead of waiting at an emergency shell,
the watchdog drivers that Ubuntu deny-lists, the ESP mounted only on demand,
and a reproducible build.

## Consequences

- Fjarr maintains the applier's glue: the activation parking, roll back now
  and the slot state, and tests them in the rehearsal on every image change.
- systemd is pinned to the release in the image (259 in Ubuntu 26.04), and
  the applier talks to `systemd-sysupdate` on the command line, not over
  D-Bus.
- The x86 Ubuntu image is a built image (mkosi), not an installed Ubuntu;
  apt on a running slot is not how it updates.
- The release's signature is the only trust root; sysupdate's own GPG check
  applies only to network sources, which the applier never uses.
- Revisit (a superseding ADR) if: sysupdate gains roll-back and
  install-without-activating verbs or a stable Varlink API (systemd 260 and
  later); RAUC gains systemd-boot support; the read-only root
  ([docs/32](../32-fleet-ota.md#x86-test-image)) changes how slots are
  written; or the Yocto and NixOS recipes on x86 (slice 8.5b) find one
  engine that serves all three.
