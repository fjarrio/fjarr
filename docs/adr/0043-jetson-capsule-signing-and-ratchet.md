---
title: "ADR 0043: Jetson Capsule Signing and the Firmware Ratchet"
description: Which certificate a Jetson's UEFI trusts for bootloader capsules, who holds the signing key, and where the firmware's lowest supported version sits; proposed after phase E of slice 8.0 measured both on the ZED Box Mini.
---

- **Status**: proposed (Q11 in [docs/32](../32-fleet-ota.md#jetson-test-image); Erik decides)
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

**a. meta-tegra's default:** every capsule's floor is its own version.

- Pro: no older firmware can ever come back through a capsule.
- Con: it contradicts the signed downgrade authorization. A deliberate downgrade to a
  release with older firmware fails at UEFI, after the slot was written, with no way
  forward but a newer release.
- Con: as E4 showed, one capsule raises the floor for the whole box, so the first
  firmware update of a product decides all its later rollbacks.

**b. The floor as an explicit release property.** A release states its firmware floor.
It stays at the product's first shipped firmware unless a release is marked as a
security floor, which raises it on purpose (`TEGRA_UEFI_LOWEST_SUPPORTED_VERSION` set
from the release, not from the firmware version). The applier keeps refusing capsules
below the ESRT's floor before it stages anything (N13).

- Pro: downgrades within the floor work as docs/32 designs them, and a fixed
  vulnerability is still locked out once it matters.
- Con: someone must decide, per release, whether it raises the floor. A floor raised
  by mistake cannot be lowered.

**c. No floor.**

- Pro: nothing to decide.
- Con: a signed capsule with a known firmware vulnerability can be installed again
  forever; that rules it out for production.

## Decision

**Proposed: C and b.** Erik decides; nothing below is built yet.

- **The capsule key is the robot company's**, a build input of the board package, with
  a Fjarr-managed key per tenant as the easy start. This is the custody model docs/32
  already chose for release keys, so the firmware gains no party able to forge it that
  the releases do not already have. A production build refuses edk2's test root; lab
  and prototype builds keep it, labelled as such.
- **The firmware floor is set by the release**, not by every capsule: by default the
  product's first shipped firmware, raised only by a release marked as a security floor.
  That keeps docs/32's signed downgrade authorization meaningful for firmware too. The
  applier's pre-flight refusal below the ESRT's floor (N13) stays.
- **Independent of both: the release signature covers the capsule.** The capsule and its
  firmware version are listed, with their hashes, in the signed release manifest. In
  the spike, they go into the signed SWUpdate description, and `install` checks the
  staged capsule against that hash, not against whatever file is there.

## Consequences

- The Jetson board package gains a UEFI change: a `.bbappend` that builds the company's
  certificate into `PcdFmpDevicePkcs7CertBufferXdr`. A test shows that a capsule signed
  with edk2's test key is refused (E5's check, with a validly signed foreign capsule),
  and that the company's capsule is accepted.
- `fjarr provision` and the release CI need a capsule key alongside the release key:
  generation, a hardware-token or signing-server path through `sign_uefi_capsules`,
  backup, and rotation by a capsule carrying the new certificate.
- The release format gains a firmware floor field, and `fjarr bundle` sets
  `TEGRA_UEFI_LOWEST_SUPPORTED_VERSION` from it rather than from the firmware version.
  Raising it is a deliberate, reviewed step in the release.
- A lost capsule key leaves reflash over RCM as the only way to change the firmware,
  which needs a person at each robot. The docs must say so where the key is created.
- In the spike, until this is accepted, capsules stay signed with meta-tegra's test keys
  (Q11), and the lab's capsule check (L4, `capsule-check.py`) is what holds a capsule to
  the firmware a flash wrote.
- Revisit if NVIDIA's secure boot fusing becomes part of the product (it would add the
  bootloader's own PKC chain, and the capsule key would sit beside it), or if edk2-nvidia
  starts taking the trusted certificate from a source other than the UEFI build.
