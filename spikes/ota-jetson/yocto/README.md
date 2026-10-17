# ota-jetson/yocto — the 8.0 Jetson test image (Yocto)

Spike code for the Yocto image of [docs/32#jetson-test-image](../../../docs/32-fleet-ota.md#jetson-test-image)
("The image"): a kas configuration, and the layer `meta-fjarr-spike` with the ZED Box Mini
machine, a small distro, the slot image and its initrd. Recipes are productised later; nothing
here ships, and M8 publishes recipes, not images ([docs/32](../../../docs/32-fleet-ota.md#x86-test-image),
"Recipes, not images").

Run on the host, never inside `dev`:

```bash
spikes/ota-jetson/yocto/build.sh checkout            # the layers at their pins, into out/yocto/
spikes/ota-jetson/yocto/build.sh dry-run             # parse, then bitbake -n
spikes/ota-jetson/yocto/build.sh start               # the full build, detached; log out/yocto/build.log
spikes/ota-jetson/yocto/build.sh status              # running or not, and the log's tail
spikes/ota-jetson/yocto/build.sh stop
spikes/ota-jetson/yocto/build.sh build [targets]     # in the foreground
spikes/ota-jetson/yocto/build.sh --image-version 1.2 start          # a second release for the update tests
spikes/ota-jetson/yocto/build.sh --unpatched-kernel start           # NVIDIA's watchdog driver (D2b)
```

`start` again resumes an interrupted build from its stamps and the shared-state cache. The
default targets are `fjarr-spike-image` and `tegra-uefi-capsules`; `build` and `start` run
`bitbake -k`, so a failed recipe does not stop the rest and the log's summary lists every
failure. Everything the build writes
is under `spikes/ota-jetson/out/yocto/` (gitignored): the layer checkouts, `downloads/`,
`sstate-cache/`, `build/` (`build-nvwdt/` for `--unpatched-kernel`), and the pinned
`tools/kas-container`. The outputs, in `build/tmp/deploy/images/zedbox-mini-orin-nx16/`:

| File | What |
|---|---|
| `fjarr-spike-image-zedbox-mini-orin-nx16.rootfs-<v>.tegraflash.tar.gz` | the first flash: unpack it, `./initrd-flash` with the box in USB recovery mode (RCM) |
| `fjarr-spike-image-zedbox-mini-orin-nx16.rootfs-<v>.ext4` | the slot image, 16 GiB, labelled `fjarr_<v>`, the raw image a release writes to `APP` or `APP_b` |
| `zedbox-mini-orin-nx16-tegra-bl.cap` (`tegra-bl.cap`) | the bootloader capsule from the same inputs, signed with meta-tegra's EDK2 test certificates (Q11) |
| `fjarr-spike-initramfs-zedbox-mini-orin-nx16.cpio.gz` | the initrd, also in each slot's `/boot` |

**Never flash a build made with `FJARR_BOARD_BCT = "0"`**: its MB2 reads a carrier EEPROM the ZED
Box Mini does not have and stops before UEFI. Every new firmware build boots from RAM over RCM
first (D0, rule L4), which shows such a mistake without writing anything. The images carry the
lab's private SSH host keys: they stay on the lab's machines.

The build runs at most 6 bitbake tasks with `make -j 6`, at nice 19 (the workstation is shared;
Docker already runs containers at nice 12 with idle I/O there), with `rm_work`, and stops when
less than 20 GB is free.

`build.sh` makes the lab identity once, in `spikes/ota-jetson/.lab/` (gitignored):
`.lab/yocto/authorized_keys` (the operator's key, `FJARR_LAB_PUBKEY`, default
`~/.ssh/id_rsa.pub`; append the mini-PC's key when lab scripts reach the box from there),
`.lab/yocto/ssh/` (SSH host keys shared by every slot image), and the SWUpdate test key
`.lab/swu.key` and `.lab/swu.crt` (made only if absent; the image trusts `swu.crt`).

## The pins

| Layer | Branch | Commit | Why |
|---|---|---|---|
| meta-tegra | scarthgap | `6d7cc8f27e8b` (2026-09-29) | L4T 36.5.2 (Q1); the branch head and the commit tegra-demo-distro's scarthgap builds |
| poky | scarthgap | `3a3d07f625ae` (yocto-5.0.20+156) | what tegra-demo-distro's scarthgap (`874b4877`, 2026-10-03) builds meta-tegra `6d7cc8f` with |
| meta-openembedded (meta-oe) | scarthgap | `0f00f8b9a219` | as poky |
| meta-swupdate | scarthgap | `25c9be858d1a` | the branch head (2026-10-16); tegra-demo-distro leaves this layer to the user. The distro pins SWUpdate 2025.12, the version the x86 spike measured |

kas is 5.5 (`kas-container` checked by sha256, image `ghcr.io/siemens/kas/kas:5.5`, Debian 13).
bitbake warns that Debian 13 is not a tested host for scarthgap; OE4T builds this meta-tegra
on Ubuntu 22.04 to 26.04.

## What the image is, item by item

**Machine `zedbox-mini-orin-nx16`** (`conf/machine/`) requires meta-tegra's
`p3509-a02-p3767-0000` and sets the non-super values NVIDIA's own L4T uses for that board, as
Stereolabs ships the box: BPMP DTB `tegra234-bpmp-3767-0000-a02-3509-a02.dtb`, nvpmodel
`nvpmodel_p3767_0000` and kernel DTB `tegra234-p3768-0000+p3767-0000-nv.dtb` (meta-tegra's
`orin-nx.inc` defaults to super mode since L4T 36.4.3).

**The layout.** meta-tegra takes the NVMe table from `PARTITION_LAYOUT_EXTERNAL`
(`PARTITION_LAYOUT_EXTERNAL_REDUNDANT` when `USE_REDUNDANT_FLASH_LAYOUT` is set), which
`tegra-storage-layout-base` installs from L4T's `tools/kernel_flash/`; `image_types_tegra`
fills the slot size from `ROOTFSPART_SIZE` (`ROOTFSPART_SIZE_DEFAULT` halved for A/B). The
machine sets `USE_REDUNDANT_FLASH_LAYOUT_DEFAULT = "1"`, `ROOTFSPART_SIZE_DEFAULT` = 32 GiB
(16 GiB per slot) and its own layout name. NVIDIA's XML is `LicenseRef-NvidiaProprietary`, so
the layer does not carry a modified copy: `tegra-storage-layout-base_%.bbappend` checks NVIDIA's
`flash_l4t_t234_nvme_rootfs_ab.xml` against its sha256 (`ed2b66fb…`, the same in 36.4.4 and
36.5.2) and inserts Fjarr's partition (`fjarr-data-partition.xml`) before `secondary_gpt`:
`fjarr-data`, number 17, allocation attribute `0x808`. meta-tegra's `initrd-flash` writes the
external disk with `make-sdcard`, which creates a `0x808` partition with `sgdisk --largest-new`,
so `/data` takes the rest of the disk, about 205 GiB on the lab box. It writes the same image to
`APP` and `APP_b`, and leaves `/data` unformatted (a `filesystem_type` would make the host run
`mkfs` without `-F`, which stops at an old signature on any reflash): the slot's
`fjarr-data-format` creates ext4 on first boot, only when `blkid -p` finds nothing and no backup
superblock exists, then mounts it at `/data` (not required for boot, so the box stays
reachable). `recovery` and `recovery-dtb` stay empty, as meta-tegra leaves them (the rescue in
`recovery` is phase A5).

**The boot state.** `RootfsRetryCountMax` is 3 (Q5) through `TEGRA_BOOTCONTROL_OVERLAYS`, still
meta-tegra's mechanism for UEFI variable defaults: `fjarr-l4t-config` deploys
`L4TConfiguration-FjarrRootfsRetryCountMax.dtbo` beside the firmware, listed after meta-tegra's
`L4TConfiguration.dtbo` and `L4TConfiguration-RootfsRedundancyLevelABEnable.dtbo`; tegraflash and
the capsule both apply the list. NVIDIA's `L4TConfiguration.dts` (edk2-nvidia r36.5.1, the UEFI
meta-tegra builds) already sets 3, runtime and locked; the overlay keeps the value explicit.
meta-tegra's `nv_update_verifier` stays enabled until the applier's `fjarr-boot-verify.service`
is in the image, then it is masked: without a replacement, a committed slot would use up its
retries in three warm reboots.

**The board package** (`spikes/ota-jetson/board/`, shared with the NixOS recipe). With
`FJARR_BOARD_BCT = "1"` (the default) `tegra-bootfiles_%.bbappend` applies
`board/bct/zedbox-mini-mb1-pinmux-gpio.patch` (`patch -p1 -F0`) to copies of NVIDIA's MB1
pinmux and GPIO files, never to the shared BSP tree, and installs them with Fjarr's
`board/bct/tegra234-mb2-bct-misc-p3767-0000-zedbox-mini.dts` under the names the machine's
`TEGRA_FLASHVAR_PINMUX_CONFIG` and `TEGRA_FLASHVAR_MB2BCT_CFG` select; the flash package and the
capsule both build from them. Replayed against meta-tegra's (dos2unix-converted) 36.5.2 tree
(2026-10-17), the patch applies without fuzz and the three files compile as tegraflash compiles
them (cpp, dtc), with `cvb_eeprom_read_size = <0x00>` in the MB2 tree where NVIDIA's has `0x100`.
With `FJARR_BOARD_DT = "1"` (the default) `zedbox-mini-devicetree`, modelled on
tegra-demo-distro's device-tree recipe, builds `board/dt/tegra234-p3768-0000+p3767-0000-zedbox-mini.dts`
against NVIDIA's device-tree sources from `nvidia-kernel-oot`; it goes into each slot's `/boot`
and the extlinux `FDT` line names it. The board package's own checks cover the rest.

**The root of the booted slot.** The same slot image must boot from `APP` on chain A and `APP_b`
on chain B. meta-tegra's initrd (`tegra-minimal-init`) finds `APP_b` through the UEFI variable
`BootChainOsCurrent`, which L4TLauncher sets, but falls back to `nvme0n1p1` (chain A's root)
after 5 s and ends in a shell nobody sees when the mount fails. This image's initrd is systemd
(`fjarr-spike-initramfs`): a udev rule (`fjarr-slot-root`) names the booted chain's slot
`/dev/fjarr/root` from the same variable, and the command line is
`root=/dev/fjarr/root rootfstype=ext4 rootflags=x-systemd.device-timeout=90s rw panic=10`
(extlinux.conf and the A/B kernel partitions' boot images both take it from `KERNEL_ARGS`).

**A trial that cannot come up resets warm.** systemd opens the watchdog in the initrd
(`RuntimeWatchdogSec=30s`, kept in the slot), the root wait is bounded, a panic reboots after
10 s, and emergency and rescue mode reboot after 30 s instead of waiting, in the initrd and the
slot, as on x86. The plan's `rd.emergency=reboot` is dracut's switch; this initrd is not dracut,
and systemd ignores the key with a value (and would boot into emergency mode with a bare
`rd.emergency`), so the drop-ins do the same job. The trial's health gate and deadline are
image files, as on x86 (`fjarr-spike-base`): only a trial pulls `boot-complete.target` in (the
applier's `fjarr-bless.service`), `fjarr-health.service` gates it with x86's checks (`/data`
mounted, a default route, sshd listening) and x86's test files in `/data/fjarr-test`
(`fail-health`, `panic`, `hold-health`), a failed gate reboots, and a trial that has not reached
the target after 15 minutes resets warm (`JobTimeoutAction=reboot-force`). Each of those resets
is the trial's second boot, which the applier's trial counter rolls back.

**SWUpdate** 2025.12 from a defconfig of the layer's own: CMS signatures (checked against
`/etc/fjarr/swu.crt`, the default e-mail-protection purpose, as the x86 key), sha256 per image,
raw images, zstd, `/etc/hwrevision` = `zedbox-mini-orin-nx16 1.0`; no bootloader backend
(`-B none` is the only one built), no Lua, no web server, no downloader, no scripts. The build
fails if the generated `.config` drifts from that. The daemon and its socket stay disabled: the
applier runs `swupdate` from the local cache. That a package writes the slot partitions and
never the ESP is the signed `sw-description`'s job (the raw handler can write files too).

**The applier** (`spikes/ota-jetson/applier/`): `fjarr-spike-applier` installs its script as
`/usr/lib/fjarr/applier` with `fjarr-trial.service` and `fjarr-boot-verify.service` (enabled) and
`fjarr-bless.service` (started by a trial only), the files its README lists by name, not its
tests. The image masks meta-tegra's `nv_update_verifier` only when `fjarr-boot-verify.service`
is in it: without a replacement, a committed slot would use up its retries in three warm
reboots.

**Also in the image:** sshd for root with the lab key only (host keys copied to `/data/ssh` on
first boot, as on x86, with the image's own copies as fallback), DHCP on any wired port with
the MAC as client identifier (systemd-networkd), `/boot/efi` as an automount that unmounts
after 30 s idle (meta-tegra mounts the ESP for the whole uptime; `setup-nv-boot-control_%.bbappend`
links an automount instead), `reboot-recovery` (meta-tegra's `tegra-flash-reboot`), nvbootctrl
and `oe4t-set-uefi-OSIndications`, `efibootmgr`, `efivar`, the disk tools, and `os-release`
with `IMAGE_ID=fjarr` and `IMAGE_VERSION`.

## The kernel's watchdog fixes

NVIDIA's 36.5.2 kernel (linux-jammy `l4t-r36.5.2`, `e6b364044e43`, the source meta-tegra
builds) carries neither upstream commit. Its `drivers/clocksource/timer-tegra186.c` has its own
version of both ideas instead: it arms WDT0 at probe (120 s) and pets it from the timer
interrupt until userspace's first ping, after which userspace owns WDT0 and nothing pets it.
That hand-over has a bug: `tegra186_wdt_ping()` clears a bit in a local `value` it never read
and writes it to `WDTCR` (GCC 13.3 with `-Wmaybe-uninitialized`: "'value' may be used
uninitialized", line 229; kernel builds turn that warning off). And once a watchdog is stopped
by a magic close, the kernel no longer covers it.

The upstream fix does not apply as is: 46fb093b87a0 (a kernel watchdog armed at probe and kept
alive from its interrupt) is the last of a series that first makes the driver handle several
watchdogs (ca57bf46e7a9, 341305ea9c89, 5eb5d8eaeacf), on top of b42d781e0350 (no self-pinging).
`recipes-kernel/linux/linux-jammy-nvidia-tegra/0001-timer-tegra186-backport-the-kernel-watchdog.patch`
backports the series' watchdog logic to NVIDIA's file and replaces NVIDIA's version: WDT0 is the
kernel's (120 s, petted from its interrupt, not exposed to userspace), WDT1 is userspace's
`/dev/watchdog0` without petting, WDT2 belongs to the audio engine. NVIDIA's 5.15-specific code
stays (the remove callback, noirq PM ops, its timeleft). It applies to the tree at
`e6b364044e43` and compiles cleanly with GCC 13.3 against NVIDIA's defconfig (2026-10-17);
nothing has run it yet. The patch is GPL-2.0-only, like the file it changes.

D2b compares it with NVIDIA's driver: `build.sh --unpatched-kernel` builds the second slot image
with `FJARR_WDT_BACKPORT = "0"` (kernel version suffix `-nvwdt`) in `out/yocto/build-nvwdt/`. Two
things for D2b to watch: upstream reserves the first watchdog the OS may access for the kernel,
so systemd gets WDT1, whose timer source the firmware sets; and with the backport a hard lockup
resets after 120 s even while userspace's watchdog is stopped.

## Not in this image yet

- **The lab dead-man** (L3: enter RCM when no operator contact for 20 minutes) is not in the
  layer; until it is, this image does not meet L3 and is not for the box. The image recipe says
  so in the build itself: without `fjarr-lab-deadman.service` in the root it warns (`bbwarn`) and
  writes `/etc/fjarr/NOT-FOR-THE-BOX` into the root, which the flash scripts (D0, D1) must refuse.
  The port is `rescue/rootfs/usr/sbin/fjarr-lab-deadman` plus a systemd unit.
- **The rescues** (A5 in `recovery`, which meta-tegra leaves empty; A8 on the network), the
  flash container, the `.swu` packaging (its `sw-description` with `fjarr,slot-a|b` and
  `hardware-compatibility` `1.0`) and the flash itself (D0, then D1) are other tasks.
- **Capsule signing** stays on meta-tegra's EDK2 test certificates until the ADR after phase E
  (Q11). `TEGRA_UEFI_LOWEST_SUPPORTED_VERSION` is meta-tegra's default, the L4T version.
- The recipes name the applier's and the board package's files; a renamed or added file there
  needs the matching line here.

## Files

| Path | What |
|---|---|
| `kas.yml` | layers and pins, machine, distro, targets, the build's host limits |
| `build.sh` | kas-container wrapper; the lab identity in `.lab/` |
| `meta-fjarr-spike/conf/machine/zedbox-mini-orin-nx16.conf` | the machine |
| `meta-fjarr-spike/conf/distro/fjarr-spike.conf` | poky, systemd, a headless feature set, SWUpdate 2025.12 |
| `meta-fjarr-spike/recipes-bsp/tegra-binaries/` | `/data` in the layout; the board's BCT files |
| `meta-fjarr-spike/recipes-bsp/uefi/` | the `RootfsRetryCountMax` overlay |
| `meta-fjarr-spike/recipes-bsp/tools/` | `/boot/efi` as an automount |
| `meta-fjarr-spike/recipes-bsp/devicetree/` | the board package's device tree (skipped with `FJARR_BOARD_DT = "0"`) |
| `meta-fjarr-spike/recipes-kernel/linux/` | the watchdog backport |
| `meta-fjarr-spike/recipes-support/swupdate/` | SWUpdate's defconfig |
| `meta-fjarr-spike/recipes-core/images/` | `fjarr-spike-image`, `fjarr-spike-initramfs` |
| `meta-fjarr-spike/recipes-core/fjarr-slot-root/` | `/dev/fjarr/root` |
| `meta-fjarr-spike/recipes-core/fjarr-spike-base/` | network, sshd, watchdog, emergency reboot, `/data`, the trial's health gate and deadline |
| `meta-fjarr-spike/recipes-core/fjarr-spike-initrd-config/` | the initrd's systemd settings |
| `meta-fjarr-spike/recipes-core/fjarr-spike-lab/` | the lab key, host keys and SWUpdate certificate |
| `meta-fjarr-spike/recipes-core/fjarr-spike-applier/` | the applier and its units from `../applier/` |
