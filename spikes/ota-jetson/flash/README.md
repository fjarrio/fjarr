# ota-jetson/flash — NVIDIA's flash tools for the ZED Box Mini

Spike code for [docs/32#jetson-test-image](../../../docs/32-fleet-ota.md#jetson-test-image)
(flashing from the mini-PC), from phase A, off the box: NVIDIA's L4T flash tools in an Ubuntu
22.04 container, a Linux_for_Tegra tree prepared the way Stereolabs' flash script prepares its own,
the flash images generated without a device, and the mini-PC's host setup for phase C. Nothing
here ships, and nothing here has touched the box, its plug or the mini-PC.

| File | What |
|---|---|
| `Dockerfile.l4t` | the `fjarr-l4t-flash` image: Ubuntu 22.04 with the packages of L4T's `tools/l4t_flash_prerequisites.sh` |
| `host-nfs-only` | installed in the image as `service` and `rpcbind`, so the container never starts NFS daemons |
| `prepare-l4t.sh` | a Linux_for_Tegra tree: NVIDIA's BSP, Stereolabs' device-tree tarball over it, the minimal rootfs |
| `minimal-rootfs.sh` | the least `rootfs/` NVIDIA's tools need (runs in the container) |
| `rootfs-debs.lock` | the 63 arm64 packages of that rootfs, pinned by SHA-256 |
| `lock-rootfs-debs.sh` | regenerates the lock from NVIDIA's own lists |
| `gen-images.sh` | `l4t_initrd_flash.sh --no-flash` offline, and a record of what it made |
| `host-setup.sh` | the mini-PC's changes for phase C, with `--dry-run` and `--undo` |
| `test-host-setup.sh` | runs `host-setup.sh` for real in a throwaway Ubuntu 26.04 container |

Run on the host (the workstation, later the mini-PC), never inside `dev`. Downloads, the tree and
the results go to `spikes/ota-jetson/out/flash/` (gitignored):

```bash
spikes/ota-jetson/flash/prepare-l4t.sh 36.4.4          # out/flash/l4t-36.4.4/Linux_for_Tegra
spikes/ota-jetson/flash/gen-images.sh stock            # Stereolabs' layout   -> out/flash/results/stock/
spikes/ota-jetson/flash/gen-images.sh ab               # NVIDIA's root A/B     -> out/flash/results/ab/
spikes/ota-jetson/flash/gen-images.sh initrd           # the backup tool's boot package
spikes/ota-jetson/flash/host-setup.sh --dry-run --l4t /abs/path/Linux_for_Tegra
spikes/ota-jetson/flash/test-host-setup.sh             # PASS/FAIL lines
```

The rootfs inside the tree is root's; remove a tree with
`docker run --rm -v "$PWD/spikes/ota-jetson/out/flash:/o" fjarr-l4t-flash rm -rf /o/l4t-36.4.4`.

## The container

`Dockerfile.l4t` installs the list of `tools/l4t_flash_prerequisites.sh` the way that script does
(recommends included), on `ubuntu:22.04` pinned by digest. The two releases' lists compared:
36.5.2's adds `file`, `gdisk`, `parted`, `usbutils`, `xxd`, `zlib1g` and `python-is-python3` to
36.4.4's and drops nothing, so the image takes 36.5.2's and serves both. Added for the container:
`openssh-client` (the flash reaches the box over ssh), `sudo` (NVIDIA's scripts call it as root),
`kmod` (`depmod` for the minimal rootfs), and `USER=root`, which `flash.sh` tests and a container
does not set. The image is 378 MB (2026-10-17).

The flash in the container runs with the host's network, the docker mode NVIDIA's script supports: with
`/run/nvidia_initrd_flash/docker_host_network` present, `l4t_initrd_flash.sh` expects the host's
NFS server to export the tree at the same paths (`is_inside_docker` in
`tools/kernel_flash/l4t_network_flash.func`). It still runs `rpcbind` and
`service nfs-kernel-server restart` first (`network_prerequisite`), and a second `rpc.mountd` in
the host's network would answer the box from the container's empty exports. So the image replaces
both: `rpcbind` does nothing, and the restart succeeds only when the host's server answers
`showmount -e localhost`. NVIDIA's 36.4.4 flashing guide describes no docker use; that check in
the script is all there is.

## The tree: what Stereolabs' script does, and what is left out

`prepare-l4t.sh 36.4.4` follows `zedbox_mini_usb_flash_6.2_gpio.sh` (public, from
download.stereolabs.com): NVIDIA's `Jetson_Linux_R36.4.4_aarch64.tbz2`, then Stereolabs'
`zedbox_mini_device_trees_364.tar` unpacked in the directory holding `Linux_for_Tegra`. The tarball
replaces the three bootloader files, the ten kernel DTBs and `jetson-orin-nano-devkit-super.conf`
(a symlink in NVIDIA's tree), adds `kernel/dtb/devicetree.dts`, and carries
`p3509-a02-p3767-0000.conf` byte-identical to NVIDIA's. Downloads are checked against SHA-256s in
the script: the BSP `a6ce11c2…`, the tarball `d1799349…`; the script itself, which `prepare-l4t.sh`
follows rather than runs, is `b5896469…` (2026-10-17).

Left out of Stereolabs' flow: the sample rootfs and `apply_binaries.sh` (below), the default user
and hostname it writes into the rootfs, `l4t_flash_prerequisites.sh` on the host (the container
holds those packages), and the flash itself.

### The least rootfs

NVIDIA's tools read `Linux_for_Tegra/rootfs` in three places when they generate images:

- `flash.sh` packs it into `APP`'s image and needs `boot/extlinux/extlinux.conf`;
- the flash initrd copies eight tools, four libraries and ten module paths out of it
  (`generate_flash_initrd` in `tools/kernel_flash/l4t_initrd_flash_internal.sh`);
- the recovery image for the `recovery` partition copies 114 files out of it, sshd and its
  configuration among them, listed in `tools/ota_tools/version_upgrade/recovery_copy_binlist.txt`.
  A missing file there is only a warning, so a thin rootfs builds a broken recovery image without
  failing.

`minimal-rootfs.sh` gives exactly those, in place of the 1.83 GB sample rootfs
(`Tegra_Linux_Sample-Root-Filesystem_R36.4.4_aarch64.tbz2`) and `apply_binaries.sh`: merged `/usr`,
63 arm64 packages from Ubuntu 22.04's frozen release pocket (18 MB of `.deb`s, unpacked without
their maintainer scripts), the BSP's `kernel_supplements.tbz2` and `kernel_oot_modules.tbz2`,
`depmod` as the kernel package's postinst runs it, and `etc/ssh/sshd_config` copied as
openssh-server's postinst makes it. `lock-rootfs-debs.sh` derives the package list from NVIDIA's
two lists with `apt-file` over the signed jammy index; the library versions match those NVIDIA's
sample rootfs lists in `nv_tools/scripts/desktop_samplefs_contents.txt`. Before marking the tree
ready, `minimal-rootfs.sh` checks every path both lists name, read from NVIDIA's files. The result:
3,883 entries, 125 MB. Generating images then adds about 57 MB: `flash.sh` writes the kernel
`Image`, the `initrd` and the DTB into `boot/`, and `nv_boot_control.conf` and
`user_release_version` into `etc/`, as it does in the sample rootfs.

Enough for: every image `--no-flash` generates, and the flash initrd NVIDIA's backup and restore
tool boots (`l4t_backup_restore.sh` runs `l4t_initrd_flash.sh --initrd`, which boots the box into
it without mounting `rootfs/`). Not enough for a network flash (`--network usb0` without
`--no-flash`): there the box mounts `rootfs/` over NFS, chroots into it and runs the flash script
with its userland. No phase flashes NVIDIA's images (phase C restores the box's own backup, and
Fjarr's images come from meta-tegra and jetpack-nixos); a factory reflash through Stereolabs'
script would need the sample rootfs.

What `apply_binaries.sh` would have added and the images lack: NVIDIA's userspace in `APP` (never
flashed by Fjarr), and `nv-update-initrd`, which runs in a qemu chroot of the full rootfs and adds
modprobe configuration, NVMe, PCIe, USB-gadget, Ethernet and PPS modules to `l4t_initrd.img`, the
initrd of `boot.img` and `recovery.img`.

## Generated without a device (2026-10-17)

`gen-images.sh` runs NVIDIA's offline mode (`BOARDID=3767 BOARDSKU=0000 FAB=300`, the Orin NX 16 GB
row of `README_initrd_flash.txt`) with Stereolabs' arguments plus `--no-flash`:

