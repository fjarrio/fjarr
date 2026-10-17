# board — the ZED Box Mini board package (M8 slice 8.0)

The board package that [docs/32](../../../docs/32-fleet-ota.md#jetson-test-image) ("The board
package") specifies and [docs/18](../../../docs/18-open-questions.md) #45 records: Fjarr's own
bootloader files and carrier device tree for the ZED Box Mini (Orin NX 16 GB on Stereolabs'
carrier), and the pin for Stereolabs' driver package. Both Jetson recipes take them, Yocto
first (`../yocto/`), then NixOS. Spike code from phase A (off the box); nothing here has
touched the box, and nothing here ships yet.

| File | What |
|---|---|
| `bct/zedbox-mini-mb1-pinmux-gpio.patch` | Stereolabs' MB1 pinmux and GPIO settings as a patch to NVIDIA's two BSD-3-Clause files for them, LF line endings |
| `bct/tegra234-mb2-bct-misc-p3767-0000-zedbox-mini.dts` | Fjarr's own MB2 file: NVIDIA's MB2 common file, included by name at build time, with the carrier EEPROM's read size set to 0 |
| `dt/tegra234-p3768-0000+p3767-0000-zedbox-mini.dts` | Fjarr's carrier device tree: NVIDIA's tree for the module on the P3768 carrier, included whole, plus the carrier's changes |
| `dt/carrier.diff` | the carrier's changes as `dtbdiff.py` prints them: what the checks expect |
| `stereolabs.lock` | Stereolabs' driver package for L4T 36.5.2: URL, SHA-256 (hex and SRI), size |
| `checks.lock` | the checks' own inputs: NVIDIA's BSPs and public sources for 36.4.4 and 36.5.2, Stereolabs' published device-tree tarballs |
| `fetch-check.sh` | fetches the driver package by its pin and lists it, installing nothing |
| `check-bct.sh`, `check-dtb.sh` | the bootloader files and the device tree against both releases, Stereolabs' files and the lab box |
| `selftest.sh` | the checks' failure modes, offline: each comparison must fail on a wrong input |
| `dtbdiff.py` | structural diff of two device trees that ignores phandle numbering |
| `lib.sh`, `run.sh`, `Dockerfile` | shared helpers; the runner; the tools image (dtc, cpp, patch, dpkg-deb, shellcheck) |

Run on the host, never inside `dev`; each script runs in the tools image and writes only to
`out/board/` (and `.lab/board/` for the box's partitions):

```bash
spikes/ota-jetson/board/run.sh selftest.sh      # seconds, offline
spikes/ota-jetson/board/run.sh fetch-check.sh   # the pinned driver package, 1.6 MB
spikes/ota-jetson/board/run.sh check-bct.sh     # needs both BSPs (1.5 GB, fetched once)
spikes/ota-jetson/board/run.sh check-dtb.sh     # also NVIDIA's public sources (460 MB)
spikes/ota-jetson/board/run.sh lint             # shellcheck, Python syntax
```

Downloads land in `out/board/dl/<lock entry>/`, one directory each, and are checked against
their SHA-256 on every use; a file already downloaded can be put there by hand.
`check-dtb.sh` compares with the box's kernel-dtb partitions when the factory backup is in
`.lab/zedbox-backup/` (or `FJARR_ZEDBOX_BACKUP`), and skips that otherwise.

## The bootloader files

**MB1 pinmux and GPIO.** The `p3509-a02-p3767-0000` board configuration, the one Stereolabs
flashes with, takes its pinmux from NVIDIA's
`bootloader/generic/BCT/tegra234-mb1-bct-pinmux-p3767-hdmi-a03.dtsi`, which includes
`bootloader/tegra234-mb1-bct-gpio-p3767-hdmi-a03.dtsi`. Both are BSD-3-Clause and
byte-identical in L4T 36.4.4 and 36.5.2. The patch carries what Stereolabs' published
`zedbox_mini_device_trees_364.tar` changes in them, apart from a generator comment: DAP4
(PA4–PA7) from I2S4 to GPIO, PZ6 driven, PQ6 and PN1 no longer tristated, and MB1's GPIO lists
to match, with PZ6 and PG6 set high. Apply it with `patch -p1 -F0` in `Linux_for_Tegra/`.

NVIDIA ships both files with CRLF line endings. The patch is LF because meta-tegra converts
them (`tegra-binaries-36.5.2.inc` runs dos2unix on `*.dts*`), and the Yocto layer's
`tegra-bootfiles` bbappend applies it to copies of them there, never to the shared BSP tree,
and installs them as `…-zedbox-mini.dtsi`. jetpack-nixos converts nothing (master `d4c4a51`:
`flashScriptOverrides.patches` and `.postPatch` go onto the `flash-tools` derivation over
NVIDIA's files as shipped), so it needs one of:

- the CRLF form of the patch, which `check-bct.sh` writes to
  `out/board/bct/zedbox-mini-mb1-pinmux-gpio.crlf.patch` and checks, in
  `flashScriptOverrides.patches`;
- a `postPatch` that strips the two files' CRs and then applies the LF patch;
- whole patched files copied in by `postPatch` (BSD-3, so they may be committed, but the patch
  stays the source).

**MB2.** The carrier has no board EEPROM, and with NVIDIA's settings MB2 stops before UEFI
reading one ([docs/32](../../../docs/32-fleet-ota.md#jetson-test-image)). NVIDIA's
`tegra234-mb2-bct-misc-p3767-0000.dts` states no licence and includes the proprietary
`tegra234-mb2-bct-common.dtsi`, so Fjarr's file is its own: it includes that common file by
name and sets only the carrier EEPROM block, read size 0. meta-tegra takes it through
`TEGRA_FLASHVAR_MB2BCT_CFG`; in jetpack-nixos a `postPatch` copies it over NVIDIA's p3767 file,
the file the board configuration names. Compiled from the same includes it gives the same tree
as Stereolabs' MB2 file, on both releases.

Every reflash and every capsule carries both: meta-tegra's capsule recipe builds from the same
`tegraflash` inputs.

## The carrier device tree

Fjarr's tree includes NVIDIA's `tegra234-p3768-0000+p3767-0000-nv.dts` (from
`kernel_oot_modules_src.tbz2` in NVIDIA's public sources) and adds the carrier's changes:

- **USB:** the recovery port (usb2-0) gains a micro-USB `connector` with VBUS sensed on PZ1;
  the device controller takes only usb2-0; the host controller takes usb2-1 to usb2-3 and
  usb3-0 to usb3-2, with usb3-2 enabled and the USB 3 ports' USB 2 companions remapped;
- **`/bus@0/pps`:** a `pps-gpio` input on PA6;
- **`/gpio-keys/key-suspend`:** `KEY_SLEEP` on PG2, active low.

It is a source-level change, not a DT overlay: the USB controllers name NVIDIA's padctl lanes,
which carry no labels, and dtc cannot emit an overlay fixup for a reference by path. Built
with NVIDIA's include list and flags (`dt_build` in `lib.sh`, the same list tegra-demo-distro's
`tegrademo-devicetree` and the Yocto layer's `zedbox-mini-devicetree` recipe use), it keeps
NVIDIA's labels, so Stereolabs' camera overlay still merges into it at build time. For
jetpack-nixos the tree cannot be one of `hardware.deviceTree.overlays` for the same reason: it
is built from jetpack-nixos's own NVIDIA sources (`hardware.deviceTree.dtbSource.src`) with the
include list of its `dtboBuildExtraIncludePaths."6"` plus `nv-platform/` (phase G).

The node contents follow the box's factory tree exactly, including two departures from the
upstream binding that the 5.15 kernel accepts: the connector's `compatible` order
(`"usb-b-connector", "gpio-usb-b-connector"`; `usb-conn-gpio` matches either position) and
`vbus-gpio` for `vbus-gpios` (gpiolib takes both). Correcting them waits for a test on the box.

## Measured (2026-10-17)

All checks pass: `selftest.sh` 22, `fetch-check.sh` 16, `check-bct.sh` 44, `check-dtb.sh` 11;
`lint` clean. Tools: the image above (Ubuntu 26.04: dtc 1.7.2, cpp 15.2, GNU patch 2.8).

**Bootloader files**, on L4T 36.4.4 and on 36.5.2:

| Check | Result |
|---|---|
| NVIDIA's pinmux, GPIO and MB2 misc files, 36.4.4 against 36.5.2 | byte-identical (SHA-256 `10c470fe…`, `14f2e544…`, `2dcf4c37…`) |
| LF patch on LF copies (meta-tegra) | applies, no offset, no fuzz |
| CRLF patch on NVIDIA's files as shipped (jetpack-nixos) | applies, no offset, no fuzz; every line stays CRLF |
| LF patch on NVIDIA's files as shipped | refused |
| Patched files against Stereolabs' | equal from line 2 (line 1: the generator comment) |
| Pinmux and GPIO compiled as tegraflash does, in the meta-tegra and the jetpack-nixos layout | byte-identical to Stereolabs' (`0f3a90d3…` on both releases), not to NVIDIA's |
| MB2 compiled | byte-identical to Stereolabs' (36.4.4 `4629d008…`, 36.5.2 `2b1ef462…`: NVIDIA's MB2 common file differs between the releases), not to NVIDIA's; the carrier EEPROM's read size 0, the module EEPROM (0xa0, 256 bytes) as NVIDIA's |
| Stereolabs' 36.5 tarball (dated 2026-06-02) against its 36.4 one | the same three files |

**Carrier device tree:**

| Check | Result |
|---|---|
| NVIDIA's tree built here from its public sources against the DTB in its BSP | byte-identical, on both releases: the build is NVIDIA's |
| Fjarr's tree against NVIDIA's, 36.4.4 and 36.5.2 | exactly `dt/carrier.diff` |
| Stereolabs' 36.4.4 DTB against NVIDIA's 36.4.4 | exactly `dt/carrier.diff` |
| Fjarr's 36.4.4 tree against Stereolabs' DTB | the same tree, phandle numbers included; the bytes differ only in where the two new nodes sit and in the pps node's property order |
| Fjarr's 36.4.4 tree against the box's `A_kernel-dtb` and `B_kernel-dtb` | the same tree (both partitions hold Stereolabs' DTB, zero-padded to 768 KiB) |
| Stereolabs' `tegra234-p3768-camera-zedbox-mini-sl-overlay.dtbo` (1.4.4 for 36.5.2) | merges into Fjarr's 36.5.2 tree as into NVIDIA's; the merged trees differ by exactly `dt/carrier.diff`. By hand, the package's other eight overlays merge too |

NVIDIA's public sources, recorded in `checks.lock`: 36.4.4 `cd4fa3bd…0661` (226,164,437
bytes), 36.5.2 `4347a718…01d5` (232,402,561 bytes); their `kernel_oot_modules_src.tbz2`
matches the SHA-1 NVIDIA ships beside it.

**Stereolabs' driver package** (`fetch-check.sh`): SHA-256, size and SRI form as pinned;
`stereolabs-zedbox-mini` 1.4.4-SL-MAX9296-ZEDBOX-MINI-L4T36.5.2, arm64, Pre-Depends
`nvidia-l4t-core (>> 36.5-0), nvidia-l4t-core (<< 36.6-0)`; it carries `preinst`,
`postinst` and `postrm`, which no Fjarr image runs. Its data.tar holds 61 files: the seven modules docs/18 #45 lists
under `/usr/lib/modules/5.15.199-tegra/kernel/drivers/stereolabs/`, the replacement `host1x.ko`
and `tegra-drm.ko` under `/tmp/nvidia-capture-patches/`, and nine camera overlays in `/boot`.

**Seen, left as Stereolabs ships it:** PZ6 and PG6 sit in both the MB1 `gpio-input` and
`gpio-output-high` lists; what MB1 makes of that is read on the box (phase B, `gpioinfo`). The
host controller names the usb2-3 lane, which stays disabled: NVIDIA's 5.15 PHY core logs
"Requested PHY is disabled" and the controller goes on without it. The recovery port keeps
NVIDIA's role switch and Type-C endpoint (`fusb301@25`) beside the new connector, and usb3-1 and
usb3-2 both take usb2-1 as companion.

## Licences

No file of NVIDIA's or Stereolabs' is in this directory. The patch holds context lines of
NVIDIA's BSD-3-Clause files, whose notice the patched files keep, and the carrier's settings,
which Fjarr's own files restate. NVIDIA's proprietary MB2 common file and its GPL-2.0-only
kernel device-tree sources are included by path at build time, and Stereolabs' files are
fetched by their pins, never committed.

**Open for Erik:** the files here carry no licence of their own yet. The repository's planned
AGPL-3.0 does not combine with what they are built into: the device tree compiles together
with NVIDIA's GPL-2.0-only sources into one DTB, and the MB2 file with NVIDIA's proprietary
common file into one BCT. A permissive licence would fit, for example BSD-3-Clause for the
patch and the MB2 file (as NVIDIA's MB1 files) and `GPL-2.0-only OR MIT` for the device tree
(as many upstream kernel trees).

## What remains

- The recipes take these files: the Yocto layer's machine sets `FJARR_BOARD_BCT` and
  `FJARR_BOARD_DT` to 1 (its install steps, run by hand on 36.5.2's files, give the files
  `check-bct.sh` compiles); NixOS in phase G.
- The capsule's contents (phase A): its MB2 BCT must show the carrier EEPROM read size 0 and
  its MB1 BCT this pinmux.
- On the box: MB2 past its EEPROM step with Fjarr's firmware from RAM (D0); `i2cdetect` showing
  nothing at 0x57 and `gpioinfo` for PZ6 and PG6 (B); USB, the micro-USB role, PPS and the
  suspend key under Fjarr's tree.
- The kernel modules from source (#47) and the camera overlay merged in the image (8.4).
- The licence question above; rows in docs/14 for the tools image and the build-time inputs.
