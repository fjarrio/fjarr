#!/usr/bin/env bash
# Check Fjarr's carrier device tree: build it, as NVIDIA's kernel build does, on NVIDIA's L4T
# 36.4.4 and 36.5.2 device-tree sources and compare the trees structurally (board/dtbdiff.py:
# phandle numbering ignored). Writes under out/board/ and, for the lab box's partitions,
# .lab/board/; installs and flashes nothing.
#   spikes/ota-jetson/board/run.sh check-dtb.sh
# spec: docs/32-fleet-ota.md#jetson-test-image ("The board package")
#
# - NVIDIA's own tree built here from its public sources equals the DTB in NVIDIA's BSP, which
#   shows the build is NVIDIA's;
# - on 36.4.4, Fjarr's tree equals Stereolabs' published DTB and, when the lab backup is there
#   (.lab/zedbox-backup, or FJARR_ZEDBOX_BACKUP), the box's A and B kernel-dtb partitions;
# - on both releases, Fjarr's tree changes NVIDIA's by exactly dt/carrier.diff, as Stereolabs'
#   36.4.4 tree changes NVIDIA's 36.4.4 one;
# - Stereolabs' camera overlay for the box, from the pinned package, merges into Fjarr's 36.5.2
#   tree as into NVIDIA's.
set -euo pipefail
# shellcheck source=SCRIPTDIR/lib.sh
. "$(dirname "$0")/lib.sh"
need curl tar bzip2 cpp dtc fdtoverlay dpkg-deb python3 cmp diff zstd sha256sum

lock=$board_dir/checks.lock
dts=$board_dir/dt/tegra234-p3768-0000+p3767-0000-zedbox-mini.dts
expected=$board_dir/dt/carrier.diff
nvdtb=kernel/dtb/tegra234-p3768-0000+p3767-0000-nv.dtb
backup=${FJARR_ZEDBOX_BACKUP:-$spike_dir/.lab/zedbox-backup}
work=$out/dtb
rm -rf "$work"
mkdir -p "$work"

for v in 36.4.4 36.5.2; do
  echo "== L4T $v"
  bsp=$(fetch "$lock" "l4t-$v-bsp")
  src=$(fetch "$lock" "l4t-$v-public-sources")
  extract "$bsp" "$out/l4t/$v/dtb" "Linux_for_Tegra/$nvdtb"
  extract "$src" "$out/l4t/$v/sources" Linux_for_Tegra/source/kernel_oot_modules_src.tbz2
  extract "$out/l4t/$v/sources/Linux_for_Tegra/source/kernel_oot_modules_src.tbz2" \
    "$out/l4t/$v/oot" hardware/nvidia
  t=$out/l4t/$v/oot/hardware/nvidia
  shipped=$out/l4t/$v/dtb/Linux_for_Tegra/$nvdtb
  w=$work/$v
  mkdir -p "$w"
  dt_build "$t/t23x/nv-public/nv-platform/tegra234-p3768-0000+p3767-0000-nv.dts" "$w/nvidia.dtb" "$t"
  dt_build "$dts" "$w/zedbox-mini.dtb" "$t"
  check "NVIDIA's tree built from its public sources = the DTB in NVIDIA's BSP" \
    same_tree "$shipped" "$w/nvidia.dtb"
  if cmp -s "$shipped" "$w/nvidia.dtb"; then echo "      (byte-identical)"; fi
  check "Fjarr's tree changes NVIDIA's DTB by exactly dt/carrier.diff" \
    matches_diff "$expected" "$shipped" "$w/zedbox-mini.dtb"
  echo "      sha256 $(sha256_of "$w/zedbox-mini.dtb" | cut -c1-16)  ${w#"$spike_dir"/}/zedbox-mini.dtb"
done

echo "== Stereolabs' tree for L4T 36.4.4"
sl=$(fetch "$lock" stereolabs-device-trees-364)
extract "$sl" "$out/stereolabs/364-dtb" "Linux_for_Tegra/$nvdtb"
sldtb=$out/stereolabs/364-dtb/Linux_for_Tegra/$nvdtb
check "Stereolabs' DTB changes NVIDIA's 36.4.4 DTB by exactly dt/carrier.diff" \
  matches_diff "$expected" "$out/l4t/36.4.4/dtb/Linux_for_Tegra/$nvdtb" "$sldtb"
check "Fjarr's tree on 36.4.4 = Stereolabs' DTB" same_tree "$sldtb" "$work/36.4.4/zedbox-mini.dtb"

# docs/18 #45: the images merge Stereolabs' camera overlay into the tree at build time, which
# needs NVIDIA's labels (__symbols__) in Fjarr's tree. Only the overlay file is taken out of
# the pinned package's data.tar; nothing is installed.
echo "== Stereolabs' camera overlay (stereolabs.lock), merged at build time"
deb=$(fetch "$board_dir/stereolabs.lock" stereolabs-zedbox-mini)
ovl=boot/tegra234-p3768-camera-zedbox-mini-sl-overlay.dtbo
o=$work/camera-overlay
mkdir -p "$o"
dpkg-deb --fsys-tarfile "$deb" | tar -x -C "$o" "./$ovl"
nv52=$out/l4t/36.5.2/dtb/Linux_for_Tegra/$nvdtb
check "the overlay merges into Fjarr's 36.5.2 tree (fdtoverlay)" \
  fdtoverlay -i "$work/36.5.2/zedbox-mini.dtb" -o "$o/zedbox-mini+camera.dtb" "$o/$ovl"
check "  ... as into NVIDIA's" fdtoverlay -i "$nv52" -o "$o/nvidia+camera.dtb" "$o/$ovl"
check "  ... and the merged trees differ by exactly dt/carrier.diff" \
  matches_diff "$expected" "$o/nvidia+camera.dtb" "$o/zedbox-mini+camera.dtb"

echo "== the lab box (factory image, L4T 36.4.4)"
parts=()
if [ -d "$backup" ]; then
  shopt -s nullglob
  parts=("$backup"/*[AB]_kernel-dtb.img.zst)
  shopt -u nullglob
fi
if [ "${#parts[@]}" -eq 0 ]; then
  echo "SKIP  no kernel-dtb partition dumps in ${backup#"$spike_dir"/}"
else
  private=$spike_dir/.lab/board
  mkdir -p "$private"
  chmod 700 "$private"
  for p in "${parts[@]}"; do
    img=$private/$(basename "$p" .zst)
    part=$(basename "$img" .img)
    zstd -q -d -f "$p" -o "$img"
    check "Fjarr's tree on 36.4.4 = the box's ${part#*-} partition" \
      same_tree "$img" "$work/36.4.4/zedbox-mini.dtb"
  done
fi
finish