```bash
l4t_initrd_flash.sh --no-flash --external-device nvme0n1p1 -c tools/kernel_flash/flash_l4t_external.xml \
  -p "-c bootloader/generic/cfg/flash_t234_qspi.xml" --showlogs --network usb0 p3509-a02-p3767-0000 internal
```

The container gets no network and no host device but the loop devices, and `CAP_SYS_ADMIN` without
AppArmor for the loop mounts `flash.sh` makes; `/sys` stays read-only, so NVIDIA's write to the host's
USB autosuspend setting fails harmlessly. Checked on the workstation after each run: the autosuspend
value and the loop devices unchanged. A run took 241 s (`stock`), 218 s (`ab`) and 95 s (`initrd`);
898, 962 and 282 s in a second round, from a freshly prepared tree, while other builds kept the
workstation at a load of 20–35.

**The NVMe** (`images/external/flash.idx`; partition numbers from the index, the same as on the
factory box):

| p | Partition | Size | Image | Image bytes |
|---|---|---|---|---|
| 1 | `APP` | to the disk's end (the index assumes a 57 GiB disk, so 55.5 GiB; 237 GiB on the box) | `system.img`, a zstd tar of the root filesystem | 70,253,599 and 70,486,180 in two runs (the minimal rootfs; file times differ) |
| 2 | `A_kernel` | 128 MiB | `boot.img` (kernel, initrd, command line) | 58,003,456 |
| 3 | `A_kernel-dtb` | 768 KiB | `kernel_tegra234-p3768-0000+p3767-0000-nv.dtb` | 249,871 |
| 4 | `A_reserved_on_user` | 31.6 MiB | — | |
| 5 | `B_kernel` | 128 MiB | `boot.img`, the same file | 58,003,456 |
| 6 | `B_kernel-dtb` | 768 KiB | the same DTB | 249,871 |
| 7 | `B_reserved_on_user` | 31.6 MiB | — | |
| 8 | `recovery` | 80 MiB | `recovery.img` | 63,739,904 |
| 9 | `recovery-dtb` | 512 KiB | `tegra234-p3768-0000+p3767-0000-nv.dtb.rec` | 249,871 |
| 10 | `esp` | 64 MiB | `esp.img`, FAT holding only `EFI/BOOT/BOOTAA64.efi` | 67,108,864 |
| 11–13 | `recovery_alt`, `recovery-dtb_alt`, `esp_alt` | 80 MiB, 512 KiB, 64 MiB | — | |
| 14 | `UDA` | 400 MiB | — | |
| 15 | `reserved` | 479.5 MiB | — | |

