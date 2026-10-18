# rescue: the ZED Box Mini's network rescue

Spike code for the second of the three rescue layers in
[docs/32](../../../docs/32-fleet-ota.md#jetson-test-image) ("Three rescue layers", item 2): when
the box's firmware runs but nothing on the NVMe boots, UEFI tries its network boot entries, and the
lab mini-PC answers with a small Linux that runs from RAM. It has sshd with the lab key, DHCP, the
disk tools, `reboot-recovery` and the 20-minute lab dead-man, so it ends in RCM, where the mini-PC
can reflash the box. Phase A8 builds and tests it off the box; T5 in phase B6 proves it on the box
and waits for Erik. Nothing here ships.

```bash
# On the workstation (Docker, with qemu-user's binfmt for arm64), never inside `dev`:
spikes/ota-jetson/rescue/build.sh [--l4t 36.4.4|36.5.2] [--test]   # out/rescue/<l4t>/fjarr-net-rescue.efi
spikes/ota-jetson/rescue/test/netboot-test.sh [36.4.4]             # the off-box bench, after --test
spikes/ota-jetson/rescue/test/minipc-test.sh [36.4.4]              # net-rescue.sh's install and --undo
# On the mini-PC, as root, with net-rescue-http.py beside the script:
net-rescue.sh --mac <box MAC> [--efi FILE] [--iface IF]            # install or update, and serve
net-rescue.sh --status                                             # exit 0 while it is serving
net-rescue.sh --undo                                               # remove everything it added
```

The box's MAC stays out of the repo, in `.lab/rescue/box-mac`.

| File | What |
|---|---|
| `build.sh` | the build: NVIDIA's kernel and modules from the BSP tarball, the userland from Ubuntu 22.04 arm64, an initramfs, one UKI |
| `Dockerfile.userland`, `collect-userland.sh` | Ubuntu 22.04 arm64, run through qemu-user: copies BusyBox, the tools and every library they load into the tree, compiles `reboot-recovery` |
| `Dockerfile.tools` | Ubuntu 26.04 amd64: ukify and systemd's arm64 stub, depmod, and the bench (QEMU, AAVMF, dnsmasq, shellcheck) |
| `mkcpio.py` | writes the tree as a cpio without root: root-owned, fixed timestamps, `/dev/console` |
| `reboot-recovery.c` | `reboot(LINUX_REBOOT_CMD_RESTART2, "forced-recovery")`, what `reboot forced-recovery` does |
| `rootfs/` | the rescue's own files: `/init`, `/etc/inittab`, the dead-man, DHCP, sshd's configuration |
| `net-rescue.sh`, `net-rescue-http.py` | the mini-PC side: dnsmasq as proxy DHCP and TFTP, and a one-file HTTP server, for one MAC |
| `test/` | the bench: `netboot-test.sh` starts `bench.sh` in a container; `dhcp-probe.py` asks like a UEFI client. `minipc-test.sh` runs `net-rescue.sh` as root in a stand-in for the mini-PC (`Dockerfile.minipc`: Ubuntu 26.04 with systemd and ufw) |

`build.sh` creates `.lab/rescue/ssh_host_ed25519_key` once (gitignored) and writes
`out/rescue/<l4t>/known_hosts` for it. Scripts tell the rescue from the box's own system at the
same address by its host key:
`ssh -o UserKnownHostsFile=out/rescue/36.4.4/known_hosts -o HostKeyAlias=fjarr-net-rescue root@<box>`.

## One EFI file: a unified kernel image

UEFI's PXE and HTTP boot fetch one file and start it, so the kernel must arrive with its initramfs
and command line. Two ways were open:

- **An initramfs compiled into the kernel.** NVIDIA's 36.4.4 kernel has none
  (`CONFIG_INITRAMFS_SOURCE=""`) and no built-in command line (`CONFIG_CMDLINE=""`), so this means
  rebuilding NVIDIA's kernel from source. The rebuilt kernel is no longer the one NVIDIA's modules,
  the out-of-tree `r8168` among them, were built against (`modversions`, signed with the build's
  own key), and every BSP update means another kernel build.
- **A unified kernel image (UKI)**, chosen: systemd's EFI stub (`linuxaa64.efi.stub`) with
  NVIDIA's `Image` byte for byte in `.linux` (sha256 `a1ffea85…`, the same file as the stock
  box's `/boot/Image`), the initramfs in `.initrd` and the command line in `.cmdline`. The stub
  hands the initramfs to the kernel through the `LINUX_EFI_INITRD_MEDIA_GUID` LoadFile2 protocol,
  which the kernel's EFI stub reads since Linux 5.8. Nothing else works for this kernel on a
  network boot: its stub has no `initrd=` loader (`CONFIG_EFI_GENERIC_STUB_INITRD_CMDLINE_LOADER`
  is off), and a network boot has no file system to load from anyway. Ubuntu 22.04's stub
  (systemd 249) has no LoadFile2 support, so the stub and `ukify` come from Ubuntu 26.04
  (systemd 259.5). A rebuild for 36.5.2 swaps the kernel and modules, nothing else.

**The command line** is NVIDIA's own for the board (`CMDLINE_ADD` in the BSP's
`p3767.conf.common`, what L4TLauncher passes) plus `panic=10` and `efi=runtime` (36.5.2's BSP
adds the latter itself). systemd's stub replaces `.cmdline` with the boot entry's load options when
Secure Boot is off, but the box's network entries are the firmware's auto-created ones (their
optional data is edk2's auto-create GUID), and edk2's boot manager, also at NVIDIA's 36.4.4 pin,
passes no load options for those (`BmIsAutoCreateBootOption` in `BmBoot.c`). The bench sees
`/proc/cmdline` equal to `.cmdline` through AAVMF, the same boot manager. `/init` sets
`kernel.panic` itself in case.

**The device tree** is UEFI's own. In device-tree mode NVIDIA's UEFI installs the tree it was
flashed with as the FDT configuration table, applies the firmware's overlays at the end of DXE and
its fixups (floorsweeping, board properties, ramoops, OP-TEE and fTPM nodes; the QSPI nodes are
removed): edk2-nvidia r36.4.4, `DxeDtPlatformDtbKernelLoaderLib.c`. L4TLauncher replaces that
table with the slot's kernel tree; a kernel started straight from the network keeps UEFI's. The
UKI carries no `.dtb`: a tree from the slot would miss the firmware's fixups. Whether UEFI's tree
brings up PCIe, the NVMe, the Ethernet port and RCM on the box is for T5 to see.

**Plain HTTP is allowed**: edk2 refuses `http://` boot URIs unless the platform sets
`PcdAllowHttpConnections`, and edk2-nvidia r36.4.4 sets it (`NVIDIA.common.dsc.inc`); its firmware
also carries `HttpBootDxe`, `UefiPxeBcDxe` and Realtek's PCIe UNDI driver.

## What runs in the rescue

- **The kernel** `5.15.148-tegra` from the BSP, with the modules NVIDIA's own recovery image loads
  (the stock `recovery` partition's `nv_recovery.sh` and `init`): `phy-tegra194-p2u`,
  `pcie-tegra194`, `nvme` with `nvme-core` for the disk, `tegra-bpmp-thermal`, `pwm-tegra`,
  `pwm-fan` for the fan, and NVIDIA's out-of-tree `r8168` for the Ethernet port. `depmod` runs over
  just these.
- **`/init`** (BusyBox sh) mounts `/proc`, `/sys`, `/dev`, `/run`, `/tmp` and `efivarfs`, opens the
  hardware watchdog (BusyBox `watchdog`, 60 s; L3), loads the modules and hands PID 1 to BusyBox
  `init`, which respawns the rest from `/etc/inittab`: `syslogd`, the dead-man, DHCP (`udhcpc` on
  every wired port as it appears), `sshd` and a root shell on the console (a display and keyboard
  on the box). The scripts are BusyBox sh, not bash: the rescue carries no bash, and PID 1 must not
  stop at a failed step, so there is no `set -e`.
- **The dead-man** (`fjarr-lab-deadman`): when `/run/fjarr-lab-ok` has gone 20 minutes without a
  touch, counted from boot and restarted by every touch, it logs to the kernel log and runs
  `reboot-recovery`. It measures uptime, not the wall clock, and keeps its count in `/run` across a
  respawn. If RCM is refused it resets after 30 s, back to the firmware's boot order.
- **sshd** (OpenSSH 8.9p1): root with the keys in `~/.ssh/id_*.pub` of the workstation that built
  it (or `FJARR_LAB_PUBKEYS`), no passwords, the host key from `.lab/rescue/`, sftp.
- **Tools**: BusyBox 1.30.1 (sh, ip, udhcpc, mount, dd, tar, wget, vi, …), `sgdisk`,
  `mke2fs`/`e2fsck`/`resize2fs`/`tune2fs`/`chattr`, `nvme`, `efibootmgr`, `mkfs.fat`/`fsck.fat`,
  `zstd`, `lsblk`/`blkid`/`wipefs`/`blkdiscard`/`findmnt`/`sfdisk`/`fdisk`, and `reboot-recovery`.
  BusyBox is Ubuntu's ordinary build: `busybox-static`'s shell runs its own applets ahead of
  `$PATH`, so `mke2fs`, `fdisk` and `blkdiscard` were BusyBox's until the bench caught it; the
  build now checks that the rescue's shell finds the real ones. There is no udev:
  `blkid -t PARTLABEL=APP -o device` finds partitions, and `efibootmgr` and
  `/sys/firmware/efi/efivars` reach the boot variables. No `nvbootctrl` (NVIDIA's, closed).

Size (36.4.4): the UKI is 50.3 MB, of which NVIDIA's uncompressed `Image` is 43.1 MB; the
initramfs is 23.3 MB, 7.1 MB with xz. sshd's libraries are its largest part (OpenSSL 4 MB, and
Ubuntu's sshd links systemd and Kerberos); a smaller sshd would save about 2 MB of the 50.

## Serving it from the mini-PC: `net-rescue.sh`

- **dnsmasq in proxy mode** (`dhcp-range=<LAN>,proxy`): it hands out no address, the LAN router
  keeps doing that, and it answers only the box's MAC (`dhcp-mac` and `dhcp-ignore=tag:!…`). DNS is
  off (`port=0`).
- **PXEv4** (client arch 11): one `pxe-service`, so dnsmasq's UEFI path sends the firmware to port
  4011, where it gets the file name, and TFTP serves it. TFTP serves files only from
  `tftp/<the client's MAC>/` (`tftp-unique-root=mac`), so another machine finds nothing. dnsmasq
  maps the client's address to its MAC through an ARP view it refreshes every 90 s.
- **HTTPv4** (client arch 19): dnsmasq's proxy answers only PXE clients, unless
  `dhcp-pxe-vendor` names `HTTPClient` too; then the proxy offer carries vendor class
  `HTTPClient` and the URL, `http://<mini-PC>:8471/fjarr-net-rescue.efi`, which edk2's HTTP boot
  takes with the router's offer (its "DHCP offer plus proxy offer with an IP URI" case). The box's
  boot order tries HTTPv4 before PXEv4. `net-rescue-http.py` serves that one file to that one MAC,
  looked up in the kernel's neighbour table on every request (others get 403), with
  `Content-Type: application/efi`.
- **What it changes on the mini-PC**: `dnsmasq-base` if it is missing, `/var/lib/fjarr-net-rescue`,
  two enabled units (`fjarr-net-rescue-dhcp`, `fjarr-net-rescue-http`, so it survives a reboot of
  the mini-PC), and four ufw rules if ufw is active. `--undo` removes exactly these, and refuses
  while a lab script holds the box lock (`/run/lock/fjarr-zedbox.lock`, or `FJARR_ZEDBOX_LOCK`),
  since a write may rely on the rescue. Installing refuses 00:00-06:30 Europe/Stockholm (Q6, and
  the 30 min `fjarr-lab` gives a job still running at its end, as L1 in `lib/lab.sh`).
- **What it leaves alone**: the router's DHCP (no addresses), the NUC's nightly reflash (the NUC
  boots its own on-disk rescue through `BootNext`; were its firmware ever to fall through to
  network boot, the proxy ignores its MAC), fjarr-lab's own HTTP server (`127.0.0.1:8090`), the
  desktop session and the agent. dnsmasq binds to the one interface (`bind-dynamic`, which also
  rides out a Wi-Fi reconnect), so another dnsmasq on the host keeps its own.
- **The mini-PC is on Wi-Fi** (its Ethernet port has no cable): the box's broadcasts must cross the
  router's bridge from the wired port to Wi-Fi. B2 checks that; `--iface` takes the Ethernet port
  if it gets a cable.

## Tested off the box (2026-10-17)

- `dnsmasq --test` (2.92, as on the mini-PC's Ubuntu 26.04) accepts the configuration
  `net-rescue.sh --print-config` writes for the mini-PC (`wlp3s0`, a /24).
- shellcheck 0.11 is clean on every script: bash on the workstation and the mini-PC, BusyBox's
  dialect for the rescue's own.
- `build.sh` checks in a chroot of the tree (qemu-user) that every binary finds its libraries, that
  every program `/etc/inittab` starts exists, that sshd accepts its configuration and host key
  (`sshd -t`), and that the shell finds e2fsprogs' `mke2fs` and util-linux's `fdisk` and
  `blkdiscard`.
- `test/minipc-test.sh` passed (2026-10-17): install on a stand-in without dnsmasq (dnsmasq-base
  installed, four ufw rules, two enabled units, `--status` passes), `--undo` refused while the box
  lock is held with the rescue still serving, then `--undo` leaves no state, unit, ufw rule or
  package behind and `--status` fails. The window's end moved from 06:00 to 06:30 after that run,
  and the test gained two refused installs (03:00, 06:15); it has not run since (it installs
  packages from the network).
- **The bench** (`test/netboot-test.sh`), run 8 of 2026-10-17 (the host at load 30-40 from a Yocto
  build): the probes (proxy offers for PXE and HTTP clients with the box's MAC only, TFTP and HTTP
  to that MAC only); a PXEv4 boot of the test UKI to root over ssh in 246 s, with the UKI's command
  line, the modules, the tools and the firmware variables; the dead-man entering RCM 202 s after the
  last touch (limit 180 s, polls every 10 s); an HTTPv4 boot in 275 s; `reboot-recovery` reaching
  the kernel as `RESTART2 forced-recovery`. Its last step, the lab UKI by HTTP alone, failed: AAVMF
  reset the download at 48% ("Unexpected network error") under that load, and PXE was still
  blocked from step 3. Step 4 now leaves both ways open; run 9 with that change is pending.

**What the bench cannot show**: NVIDIA's firmware and its Realtek UNDI driver, UEFI's device tree
on the box, RCM itself (QEMU only shows the restart command reaching the kernel), the Wi-Fi hop
between the box and the mini-PC, and real timings. T5 shows those.

**Running the bench** (for whoever runs it next): AAVMF 2025.11's network stack needs a random
number generator (Ubuntu LP #2069439), so the VM gets `virtio-rng-pci`; NVIDIA's kernel oopses
bringing up the second CPU on QEMU's default GICv2, so the machine is `gic-version=3`, as on the
Orin; `acpi=off` makes AAVMF hand the kernel a device tree, as NVIDIA's UEFI does; `-cpu max`
spends minutes in the firmware under emulation, `neoverse-n1` does not; and a `bootindex` on the NIC
makes AAVMF drop the network entries. The test UKI adds `earlycon`, since the kernel prints nothing
on QEMU's UART before its console driver loads.

## T5 on the box (phase B6)

`../b6.sh net-up` and `../b6.sh t5` run these steps under the box lock, with their preconditions,
checks and results ([B6 in the spike's README](../README.md#b6-one-shot-boot-tests)); `../b6.sh
net-down` runs `--undo` at the end of B6. By hand, under the box lock (L1), never 00:00-06:30, with
the stock box running:

1. On the workstation: `build.sh`, then copy `net-rescue.sh`, `net-rescue-http.py` (beside it) and
   `out/rescue/36.4.4/fjarr-net-rescue.efi` to the mini-PC, and there, as root,
   `./net-rescue.sh --mac <box MAC> --efi fjarr-net-rescue.efi`; `--status` must pass.
2. On the box: `sudo efibootmgr -n 0004` (UEFI HTTPv4), reboot. Find the box by its MAC and log
   in with the rescue's host key (above); `touch /run/fjarr-lab-ok`. Record `/proc/cmdline`,
   `dmesg` (PCIe, NVMe, `r8168`), `lsblk`, `efibootmgr -v` (BootCurrent 0004, no BootNext), and
   the download time from the mini-PC's journal (`journalctl -u fjarr-net-rescue-http`).
3. `reboot-recovery`: the mini-PC sees `0955:7323`; a plug power cycle brings the stock system
   back (`0955:7020`, BootCurrent 0001).
4. The same with `efibootmgr -n 0002` (UEFI PXEv4, TFTP over the router's bridge to Wi-Fi).
5. Once, leave the rescue untouched: RCM after 20 minutes.

Done when HTTPv4 and PXEv4 both bring the rescue up and it reaches RCM, or one does and is
recorded as the backstop. With nothing on the NVMe bootable, the stock box's boot order falls
through to HTTPv4 first, then PXEv6, then PXEv4.

## Open

- **T5** above; until it passes, every write in C, D and F needs Q4.
- **36.5.2**: rebuild with `build.sh --l4t 36.5.2` after D1 (it extracts the 36.5.2 kernel and
  modules from the BSP tarball) and serve that.
- **The lab lock**: `--undo` checks `/run/lock/fjarr-zedbox.lock`, the lock `../lib/lab.sh` takes
  (`LAB_LOCK_FILE`, L1); `FJARR_ZEDBOX_LOCK` follows it if that ever moves.
- **The recovery-partition rescue** (A5) could carry this initramfs in an Android boot image
  (23.3 MB unpacked, 7.1 MB with xz, inside its 37 MB budget); it is the Yocto recipe's to build.

## Licences

Lab tools only: nothing here is in anything Fjarr ships.

| Component | Version | Licence | Where |
|---|---|---|---|
| NVIDIA L4T kernel and modules (incl. `r8168`) | 5.15.148-tegra (L4T 36.4.4) | GPL-2.0-only | the UKI |
| systemd-stub (`systemd-boot-efi`, arm64) | 259.5 (Ubuntu 26.04) | LGPL-2.1-or-later | the UKI |
| BusyBox (`busybox`) | 1.30.1 | GPL-2.0-only | the initramfs |
| OpenSSH (`sshd`, `sftp-server`) | 8.9p1 | BSD-style (OpenSSH) | the initramfs |
| gdisk, e2fsprogs, nvme-cli, efibootmgr | 1.0.8, 1.46.5, 1.16, 17 | GPL-2.0 | the initramfs |
| dosfstools | 4.2 | GPL-3.0-or-later | the initramfs |
| util-linux (and `fdisk`) | 2.37.2 | GPL-2.0-or-later and others | the initramfs |
| zstd | 1.4.8 | BSD-3-Clause or GPL-2.0 | the initramfs |
| glibc, OpenSSL, Kerberos, libsystemd, PAM, libaudit, libselinux, libwrap, zlib (the tools' libraries) | 2.35, 3.0.2, 1.19.2, 249.11, 1.4.0, 3.0.7, 3.3, 7.6, 1.2.11 | LGPL-2.1+, Apache-2.0, MIT, LGPL-2.1+, BSD-3/GPL, LGPL-2.1, public domain, BSD, Zlib | the initramfs |
| ukify (and python3-pefile) | 259.5 | LGPL-2.1-or-later (pefile: MIT) | build |
| dnsmasq (`dnsmasq-base`) | 2.92 | GPL-2.0-only or GPL-3.0-only | the mini-PC, the bench |
| Python | 3.14 (the mini-PC's) | PSF-2.0 | the mini-PC's HTTP server |
| QEMU, AAVMF (edk2) | 10.2.1, 2025.11 | GPL-2.0-only, BSD-2-Clause-Patent | the bench |
| shellcheck | 0.11.0 | GPL-3.0-or-later | checks |
