#!/bin/bash
# Prepare a Linux_for_Tegra tree for NVIDIA's flash tools the way Stereolabs' flash script for the
# ZED Box Mini (zedbox_mini_usb_flash_6.2_gpio.sh) prepares its own, minus the sample rootfs:
# NVIDIA's BSP, Stereolabs' device-tree tarball unpacked over it, and minimal-rootfs.sh's rootfs in
# place of the sample rootfs and apply_binaries.sh. Every download is checked against a SHA-256
# pinned here or in rootfs-debs.lock.
# spec: docs/32-fleet-ota.md#jetson-test-image
#
#   prepare-l4t.sh [36.4.4]
#
# The tree goes to $FJARR_L4T_OUT/l4t-<release>/Linux_for_Tegra (default: the spike's out/flash).
# Run on the host (the workstation or the mini-PC), never inside `dev`. Needs docker and curl.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
spike=$(dirname "$here")
out=${FJARR_L4T_OUT:-$spike/out/flash}
image=${FJARR_L4T_IMAGE:-fjarr-l4t-flash}
release=${1:-36.4.4}

case $release in
36.4.4)
	bsp_url=https://developer.download.nvidia.com/embedded/L4T/r36_Release_v4.4/release/Jetson_Linux_R36.4.4_aarch64.tbz2
	bsp_sha=a6ce11c22100ab0976e419959182417c83d62f4272501bc8714f2e076e010f3b
	# The tarball Stereolabs' 36.4.4 flash scripts fetch: three bootloader files, the board conf,
	# the kernel device trees.
	ovl_url=https://stereolabs.sfo2.cdn.digitaloceanspaces.com/utils/zed_boxes/zedbox_mini_device_trees_364.tar
	ovl_sha=d17993499723bd63c1287bd482400c43322d2b94cf87073654a3daed87c0c03e
	;;
*)
	echo "only 36.4.4, the factory release: Stereolabs publishes no flash script for $release" >&2
	exit 2
	;;
esac

fetch() { # fetch <url> <sha256> <file>: download unless present, then check
	local url=$1 sha=$2 file=$3
	if [ ! -f "$file" ]; then
		mkdir -p "$(dirname "$file")"
		curl -fsSL --retry 3 -o "$file.part" "$url"
		mv "$file.part" "$file"
	fi
	echo "$sha  $file" | sha256sum -c --quiet - || {
		echo "checksum mismatch: $file (delete it to download again)" >&2
		exit 1
	}
}

# The BSP may already sit in out/bsp (phase A1's downloads); read it there, write only to $out.
bsp=$spike/out/bsp/$(basename "$bsp_url")
[ -f "$bsp" ] || bsp=$out/dl/$(basename "$bsp_url")
fetch "$bsp_url" "$bsp_sha" "$bsp"
ovl=$out/dl/$(basename "$ovl_url")
fetch "$ovl_url" "$ovl_sha" "$ovl"
while read -r sha path _; do
	case $sha in '' | '#'*) continue ;; esac
	fetch "https://ports.ubuntu.com/ubuntu-ports/$path" "$sha" "$out/dl/debs/$(basename "$path")"
done <"$here/rootfs-debs.lock"

docker build -q -t "$image" -f "$here/Dockerfile.l4t" "$here" >/dev/null

tree=$out/l4t-$release
if [ ! -d "$tree/Linux_for_Tegra" ]; then
	mkdir -p "$tree"
	echo "unpacking $(basename "$bsp") into $tree"
	tar -C "$tree" -xjf "$bsp"
fi
# Stereolabs' script unpacks its tarball in the directory that holds Linux_for_Tegra, before
# apply_binaries.sh; tar replaces NVIDIA's jetson-orin-nano-devkit-super.conf symlink with a file.
tar -C "$tree" -xf "$ovl"
echo "unpacked $(basename "$ovl") over Linux_for_Tegra"

# The rootfs is root's, as in NVIDIA's flow (the sample rootfs is unpacked with sudo).
docker run --rm --network none \
	-v "$tree:$tree" -v "$out/dl/debs:/debs:ro" -v "$here:/flash:ro" \
	"$image" /flash/minimal-rootfs.sh "$tree/Linux_for_Tegra" /debs /flash/rootfs-debs.lock

{
	echo "prepared $(date -u +%FT%TZ) by flash/prepare-l4t.sh"
	echo "bsp      $bsp_sha  $(basename "$bsp")"
	echo "overlay  $ovl_sha  $(basename "$ovl")"
	echo "image    $(docker image inspect -f '{{.Id}}' "$image")  $image"
	grep -vE '^\s*(#|$)' "$here/rootfs-debs.lock" | awk '{print "deb      " $1 "  " $2}'
} >"$tree/fjarr-prepare.txt"
echo "ready: $tree/Linux_for_Tegra (provenance in $tree/fjarr-prepare.txt)"