plus the protective MBR, the primary GPT (19,968 B) and the secondary GPT.

**The QSPI** (`images/internal/flash.idx`, 64 MiB): 61 entries. The BR BCT (8 KiB in 1 MiB), then
25 partitions per chain, A and B holding the same files: the largest are `cpu-bootloader` (UEFI,
3,301,072 B in 3.5 MiB), `secure-os` (1,887,312 B in 4 MiB), `bpmp-fw` (1,027,008 B), `dce-fw`
(781,712 B), `mb2` (440,944 B) and `bpmp-fw-dtb` (383,296 B); `smm-fw` and `reserved_on_boot`
stay empty. Then `uefi_variables` and `uefi_ftw` (no image: a full flash erases the whole QSPI
first, `erase_spi` in `l4t_flash_from_kernel.sh`, so the variables start from their defaults),
`BCT-boot-chain_backup`, the GPT backups, and `A_VER`/`B_VER` (`qspi_bootblob_ver.txt`, 109 B).

**The flash initrd and RCM boot**: `bootloader/boot0.img` (77,303,808 B with the QSPI images inside;
68,132,864 B for `--initrd`, which carries none) and `rcmboot_blob/` (`blob.bin` 80,855,423 B, with
`tegrarcm_v2` and the BCTs), booted with `uefi_jetson_minimal_with_dtb` on 36.4.4.

**Against the factory box** (its partition backup, read-only; nothing raw leaves `.lab/`):

