---
title: "ADR 0043: Jetson Capsule Signing and the Firmware Ratchet"
description: Jetson capsules are signed with the robot company's own key, which UEFI is built to trust; the firmware floor stays at the product's first firmware, with signed vulnerability advisories as the main protection against old firmware, decided after phase E of slice 8.0 measured both on the ZED Box Mini.
---

- **Status**: accepted (Q11 in [docs/32](../32-fleet-ota.md#jetson-test-image); Erik, 2026-11-19)
- **Date**: 2026-11-19
- **Supersedes**: —

## Context

A Jetson's bootloader firmware lives on the QSPI in two chains, and Fjarr updates it with
a UEFI capsule that the inactive chain receives on the next boot
([docs/32](../32-fleet-ota.md#updater)). Phase E of slice 8.0 measured the capsule path on
the ZED Box Mini (2026-11-14 to 2026-11-17). Q11 deferred two choices until then: what
signs a capsule, and how far the firmware may go back.

What phase E found:

- **UEFI checks every capsule's signature before it writes.** A capsule with one byte
  changed in chain B's UEFI image was refused with nothing written: the ESRT's last
  attempt `0x1012`, `LAST_ATTEMPT_STATUS_DRIVER_ERROR_IMAGE_AUTH_FAILURE` (E5). edk2's
  FmpDxe verifies a PKCS7 signature over the payload against a certificate built into
  UEFI (`PcdFmpDevicePkcs7CertBufferXdr`).
- **That certificate is public test material today.** edk2-nvidia's platform file
  builds UEFI to trust edk2's `TestRoot.cer`, and meta-tegra's
  `tegra-uefi-capsule-signing` class signs with edk2's `TestCert.pem`, both shipped in
  edk2's BaseTools. Any capsule signed with that public key would be written. meta-tegra
  lets the signing side be replaced (`UEFI_CAPSULE_*_CERT`, and `sign_uefi_capsules` as a
  hook for a signing server), but not the certificate UEFI trusts: that is a change to
  the UEFI build.
- **The ratchet is box-wide and one-way.** meta-tegra sets each capsule's lowest supported
  version to its own version (`TEGRA_UEFI_LOWEST_SUPPORTED_VERSION`, by default
  `TEGRA_UEFI_FW_VERSION`). After one 36.5.2 capsule, the ESRT's lowest supported
  version was 36.5.2 on both chains, chain A included, which still ran 36.4.4. A 36.4.4
  capsule offered afterwards was refused before staging (E4).
- **A trial boot cannot see the record of a capsule that older firmware applied**: the
  capsule status was 0 and the ESRT showed no attempt. The applier judges a capsule trial
  by the firmware its chain runs (E4;
  [applier/README.md](../../spikes/ota-jetson/applier/README.md)).
- **The spike's signed package does not cover the capsule.** The SWUpdate package is
  signed, but `fjarr_<v>.cap` and `fjarr_<v>.fw` lie beside it, and `install` records
  the capsule's hash from whatever file is staged. In the spike, only UEFI's check
  stands between the stage directory and the QSPI, against a public key.

Two rules in docs/32 bear on this. Release keys are held by the robot company in
production, with a Fjarr-managed key per tenant as an easy start
([Signing](../32-fleet-ota.md#signing)). A lower release may be installed only with a
signed downgrade authorization. A firmware floor that only ever rises would turn that
authorization into a dead letter for any release that carries older firmware.

This ADR is about capsules. It is not about NVIDIA's secure boot (PKC keys fused into
the SoC). Without those fuses, root on the robot can rewrite the QSPI anyway, so a
capsule key protects the update path and the files waiting on the ESP, not a robot
whose root is already compromised.

## Options considered

### What UEFI trusts, and who signs

**A. Keep meta-tegra's default:** edk2's test root in UEFI, and the release signature as
the only real check, with the release manifest covering the capsule.

- Pro: no UEFI change; one key to manage, the release key.
- Con: any process that can place a capsule on the ESP, or any image that sets
  `OsIndications`, can write firmware signed with a key anyone can download. The release
  signature then protects only the path Fjarr's applier takes.
- Con: a robot shipped this way cannot later be moved to a real key without a capsule
  that carries the new certificate, signed with the old public one. That is possible,
  but it is the same weak step at the start of every fleet's life.

**B. One Fjarr key**, built into the UEFI of every board package Fjarr ships, with Fjarr
signing every capsule.

- Pro: real protection from the first flash; one signing service to run.
- Con: Fjarr becomes able to rewrite every customer robot's firmware, which docs/32's
  signing model rules out for releases ("a compromised Fjarr Cloud can delay updates,
  never forge one").
- Con: a customer building its own board package with its own recipe has no key.

**C. The robot company's own capsule key**, as a build input of the board package. UEFI
is built to trust the company's capsule certificate, and its release CI or signing
server signs capsules through meta-tegra's `sign_uefi_capsules` hook. A Fjarr-managed key
per tenant serves as the easy start, as for release keys. A production build refuses to
build with edk2's test root.

- Pro: the same custody model as release keys. Fjarr Cloud can still never forge an
  update, now including the firmware.
- Pro: the test root stays usable for the lab and for prototypes, and is flagged loudly.
- Con: one more key per company. Its loss is serious: no more capsules, only a reflash
  through RCM with physical access. Rotation is itself a capsule: firmware carrying the
  new certificate, signed with the old key.
- Con: a UEFI change Fjarr maintains in its recipe layer (a `.bbappend` that sets the
  certificate PCD), checked again on every meta-tegra and L4T update.

### The firmware's lowest supported version

The floor is the lowest firmware a robot accepts by capsule. Its purpose is replay: every
capsule ever shipped stays validly signed, so the signature alone would let an old firmware
with a known hole be put back. It does not affect `rollback-now`, which switches to the
other chain without writing firmware; it decides only whether older firmware can be written
again once both chains carry the new one.

Who it stops matters. docs/32's release rules already keep old releases off the normal update
path (a monotonic release counter, and a signed downgrade authorization to go back). Without
NVIDIA's secure boot fuses, which the product's robots do not have today, root on a robot can
rewrite the QSPI directly, without a capsule. So on these robots a UEFI floor adds only a
second lock against someone who can deliver a capsule but has no root. On a robot fused for
secure boot it is the lock that matters: there a capsule downgrade to a vulnerable bootloader
is the way to break the secure boot chain.

**a. meta-tegra's default:** every capsule's floor is its own version.

- Pro: no older firmware ever comes back through a capsule, with no decisions to make.
- Con: a bad firmware release cannot be undone once both chains carry it: only a newer
  firmware, or a reflash at each robot over RCM.
- Con: it contradicts docs/32's signed downgrade authorization, which then works for
  everything but the firmware, and fails at UEFI.
- Con: as E4 showed, one capsule raises the floor for the whole box.

**b. The floor as an explicit release property.** A release states its firmware floor; it
stays at the product's first shipped firmware unless a release is marked as a security floor.

- Pro: downgrades within the floor work, and a fixed vulnerability can be locked out.
- Con: a decision for every firmware release; a forgotten one leaves an old firmware
  installable, and one made by mistake cannot be undone.

**c. No floor.**

- Pro: nothing to decide; every downgrade works.
- Con: firmware older than the board package was ever built for could be written too.
- Con: nothing on the robot ever refuses old firmware, even on a robot fused for secure boot.

**d. Advisories first, the floor kept low and raised rarely.** Releases carry signed
vulnerability advisories; Fjarr Cloud shows them and asks before an affected release is
installed, and `fjarr-update` does the same for a local bundle. The UEFI floor stays at the
product's first shipped firmware, and is raised only on robots fused for secure boot, or for
a vulnerability severe enough to lock out for good.

- Pro: the honest path gets an informed choice instead of a hard refusal: a bad firmware
  release can be undone, as docs/32's downgrade rule intends.
- Pro: no decision per release, which was b's cost.
- Pro: the floor stays available for the robots where it is the real protection.
- Con: on today's robots, old signed firmware stays installable by anyone who can deliver a
  capsule and ignore the advisory. That is the exposure docs/32 already accepts for releases,
  and on unfused robots root has a shorter way anyway.

## Decision

**C and d** (Erik, 2026-11-19).

- **The capsule key is the robot company's**, a build input of the board package, with a
  Fjarr-managed key per tenant as the easy start. This is the custody model docs/32 already
  chose for release keys, so the firmware gains no party able to forge it that the releases
  do not already have. A production build refuses edk2's test root; lab and prototype builds
  keep it, labelled as such.
- **Old firmware is met by advisories, not by the floor.** A release carries signed
  vulnerability advisories in the open release format. Fjarr Cloud shows them before an
  affected release is installed or targeted, and `fjarr-update` warns before a local bundle
  is installed, so open-core users without the cloud see them too. The choice stays the
  installer's.
- **The UEFI floor stays at the product's first shipped firmware**, so a capsule cannot write
  firmware the board package was never built for. `fjarr bundle` sets
  `TEGRA_UEFI_LOWEST_SUPPORTED_VERSION` to it, not to each capsule's own version. Raising it is
  a deliberate, reviewed step in a release, for robots fused for secure boot or for a
  vulnerability to lock out for good. The applier keeps refusing capsules below the ESRT's
  floor before it stages anything (N13).
- **Independent of both: the release signature covers the capsule.** The capsule and its
  firmware version are listed, with their hashes, in the signed release manifest. In the
  spike, they go into the signed SWUpdate description, and `install` checks the staged
  capsule against that hash, not against whatever file is there.

## Consequences

- The Jetson board package gains a UEFI change: a `.bbappend` that builds the company's
  certificate into `PcdFmpDevicePkcs7CertBufferXdr`. A test shows that a capsule signed with
  edk2's test key is refused (E5's check, with a validly signed foreign capsule), and that
  the company's capsule is accepted.
- `fjarr provision` and the release CI need a capsule key alongside the release key:
  generation, a hardware-token or signing-server path through `sign_uefi_capsules`, backup,
  and rotation by a capsule carrying the new certificate.
- A lost capsule key leaves reflash over RCM as the only way to change the firmware, which
  needs a person at each robot. The docs say so where the key is created.
- The release format gains signed advisories (which components and versions a vulnerability
  affects, its severity, a link) and a firmware floor field. Fjarr Cloud shows advisories in
  targeting and in the update prompt; `fjarr-update` shows them for local installs.
  Tooling may flag L4T releases whose notes list security fixes, so a floor or an advisory
  is not left to memory.
- Firmware downgrades within the floor follow docs/32's signed downgrade authorization like
  any other component.
- Until this is built, the spike keeps signing capsules with meta-tegra's test keys, and the
  lab's capsule check (L4, `capsule-check.py`) holds a capsule to the firmware a flash wrote.
- Revisit when robots are fused for secure boot (the floor then becomes the main protection,
  and the capsule key sits beside the bootloader's own PKC chain), or if edk2-nvidia starts
  taking the trusted certificate from a source other than the UEFI build.
