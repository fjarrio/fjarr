#!/bin/bash
# Generate the ZED Box Mini's flash images without a device: NVIDIA's l4t_initrd_flash.sh in its
# offline --no-flash mode, in the fjarr-l4t-flash container, on a tree from prepare-l4t.sh. Then
# list what it generated, with sizes, under out/flash/results/<layout>/.
# spec: docs/32-fleet-ota.md#jetson-test-image
#
#   gen-images.sh [stock|ab|initrd] [36.4.4]
#     stock   Stereolabs' flash command with --no-flash added: the QSPI, and the NVMe layout with one
#             APP (tools/kernel_flash/flash_l4t_external.xml), as on the factory box
#     ab      NVIDIA's NVMe root A/B layout (flash_l4t_t234_nvme_rootfs_ab.xml) with APP and APP_b
#             at 16 GiB each, the size docs/32 sets
#     initrd  only what NVIDIA's backup and restore tool boots first (l4t_backup_restore.sh runs
#             l4t_initrd_flash.sh --initrd): the RCM boot package and the flash initrd
#
# The container has no network and no host device but the loop devices: flash.sh builds the root
# filesystem image on one and mounts it, so it also gets CAP_SYS_ADMIN and no AppArmor profile,
# still less than NVIDIA's documented sudo on the host. /sys stays read-only, so NVIDIA's write to
# the host's USB autosuspend setting fails harmlessly. Run on the host, never inside `dev`.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
out=${FJARR_L4T_OUT:-$(dirname "$here")/out/flash}
image=${FJARR_L4T_IMAGE:-fjarr-l4t-flash}
layout=${1:-stock}
release=${2:-36.4.4}
tree=$out/l4t-$release
l4t=$tree/Linux_for_Tegra
board=p3509-a02-p3767-0000 # Stereolabs' board configuration, non-super power mode

grep -qx ready "$l4t/.fjarr-minimal-rootfs" 2>/dev/null ||
	{ echo "no prepared tree at $l4t: run prepare-l4t.sh $release" >&2; exit 1; }

# NVIDIA's offline mode: the board identity a connected module would report, from the table in
# tools/kernel_flash/README_initrd_flash.txt (Orin NX 16 GB).
board_env=(-e BOARDID=3767 -e BOARDSKU=0000 -e FAB=300)
qspi=(-p "-c bootloader/generic/cfg/flash_t234_qspi.xml")
case $layout in
stock)
	args=(--no-flash --external-device nvme0n1p1 -c tools/kernel_flash/flash_l4t_external.xml
		"${qspi[@]}" --showlogs --network usb0 "$board" internal)
	;;
ab)
	board_env+=(-e ROOTFS_AB=1)
	args=(--no-flash --external-device nvme0n1p1 -S 16GiB -c tools/kernel_flash/flash_l4t_t234_nvme_rootfs_ab.xml
		"${qspi[@]}" --showlogs --network usb0 "$board" internal)
	;;
initrd)
	args=(--no-flash --initrd --showlogs "$board" internal)
	;;
*)
	echo "usage: gen-images.sh [stock|ab|initrd] [release]" >&2
	exit 2
	;;
esac

docker build -q -t "$image" -f "$here/Dockerfile.l4t" "$here" >/dev/null
res=$out/results/$layout
rm -rf "$res"
mkdir -p "$res" "$out/logs"
log=$out/logs/gen-$layout-$(date -u +%Y%m%dT%H%M%SZ).log
echo "generating ($layout) in $l4t; log: $log"
start=$(date +%s)

# Inside: device nodes for the host's loop devices (the cgroup rule allows them), NVIDIA's tool, then
# detach any loop device it left on a file of this tree and give the tree back to the caller, except
# the rootfs, which stays root's.
# shellcheck disable=SC2016 # expanded by the container's shell
inner='
set -euo pipefail
for i in $(seq 0 255); do [ -e /dev/loop$i ] || mknod -m 0660 /dev/loop$i b 7 $i; done
done_() {
	for d in /sys/block/loop*; do
		f=$(cat "$d/loop/backing_file" 2>/dev/null) || continue
		case $f in "$TREE"/*) losetup -d "/dev/${d##*/}" && echo "detached ${d##*/} ($f)" ;; esac
	done
	find "$TREE" -path "$TREE/Linux_for_Tegra/rootfs" -prune -o -uid 0 -exec chown -h "$OWNER" {} +
}
trap done_ EXIT
cd "$TREE/Linux_for_Tegra"
./tools/kernel_flash/l4t_initrd_flash.sh "$@"
'
set +e
docker run --rm --network none \
	--cap-add SYS_ADMIN --security-opt apparmor=unconfined \
	--device /dev/loop-control --device-cgroup-rule 'b 7:* rmw' \
	-v "$tree:$tree" -e TREE="$tree" -e OWNER="$(id -u):$(id -g)" "${board_env[@]}" \
	"$image" bash -c "$inner" gen-images "${args[@]}" >"$log" 2>&1
rc=$?
set -e
echo "l4t_initrd_flash.sh exited $rc after $(($(date +%s) - start)) s"
[ "$rc" = 0 ] || { tail -30 "$log" >&2; exit "$rc"; }

# What NVIDIA's flash package holds: per storage, its index (one line per partition: device,
# name, start, size, file) and the files; then the flash initrd and the RCM boot command, which
# every layout builds.
imgs=$l4t/tools/kernel_flash/images
if [ "$layout" != initrd ]; then
	for dev in internal external; do
		cp "$imgs/$dev/flash.idx" "$res/$dev-flash.idx"
		cp "$imgs/$dev/flash.cfg" "$res/$dev-flash.cfg"
		(cd "$imgs/$dev" && find . -maxdepth 1 -type f ! -name 'flash.idx' ! -name 'flash.cfg' -printf '%f\t%s\t%k\n' |
			sort) >"$res/$dev-files.tsv"
	done
fi
cp "$l4t/tools/kernel_flash/initrdflashparam.txt" "$l4t/tools/kernel_flash/initrdflashimgmap.txt" "$res/"
cp "$l4t/bootloader/flashcmd.txt" "$res/rcm-flashcmd.txt"
(cd "$l4t/bootloader" && find boot0.img rcmboot_blob -type f -printf '%p\t%s\n' | sort) >"$res/rcm-files.tsv"
cp "$log" "$res/gen.log"
echo "results: $res"