| Same, byte for byte | Different |
|---|---|
| `A_kernel-dtb`, `B_kernel-dtb`, `recovery-dtb`: Stereolabs' DTB | the root `PARTUUID` on the kernel command line (new at every flash) |
| the ESP's only file, NVIDIA's 36.4.4 `BOOTAA64.efi` | `boot.img`'s ramdisk: the factory one is NVIDIA's initrd plus `nv-update-initrd`'s additions (15,855,789 against 14,909,192 B) |
| the kernel in `boot.img` and `recovery.img`, and their command lines otherwise | `recovery.img`'s ramdisk: the same additions, newer jammy-updates binaries from the sample rootfs, and SSH host keys made at every flash |

Of the NVMe images compared, only the ramdisks differ in content. `APP` (the minimal rootfs here,
Ubuntu on the box) and the GPT were not compared.

**NVIDIA's root A/B layout** (`gen-images.sh ab`: `ROOTFS_AB=1`, `-S 16GiB`,
`flash_l4t_t234_nvme_rootfs_ab.xml`) matches phase A1's reading of the layout, which D2 still
confirms on the box: `APP` p1 and `APP_b`
p2 at 17,179,869,184 B each, fixed (neither expands), `A_kernel` p3, `A_kernel-dtb` p4, `B_kernel`
p6, `recovery` p9, `recovery-dtb` p10, `esp` p11, `esp_alt` p14, `UDA` p15, `reserved` p16; chain B
gets its own `boot.img_b` (root `PARTUUID` of `APP_b`) and `APP_b` its own `system.img_b`. The QSPI
changes too, in UEFI alone (16 bytes larger): with `ROOTFS_AB=1`, `flash.sh` adds
`RootfsRedundancyLevel = 1`, a locked runtime variable, to the `L4TConfiguration` overlay of UEFI's
device tree (`process_l4t_conf_dtbo`, which also sets `fmp-lowest-supported-version` in either
mode, and `RootfsRetryCountMax` only when `ROOTFS_RETRY_COUNT_MAX` is set; the tree inside the UEFI
image is compressed, so this is read from the script, not from the bytes). Root A/B is therefore a
QSPI setting: the factory QSPI, built without it, stays boot-chain-only, and the first Fjarr flash
(D1) has to write the QSPI.

