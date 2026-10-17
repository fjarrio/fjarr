# applier — the Jetson applier spike (M8 slice 8.0, phase A)

The applier for the Yocto recipe on the ZED Box Mini, specified in
[docs/32#appliers](../../../docs/32-fleet-ota.md#appliers) and
[docs/32#jetson-test-image](../../../docs/32-fleet-ota.md#jetson-test-image). It keeps the
x86 applier's verbs and calling convention
([`spikes/ota-x86/image-ebg/mkosi.extra/usr/lib/fjarr/applier`](../../ota-x86/image-ebg/mkosi.extra/usr/lib/fjarr/applier))
and leaves the boot state NVIDIA's: L4TLauncher's retry counter, the boot-chain variables and
`nvbootctrl`. Nothing here has run on the box yet. Phase D measures what this README takes from
NVIDIA's source (D2, D3), and its findings change the code, not the other way round.

| File | Installed as | What |
|---|---|---|
| `applier` | `/usr/lib/fjarr/applier` | the verbs below (bash) |
| `fjarr-trial.service` | a system unit, enabled | early boot: counts a trial boot; the second one rolls back |
| `fjarr-bless.service` | a system unit, not enabled | a trial's confirmation: `applier mark-good` once `boot-complete.target` is reached; `fjarr-trial` starts it |
| `fjarr-boot-verify.service` | a system unit, enabled | `nvbootctrl verify` on every boot that is not a trial; replaces meta-tegra's `nv_update_verifier.service` |
| `test.sh`, `stubs/` | — | the tests, off the box |

```bash
docker run --rm --network none -u "$(id -u):$(id -g)" \
  -v "$PWD/spikes/ota-jetson/applier:/a:ro" fjarr-dev bash /a/test.sh
(cd spikes/ota-jetson/applier && docker run --rm -v "$PWD:/mnt:ro" -w /mnt \
  koalaman/shellcheck:stable -x applier test.sh stubs/*)
```

## Verbs

Every verb but `state` and `is-trial` takes `/run/fjarr-applier.lock`. A refusal exits 2 and
writes nothing; a failure after the first write exits 1.

| Verb | What it does |
|---|---|
| `install <v> <dir>` | **Pre-flight:** see [Refusals](#refusals). **Then:** marks the other slot unbootable, runs `swupdate -v -B none --digest-provider opensslCMS -k /etc/fjarr/swu.crt -e fjarr,slot-<other> -i <dir>/fjarr_<v>.swu` (as on x86, through `systemd-run` with `TMPDIR=/data/fjarr-update/tmp`), checks the slot's ext4 label is `<IMAGE_ID>_<v>`, records the slot and marks it bootable again. Never touches the ESP, the QSPI or the chains. A capsule stays in `<dir>`, the cache |
| `set-next <v>` | The other slot boots on its own chain, so the chain must hold the release's firmware: when its marker is missing or not `complete`, or the chain holds another version (by `SystemFwVersions`, the firmware's reading of the chain at this boot; by the marker when that variable is absent), the capsule goes (copied to the ESP as `TEGRA_BL.Cap.tmp`, `sync -f`, renamed `TEGRA_BL.Cap`, `sync -f`, `OsIndications` bit 2 set); otherwise `nvbootctrl set-active-boot-slot <other>`. Never both. The trial marker comes first, and the chain is marked `in-progress` before the capsule is staged |
| `state` | JSON: the booted version and slot; per slot its label, install record, `RootfsStatusSlot` and retry count; per chain its firmware version (`SystemFwVersions`, `?` when unreadable), its marker and BootROM status; the trial and the last trial; the UEFI variables (`BootChainFw*` with `ResetCount` and `Previous`, `BootChainOsCurrent`, `RootfsStatusSlot*`, `RootfsRetryCountMax`, `RootfsRedundancyLevel`, `L4TDefaultBootMode`, `OsIndications`, `BootNext`); the PMC nodes; the ESRT; `bios_version`; the capsule status; both `nvbootctrl dump-slots-info` outputs |
| `mark-good` | After health, on the trial's slot. When the trial staged a capsule: "Capsule update status" must be 1. Always: the running firmware (`bios_version`) must be the trial's. Then `nvbootctrl verify`, the chain marked `complete`, a finished switch's `BootChainFwStatus` cleared, and the trial marker renamed `last-trial`: the commit point. Without a trial: "not a trial boot", exit 0, as on x86 |
| `rollback-now [--mark-bad]` | `nvbootctrl set-active-boot-slot <other>`, then `systemctl reboot`. An unconfirmed trial is always marked unbootable first, as x86 marks it `FAILED`; `--mark-bad` does the same from a committed slot. A `BootChainFwStatus` still set is cleared first, or the firmware would not switch ([below](#from-the-source)) |
| `is-trial` | exit 0 when a trial marker names the booted slot |
| `trial-boot` | `fjarr-trial.service`. With a trial marker: on another slot than the trial's, the trial is over (`last-trial`, `result=fell-back`); on the trial's slot, `boots` + 1, and at 2 `rollback-now`, otherwise `systemctl start --no-block fjarr-bless.service` |
| `boot-verify` | `fjarr-boot-verify.service`. Without a trial marker: `nvbootctrl verify`, this boot stamped verified, this chain's marker `complete` on the running firmware, a finished switch's `BootChainFwStatus` cleared |

**The release directory** (`<dir>`, in the cache on `/data`): `fjarr_<v>.swu` (the signed
SWUpdate package), `fjarr_<v>.fw` (the L4T version of the bootloader the release runs on, one
line, such as `36.5.2`) and, optionally, `fjarr_<v>.cap` (meta-tegra's `tegra-bl.cap`).

**The state on `/data/fjarr-update/applier/`**, `key=value` files replaced atomically (a temp
file beside, `sync -f`, rename, `sync -f`):

| File | Keys | Written by |
|---|---|---|
| `slot-a`, `slot-b` | `version`, `fw`, `capsule` (its path in the cache), `capsule_sha256` | `install`; removed while the slot is rewritten |
| `chain-a`, `chain-b` | `fw`, `state` (`complete` or `in-progress`) | `set-next` (`in-progress`), `mark-good` and `boot-verify` (`complete`). The marker says whether Fjarr's last write of the chain completed; the version comes from the firmware where it can (`SystemFwVersions`), since the marker cannot see a reflash. A missing marker means unknown and forces a capsule, as tegra-demo-distro's capsule markers do, so the lab's flash (D1) should seed both, with the version it wrote to both chains |
| `trial` | `version`, `slot`, `previous`, `boots`, `capsule` (0 or 1), `fw` | `set-next` (`boots=0`), `trial-boot` |
| `last-trial` | the trial's keys and `result` (`good` or `fell-back`), written into `trial` before the rename, so a cut never leaves a `last-trial` without its result | `mark-good`, `trial-boot` |

`/run/fjarr-applier/verified` marks a boot that passed `nvbootctrl verify`.

## What each verb writes

`test.sh` runs each row from the same starting point and checks that the applier makes exactly
the writes in the last column, in that order. The start: 1.0 booted on slot A and verified,
both chains marked `complete` on 36.5.2, release 1.2 staged with its capsule, the UEFI variables
a box flashed for root A/B has (`RootfsRetryCountMax` 3, `RootfsRedundancyLevel` 1), NVIDIA's
variables immutable, both retry counters at 3. Each row names what differs.

| Token | Means |
|---|---|
| `efi:<name>=<hex>` | the UEFI variable written whole through efivarfs, after `chattr -i` and followed by `chattr +i` for NVIDIA's: 4 bytes of attributes, then the data, little-endian (`07000000ff000000`: non-volatile, boot and runtime access; 0xFF) |
| `efi:-<name>` | the variable deleted, right after `chattr -i` |
| `pmc:<node>=<value>` | a PMC scratch node written, as `0x%x` |
| `data:<path>`, `data:<a>><b>`, `data:-<path>` | under `/data/fjarr-update/`: a file replaced atomically, renamed, removed; each followed by `sync -f` (not listed) |
| `esp:<path>`, `esp:<a>><b>` | under `/boot/efi`: a file copied, renamed |
| `sync:esp` | `sync -f` of the ESP |
| `run:verified` | `/run/fjarr-applier/verified` |
| `swupdate:<selection>`, `part:<label>` | `swupdate -e <selection>`, and the partitions its package writes (the test's stub stands for the [package](#the-swupdate-package)) |
| `nvbootctrl:<args>`, `systemctl:<args>` | the command run |

| Case | Verb, and how the state differs from the start | Writes, in order |
|---|---|---|
| S1 | `state` | |
| I1 | `install 1.2 <dir>` | `efi:RootfsStatusSlotB=07000000ff000000` `pmc:rootfs_retry_count_b=0x0` `swupdate:fjarr,slot-b` `part:APP_b` `part:B_kernel` `part:B_kernel-dtb` `data:applier/slot-b` `efi:RootfsStatusSlotB=0700000000000000` `pmc:rootfs_retry_count_b=0x3` |
| I2 | `install`, SWUpdate fails after `APP_b`: exit 1, slot B stays unbootable | `efi:RootfsStatusSlotB=07000000ff000000` `pmc:rootfs_retry_count_b=0x0` `swupdate:fjarr,slot-b` `part:APP_b` |
| I3 | `install` of a release without a capsule | `efi:RootfsStatusSlotB=07000000ff000000` `pmc:rootfs_retry_count_b=0x0` `swupdate:fjarr,slot-b` `part:APP_b` `part:B_kernel` `part:B_kernel-dtb` `data:applier/slot-b` `efi:RootfsStatusSlotB=0700000000000000` `pmc:rootfs_retry_count_b=0x3` |
| I4 | `install` over 1.1 in slot B: its record goes before the slot is written | `efi:RootfsStatusSlotB=07000000ff000000` `pmc:rootfs_retry_count_b=0x0` `data:-applier/slot-b` `swupdate:fjarr,slot-b` `part:APP_b` `part:B_kernel` `part:B_kernel-dtb` `data:applier/slot-b` `efi:RootfsStatusSlotB=0700000000000000` `pmc:rootfs_retry_count_b=0x3` |
| I5 | `install`, the written slot is not labelled `fjarr_1.2`: exit 1, slot B stays unbootable | `efi:RootfsStatusSlotB=07000000ff000000` `pmc:rootfs_retry_count_b=0x0` `swupdate:fjarr,slot-b` `part:APP_b` `part:B_kernel` `part:B_kernel-dtb` |
| N1 | `set-next 1.2` after I1; chain B `complete` on 36.5.2: no capsule | `data:applier/trial` `nvbootctrl:set-active-boot-slot 1` |
| N2 | `set-next 1.2` after I1; chain B on 36.4.4: the capsule | `data:applier/trial` `data:applier/chain-b` `esp:EFI/UpdateCapsule/TEGRA_BL.Cap.tmp` `sync:esp` `esp:EFI/UpdateCapsule/TEGRA_BL.Cap.tmp>EFI/UpdateCapsule/TEGRA_BL.Cap` `sync:esp` `efi:OsIndications=070000000400000000000000` |
| N3 | `set-next 1.2` after I1; no marker for chain B | `data:applier/trial` `data:applier/chain-b` `esp:EFI/UpdateCapsule/TEGRA_BL.Cap.tmp` `sync:esp` `esp:EFI/UpdateCapsule/TEGRA_BL.Cap.tmp>EFI/UpdateCapsule/TEGRA_BL.Cap` `sync:esp` `efi:OsIndications=070000000400000000000000` |
| N4 | `set-next 1.2` after I1; chain B `in-progress` on 36.5.2 | `data:applier/trial` `data:applier/chain-b` `esp:EFI/UpdateCapsule/TEGRA_BL.Cap.tmp` `sync:esp` `esp:EFI/UpdateCapsule/TEGRA_BL.Cap.tmp>EFI/UpdateCapsule/TEGRA_BL.Cap` `sync:esp` `efi:OsIndications=070000000400000000000000` |
| N5 | as N2, with `OsIndications` already 0x10: its other bits stay | `data:applier/trial` `data:applier/chain-b` `esp:EFI/UpdateCapsule/TEGRA_BL.Cap.tmp` `sync:esp` `esp:EFI/UpdateCapsule/TEGRA_BL.Cap.tmp>EFI/UpdateCapsule/TEGRA_BL.Cap` `sync:esp` `efi:OsIndications=070000001400000000000000` |
| N6 | as N2, the ESP cannot be written: exit 1; nothing was armed, so the trial ends here | `data:applier/trial` `data:applier/chain-b` `data:-applier/trial` |
| N7 | as N1, `nvbootctrl set-active-boot-slot` fails: exit 1; the marker stays, and the next boot ends the trial if no switch was made | `data:applier/trial` |
| N8 | as N1, but `SystemFwVersions` reads 36.4.4 from chain B while its marker says 36.5.2: the firmware's reading wins | `data:applier/trial` `data:applier/chain-b` `esp:EFI/UpdateCapsule/TEGRA_BL.Cap.tmp` `sync:esp` `esp:EFI/UpdateCapsule/TEGRA_BL.Cap.tmp>EFI/UpdateCapsule/TEGRA_BL.Cap` `sync:esp` `efi:OsIndications=070000000400000000000000` |
| N9 | as N1, `SystemFwVersions` cannot read chain B (0xFFFFFFFF) | `data:applier/trial` `data:applier/chain-b` `esp:EFI/UpdateCapsule/TEGRA_BL.Cap.tmp` `sync:esp` `esp:EFI/UpdateCapsule/TEGRA_BL.Cap.tmp>EFI/UpdateCapsule/TEGRA_BL.Cap` `sync:esp` `efi:OsIndications=070000000400000000000000` |
| T1 | `trial-boot` without a trial | |
| T2 | `trial-boot`, booted slot B for the trial, `boots=0` | `data:applier/trial` `systemctl:start --no-block fjarr-bless.service` |
| T3 | `trial-boot`, booted slot B for the trial, `boots=1`; `BootChainFwStatus=0` left by the switch | `data:applier/trial` `efi:RootfsStatusSlotB=07000000ff000000` `pmc:rootfs_retry_count_b=0x0` `efi:-BootChainFwStatus` `nvbootctrl:set-active-boot-slot 0` `systemctl:reboot` |
| T4 | `trial-boot`, booted slot A while the trial is on slot B | `data:applier/trial` `data:applier/trial>applier/last-trial` |
| T5 | as T3, slot A marked unbootable: counted, then refused (exit 2) | `data:applier/trial` |
| T6 | as T3, `nvbootctrl set-active-boot-slot` fails: it reboots anyway, and L4TLauncher leaves slot B, whose counter is 0 | `data:applier/trial` `efi:RootfsStatusSlotB=07000000ff000000` `pmc:rootfs_retry_count_b=0x0` `efi:-BootChainFwStatus` `systemctl:reboot` |
| M1 | `mark-good` without a trial | |
| M2 | `mark-good`, booted slot B, a trial without a capsule; `BootChainFwStatus=0` left by the switch | `nvbootctrl:verify` `run:verified` `efi:-BootChainFwStatus` `data:applier/trial` `data:applier/trial>applier/last-trial` |
| M3 | `mark-good`, booted slot B, a capsule trial; capsule status 1, `bios_version` 36.5.2-gcid-… | `nvbootctrl:verify` `run:verified` `data:applier/chain-b` `data:applier/trial` `data:applier/trial>applier/last-trial` |
| M4 | as M3, capsule status 3: exit 1 | |
| M5 | as M3, `bios_version` v36.4.4: exit 1 | |
| M6 | as M3, `bios_version` v36.5.2, the form meta-tegra's firmware build gives | `nvbootctrl:verify` `run:verified` `data:applier/chain-b` `data:applier/trial` `data:applier/trial>applier/last-trial` |
| V1 | `boot-verify`, this boot not yet verified | `nvbootctrl:verify` `run:verified` |
| V2 | `boot-verify` on a trial boot | |
| V3 | `boot-verify` after a chain switch: `BootChainFwStatus=0`, no marker for chain A | `nvbootctrl:verify` `run:verified` `data:applier/chain-a` `efi:-BootChainFwStatus` |
| V4 | `boot-verify` with `BootChainFwStatus=12` (`STATUS_ERROR_BOOT_CHAIN_FAILED`): left for a person | `nvbootctrl:verify` `run:verified` |
| R1 | `rollback-now`, booted slot B, committed | `nvbootctrl:set-active-boot-slot 0` `systemctl:reboot` |
| R2 | `rollback-now --mark-bad`, booted slot B, committed | `efi:RootfsStatusSlotB=07000000ff000000` `pmc:rootfs_retry_count_b=0x0` `nvbootctrl:set-active-boot-slot 0` `systemctl:reboot` |
| R3 | `rollback-now` during the trial on slot B; `BootChainFwStatus=0` left by the switch | `efi:RootfsStatusSlotB=07000000ff000000` `pmc:rootfs_retry_count_b=0x0` `efi:-BootChainFwStatus` `nvbootctrl:set-active-boot-slot 0` `systemctl:reboot` |
| R4 | as R1, `nvbootctrl set-active-boot-slot` fails: exit 1, no reboot, since nothing would change | |

### Refusals

Exit 2, nothing written. `install` and `set-next` share the firmware checks (I15–I22): the ones
docs/32 names (no `BootChainFwStatus`, `BootChainFwNext` or `BootNext`; `BootChainFwResetCount`
below 2, where the firmware stops at 3), then a pending capsule, the recovery boot mode, the PMC
nodes and the booted slot. Both also need this boot verified and no trial in progress.

| Case | Verb | Refused because |
|---|---|---|
| I10 | `install 1.0` | 1.0 is the booted version |
| I11 | `install 1.3` | no `fjarr_1.3.swu` in the directory |
| I12 | `install 1.2` | no `fjarr_1.2.fw` |
| I13 | `install 1.2` | a trial (of 1.1) is in progress |
| I14 | `install 1.2` | this boot has not been verified |
| I15 | `install 1.2` | `BootChainFwStatus` is set (12) |
| I16 | `install 1.2` | `BootChainFwNext` is set |
| I17 | `install 1.2` | `BootNext` is set (lab rule L5) |
| I18 | `install 1.2` | `BootChainFwResetCount` is 2 (it is accepted at 1, and when absent, as it normally is) |
| I19 | `install 1.2` | a capsule is pending (`OsIndications` bit 2) |
| I20 | `install 1.2` | `L4TDefaultBootMode` is recovery (3) |
| I21 | `install 1.2` | the firmware's current slot (B) is not the root partition's (A) |
| I22 | `install 1.2` | the PMC retry counter nodes are missing |
| N10 | `set-next 1.3` | 1.3 is not installed in slot B |
| N11 | `set-next 1.2` | chain B needs a capsule and the release has none |
| N12 | `set-next 1.2` | the capsule in the cache changed since `install` |
| N13 | `set-next 1.2` | the capsule (36.5.2) is below the ESRT's lowest supported version |
| N14 | `set-next 1.2` | slot B is marked unbootable |
| N15 | `set-next 1.2` | slot B's retry counter is 0 |
| N16 | `set-next 1.2` | a trial is already pending |
| N17 | `set-next 1.2` | `BootChainFwStatus` is set (0): a switch not yet settled |
| M10 | `mark-good` | slot A booted, the trial is on slot B |
| R10 | `rollback-now` | slot A is marked unbootable |
| R11 | `rollback-now` | chain A's last capsule is `in-progress` |
| R12 | `rollback-now` | `BootNext` is set: the switch would fail |

### End to end

`test.sh` then drives whole updates through a model of the firmware, read from the source below:
the capsule on disk, BootChainDxe's switch (which ends without switching when a status is
already set), L4TLauncher's retry counters (seeded at a cold boot from `RootfsStatusSlot`, one
taken per boot, a slot at 0 marked unbootable and left for the other chain). `nvbootctrl` is
modelled at its least: `set-active-boot-slot` writes `BootChainFwNext`, `verify` gives the
current slot its retries back.

| Run | Checks |
|---|---|
| U1 | an update without a capsule boots, is confirmed, stays after a reboot, and `rollback-now` returns to slot A |
| U2 | a failed trial rolls back at its second boot; the next update into the same slot boots, because `install` re-arms its retry counter |
| U3 | a capsule moves chain B from 36.4.4 to 36.5.2 with slot B; `mark-good` marks chain B `complete` |
| U4 | a power cut during the trial rolls back: the cold boot restores slot B's retries, the trial counter does not reset |
| U5 | a power cut after `set-next`'s first two writes leaves slot A booting, the trial ended, and the next `set-next` stages the capsule |
| U6 | `rollback-now` from a committed slot switches although a finished switch's `BootChainFwStatus` is still set |
| U7 | at the trial's second boot `nvbootctrl` fails to switch; the reboot still ends on slot A, through L4TLauncher's own fallback |

## The units

`fjarr-trial.service` runs early, once `/data` is mounted, only when a trial marker exists.
`fjarr-bless.service` has no `[Install]`: as on x86, only a trial pulls `boot-complete.target`
in, with the image's health units (`RequiredBy=boot-complete.target`) and its deadline
(`JobTimeoutAction=reboot-force`). A failed health check reboots (the image's x86-style
`fjarr-mark-bad`), and so does a failed `mark-good` (`FailureAction=reboot`): either way the next
boot is the trial's second, which rolls back. `fjarr-boot-verify.service` runs on every other
boot after `multi-user.target`, and after `boot-complete.target` when something pulls it in; it
does not wait for health, so a committed slot is never rolled back by a failing probe, only by
three warm resets before `multi-user.target`.

The image must mask meta-tegra's `nv_update_verifier.service`, which verifies every boot at
`multi-user.target`, the trial too. Both enabled units order after meta-tegra's
`setup-nv-boot-control.service`: `nvbootctrl` reads `/etc/nv_boot_control.conf` (the chip id and
the board spec), which that service writes into `/run` at boot (meta-tegra
`tegra-nv-boot-control-config`); NVIDIA's own image ships the file instead. D1 confirms that
`nvbootctrl` answers that early.

## From the source

What the applier relies on, read in edk2-nvidia `r36.5.1` (meta-tegra `scarthgap` builds its
36.5.2 firmware from it; the files and logic are the same in `r36.4.4`), meta-tegra `scarthgap`
(6d7cc8f), tegra-demo-distro `scarthgap` (874b487), NVIDIA's Jetson Linux R36.5.2 BSP and its
R36.4.4 developer guide. The box confirms each in phase D.

**Already seen on the stock box** (phase B's read-only state dump, 2026-10-17, L4T 36.4.4 with
root A/B off): the attributes and sizes below for `RootfsStatusSlotA`/`B` (7, UINT32, 0),
`RootfsRetryCountMax` (6, 3), `RootfsRedundancyLevel` (6, 0), `BootChainFwCurrent` and
`BootChainOsCurrent` (6), `L4TDefaultBootMode` (7, 1); `SystemFwVersions` present;
`BootChainFwNext`, `BootChainFwStatus`, `BootChainFwPrevious`, `BootChainFwResetCount`,
`OsIndications` and `BootNext` absent; the PMC nodes at `/sys/devices/platform/bus@0/c360000.pmc`
in the `0x%x` form, all 0 with root A/B off; `bios_version` in the `36.4.4-gcid-<n>` form; the
ESRT's `fw_version` 0x00240404; `nvbootctrl get-current-slot` printing `0`, and
`dump-slots-info` in the form `stubs/nvbootctrl` prints. With root A/B off, `nvbootctrl -t
rootfs dump-slots-info` prints only that it is not enabled, so the stub's rootfs form is a guess
the applier never parses (`state` passes it through). In R36.5.2, NVIDIA's own image verifies a
boot with `nvbootctrl verify` alone, "both bootloader and rootfs" by its comment
(`nv-l4t-bootloader-config.sh`, `nvidia-l4t-init`).

**The variables** (efivarfs: `/sys/firmware/efi/efivars/<name>-<guid>`, 4 bytes of attributes
then the data, all little-endian; NVIDIA's are created immutable, so a write needs `chattr -i`
and one `write()` of the whole file, as NVIDIA's `nv_ota_common.func` does with `dd`):

| Variable | GUID | Attributes | Data | Values | Source |
|---|---|---|---|---|---|
| `RootfsStatusSlotA`, `B` | `781e084c-…` (`gNVIDIAPublicVariableGuid`) | NV, BS, RT (7) | UINT32 | 0 normal, 0xFF unbootable | `L4TRootfsValidation.c` 27–39; `NVIDIA.common.dsc.inc` 1195–1196; `NVIDIAConfiguration.h` 23–24; `nv_ota_common.func` `reset_rootfs_chain_status` |
| `RootfsRetryCountMax` | `781e084c-…` | BS, RT (6), volatile and locked: created each boot from the firmware's device tree, read-only after | UINT32 | 0–3, default 3 | `L4TConfiguration.dts` 35–39; `DefaultVariableDxe.c` 194–262 |
| `RootfsRedundancyLevel` | `781e084c-…` | BS, RT (6), volatile and locked | UINT32 | 0 boot only, 1 root A/B | `L4TConfiguration.dts` 40–44; `NVIDIAConfiguration.h` 26–27; meta-tegra `L4TConfiguration-RootfsRedundancyLevelABEnable.dtsi` |
| `BootChainFwCurrent` | `781e084c-…` | BS, RT (volatile) | UINT32 | 0 chain A, 1 chain B; set each boot | `BootChainDxe.c` 31–35, 669 |
| `BootChainFwNext` | `781e084c-…` | NV, BS, RT | UINT32 | the chain to switch to; deleted when the switch ends | `BootChainDxe.c` 36–41, 527–530 |
| `BootChainFwStatus` | `781e084c-…` | NV, BS, RT | UINT32 | `STATUS_*`: 0 success, 1 in progress, 3 cancelled for a capsule, 12 the new chain failed, 14 `BootNext` set, … 18 | `BootChainDxePrivate.h` 21–39 |
| `BootChainFwPrevious` | `ed3374ef-767b-42fa-af70-db520a392822` (`gNVIDIATokenSpaceGuid`) | NV, BS: **not visible to Linux** | UINT32 | the chain before the switch | `BootChainDxe.c` 48–52 |
| `BootChainFwResetCount` | `ed3374ef-…` | NV, BS: **not visible to Linux** | UINT32 | switches started, refused at 3; deleted when a switch ends | `BootChainDxe.c` 53–57, 387–412, 527–530; `BootChainDxePrivate.h` 19 |
| `BootChainOsCurrent` | `781e084c-…` | BS, RT (volatile) | UINT32 | the slot L4TLauncher booted | `L4TLauncher.c` 1812 |
| `SystemFwVersions` | `781e084c-…` | BS, RT (6), volatile: set each boot | UINT32[2]: chain A, chain B | each chain's version from its VER partition, `0x00BBMMmm` as in the ESRT; `0xFFFFFFFF` when unreadable | `FmpVersionLibPartition.c` 41–60, 214 (r36.5.1), `TegraFmp.c` 333–354, 493 (r36.4.4); `VerPartitionLib.c` 174–180; NVIDIA's `nv-l4t-bootloader-config.sh` `get_bl_version` (R36.5.2 `nvidia-l4t-init`) |
| `L4TDefaultBootMode` | `781e084c-…` | NV, BS, RT | UINT32 | 1 direct (normal), 3 recovery | `NVIDIAConfiguration.h` 32–36; `nv_ota_common.func` `set_boot_mode` |
| `OsIndications` | `8be4df61-93ca-11d2-aa0d-00e098032b8c` (UEFI global) | NV, BS, RT | UINT64 | bit 2: a capsule on disk | NVIDIA developer guide R36.4.4, "Update and Redundancy"; meta-tegra `oe4t-set-uefi-OSIndications` |
| `BootNext` | `8be4df61-…` | NV, BS, RT | UINT16 | a one-shot boot entry | UEFI specification |

**Findings that change what docs/32 expects:**

- **`BootChainFwResetCount` cannot be checked from Linux,** although docs/32 has the applier
  check it. BootChainDxe declares it, and `BootChainFwPrevious`, without runtime access and
  under another GUID, so efivarfs never shows them. The applier checks the count only if it ever
  appears; by the source it is deleted at the end of every switch, successful or not, so it
  reaches 3 only within one switch's resets. The stock box's efivarfs has neither (phase B's
  read-only dump, 2026-10-17); D2 checks again during a switch.
- **Each chain's firmware version is readable from Linux.** UEFI reads both chains' VER
  partitions at every boot into `SystemFwVersions`, which NVIDIA's own `nv-l4t-bootloader-config.sh`
  uses to compare its chains. `set-next` takes the other chain's version from it, so a reflash or a
  capsule outside Fjarr cannot leave a stale marker deciding, and an unreadable chain gets a capsule.
  The marker still decides completeness: whether a half-written chain's VER already shows the new
  version depends on the order the capsule writes the partitions in, which E5 measures.
- **A finished switch leaves `BootChainFwStatus` set.** BootChainDxe writes `SUCCESS` (0) after a
  switch and never deletes it (lines 338–354, 491–530). Left set, it cancels the next capsule
  (`BootChainCheckAndCancelUpdate`, 220–254) and makes the next `set-active-boot-slot` end without
  switching (an existing status sends `ExecuteUpdate` to `FinishUpdate`, which deletes
  `BootChainFwNext`). L4TLauncher deletes it before its own fallback (`L4TRootfsValidation.c`
  890). So `mark-good` and `boot-verify` clear a `SUCCESS` once the boot is known good, and
  `rollback-now` clears any status before switching. Whether `nvbootctrl` already does either
  is measured in D2; clearing an absent variable is a no-op.
- **`RootfsStatusSlot` alone does not make a slot unbootable.** L4TLauncher reads it only to seed
  the retry counters when the scratch register's magic is missing (255–303); every other boot
  decides by the counter (`IsRootfsSlotBootable`, 520–553). The register outlives a warm reset:
  L4TLauncher clears it by hand before its own fallback reset (878), and UEFI's chain-switch
  reset leaves it on this chip (`StatusRegReset` acts only on TH500, `StatusRegLib.c` 17, 35).
  So the applier writes both: the status variable and the counter through NVIDIA's PMC nodes
  (`rootfs_retry_count_a`/`_b`, `0x%x`, in meta-tegra's
  `0009-NVIDIA-SAUCE-soc-tegra-pmc-Add-sysfs-nodes-to-select.patch`). Without the counter, a
  slot whose trial failed by warm resets keeps a counter of 0 after it is rewritten, and the
  next switch to it falls straight back (U2 checks it). D2 measures which resets keep the
  register, and whether `nvbootctrl verify` resets the counter or the magic.
- **`bios_version` has two forms.** NVIDIA's prebuilt firmware reports `36.5.2-gcid-<n>`;
  meta-tegra's source build reports `v36.5.2` (`BUILDID_STRING=v${PV}`,
  `edk2-firmware-tegra-36.5.2.inc` 86, into SMBIOS type 0 through `PcdFirmwareVersionString`,
  `SmbiosType0Parser.c` 187). The applier compares the version between an optional `v` and the
  first `-`. The ESRT's `fw_version` is `0x00BBMMmm` (meta-tegra `lib/oe4t/uefi.py`).
- **Ordering against power cuts.** `set-next` writes the trial marker before anything that can
  switch, so the boot a switch leads to is always a trial; and the chain's `in-progress` marker
  before the capsule is staged, so a capsule applied after a cut is never mistaken for a
  complete chain (tegra-demo-distro's Lua writes its marker after `OsIndications`).

## The SWUpdate package

`install` needs a package with two selections, `fjarr,slot-a` and `fjarr,slot-b`, each writing
only that slot: the raw ext4 root to `APP` or `APP_b`, the Android boot image to `A_kernel` or
`B_kernel`, the device tree to `A_kernel-dtb` or `B_kernel-dtb`, by
`/dev/disk/by-partlabel/…`. Never the ESP. The image builds it in phase A, signed as on x86. For the
image, also:

- `/etc/os-release` with `IMAGE_ID` and `IMAGE_VERSION`, and the root's ext4 label
  `<IMAGE_ID>_<IMAGE_VERSION>`, as on x86;
- SWUpdate built with CMS signatures and the `none` bootloader (2025.12 builds it with
  `CONFIG_BOOTLOADER_NONE`, or when no U-Boot, GRUB or EFI Boot Guard backend is configured),
  without `swupdate.service` or its socket;
- `nvbootctrl` (NVIDIA's `tegra-redundant-boot`), `chattr` (e2fsprogs), GNU coreutils
  (`sync -f`, `dd status=none`), util-linux (`flock`, `findmnt`, `blkid`, `lsblk`);
- a kernel with NVIDIA's PMC scratch nodes (the patch above; NVIDIA's own kernel carries it),
  which `nvbootctrl` needs too;
- `/boot/efi` an automount of the ESP (the applier makes `/data/fjarr-update/applier` and
  `/data/fjarr-update/tmp` itself);
- meta-tegra's `nv_update_verifier.service` masked.