**Two generations of the same tree** (`stock`, run twice, image hashes from the index): the QSPI's
48 bootloader component images are byte-identical; only `A_VER`/`B_VER` differ, since
`qspi_bootblob_ver.txt` carries the build time (and the board identity of the run, here the offline
`FAB=300`), and the QSPI's GPT copies, which get new GUIDs. On the NVMe the DTBs and the MBR are
identical; `boot.img` (a new root `PARTUUID` on its command line), `recovery.img` (new SSH host
keys), `esp.img`, the GPTs and `system.img` (file times) are not. So a "byte-identical firmware"
check on a QSPI build (the QSPI rule, L4 in [docs/32's lab safety rules](../../../docs/32-fleet-ota.md#jetson-test-image)) has to compare per component and expect the version entry to differ.

## host-setup.sh: the mini-PC for phase C

`host-setup.sh --l4t <tree>` makes these changes and records each in
`/var/lib/fjarr-flash-host/state`; `--undo` reverts exactly those, in reverse order:

| Change | Why |
|---|---|
| `nfs-kernel-server` (with `rpcbind`, `nfs-common` and their libraries) | the box's flash initrd mounts the tree over NFS |
| `/etc/exports.d/fjarr-l4t-flash.exports`: each tree's `rootfs`, `tools/kernel_flash/images`, `tools/backup_restore`, to `fc00:1:1::/48` only, with NVIDIA's options (`rw,nohide,insecure,no_subtree_check,async,no_root_squash`) | the USB link's network; the paths the container sees |
| NVIDIA's `10-l4t-usb-msd.rules` and `99-l4t-host.rules` in `/etc/udev/rules.d`, checked by SHA-256 (the same in 36.4.4 and 36.5.2) | udisks, and so GNOME on the mini-PC, ignore the box's USB disks; NetworkManager leaves the flash link alone |
| `/etc/tmpfiles.d/fjarr-l4t-flash.conf`, making `/run/nvidia_initrd_flash/docker_host_network` at every boot | NVIDIA's docker mode; the container mounts it |
| `nfs-server` started, if it was not running | |
| with ufw active only: TCP 2049 from `fc00:1:1::/48` | |

It refuses before any change from 23:55 to 06:30 Europe/Stockholm (the nightly window, the 30 min
`fjarr-lab` gives a job still running at its end, and a run takes about a minute), while another script holds the lab lock `/run/lock/fjarr-zedbox.lock` (L1;
opened read-only as every lab script opens it, since `fs.protected_regular` refuses `O_CREAT` on
another user's file in `/run/lock`, even for root), with IPv6 off, when a tree path is not
canonical, and when a file it would write exists and is not its own. A package or rule that was
there before is kept by `--undo`; it purges only what it installed, and refuses if apt would remove
anything else. The package list from before an install stays on disk until the new packages are
recorded, so an interrupted install is still undone in full after a rerun. Docker it only checks
for.

Tested (2026-10-17, and again after the last change):

- the dry run on the workstation, as a user: exit 0, and `/etc/udev/rules.d`, `/etc/tmpfiles.d`,
  `/etc/exports.d`, `/etc/exports`, the NFS packages, `/run/nvidia_initrd_flash` and the state
  directory unchanged; `--dry-run --undo` reports nothing to undo; the refusals (not root, relative
  path, not a tree, no arguments) exit non-zero. `ufw status` needs root, so where ufw is installed
  the dry run shows the firewall rule as conditional on ufw being active;
- `test-host-setup.sh`: 41 of 41 checks pass in an Ubuntu 26.04 container (the mini-PC's release,
  uutils coreutils included) with stand-ins for systemd, udevd, a running NFS server and ufw
  (packages and `exportfs` real). The refusals (03:00, 23:57, a held lock, a foreign rule at
  06:00) change nothing; setup installs, loads three exports and records them; a second run
  changes no file; a second tree adds three exports; with ufw active the rule is added and later
  deleted; `--undo` returns the container to its first state except what apt itself leaves after a
  purge (the `statd` and `_rpc` users in `/etc/passwd` and `/etc/shadow`, and `/run/adduser`); an
  install interrupted before its record is undone in full after a rerun; and `--undo` keeps a
  package and a rule that were there before. The window's end moved from 06:00 to 06:30 after
  that run (one check more: a refusal at 06:15; the foreign rule now at 06:30); the test has not
  run since (it installs packages from the network).

It has not run on the mini-PC (phase C, with Erik). Notes for that run:

- NVIDIA's `l4t_initrd_flash.sh`, run natively on the host instead of in the container, links the
  same two rule names into `/etc/udev/rules.d` and deletes them on exit; rerun `host-setup.sh` after
  such a run.
- `rpcbind` and `nfsd` listen on every interface; only the exports are limited to `fc00:1:1::/48`.
- NVIDIA's backup tool runs `exportfs` itself (`enable_nfs_for_folder`); in the container that
  writes only the container's own table and exits 0 (checked), so the host's export serves.
- The flash itself needs `--privileged --net=host -v /dev:/dev` and the tree at the same path, so
  in that container NVIDIA's script does write the host's USB autosuspend setting (and restores it
  on exit), configures the USB link's address, and briefly links its udev rules in the
  container's own `/etc`. The command for phase C's backup, untested:

  ```bash
  L4T=/abs/path/Linux_for_Tegra
  docker run --rm -it --privileged --net=host -v /dev:/dev \
    -v /run/nvidia_initrd_flash:/run/nvidia_initrd_flash:ro -v "$L4T:$L4T" -w "$L4T" fjarr-l4t-flash \
    ./tools/backup_restore/l4t_backup_restore.sh -e nvme0n1 -b p3509-a02-p3767-0000
  ```

## Found on the way

- NVIDIA's recovery image (`ota_make_recovery_img_dtb.sh`) bakes SSH host keys made at build time
  into the `recovery` partition, and enables root login with a password whose hash is fixed in the
  public script. A box that boots its stock recovery (both slots unbootable, or phase B's test of
  it) offers root SSH with a published credential on its network.
- Root A/B (`RootfsRedundancyLevel`) and the version floor (`fmp-lowest-supported-version`) are set
  in the QSPI's UEFI device tree at flash time (above).
- The QSPI fix of NVIDIA's field bulletin (`trimmer2-val`, phase A1) is not in this 36.4.4 build
  either: `prepare-l4t.sh` does not apply `overlay_mb1bct_36.x.tbz2`, as Stereolabs' script does
  not. A build meant for the box's QSPI should.
