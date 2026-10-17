#!/bin/bash
# Regenerate rootfs-debs.lock: every file NVIDIA's flash tools copy out of Linux_for_Tegra/rootfs,
# mapped to the Ubuntu 22.04 arm64 package that ships it. The paths come from NVIDIA's own lists in
# the tree: the flash initrd's (generate_flash_initrd in tools/kernel_flash/l4t_initrd_flash_internal.sh)
# and the recovery image's (tools/ota_tools/version_upgrade/recovery_copy_binlist.txt, its jammy and
# all lines). Kernel modules are left out: they come from the BSP's own tarballs.
# spec: docs/32-fleet-ota.md#jetson-test-image
#
#   lock-rootfs-debs.sh [<Linux_for_Tegra>]   writes rootfs-debs.lock beside this script
#
# Runs apt-file in a throwaway fjarr-l4t-flash container with network, against the jammy release
# pocket on ports.ubuntu.com (frozen, so the pool keeps every file); apt checks the signed index
# the Contents and Packages data come from. Prints paths no package ships; minimal-rootfs.sh makes
# those itself.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
image=${FJARR_L4T_IMAGE:-fjarr-l4t-flash}

if [ "${1:-}" != --inside ]; then
	l4t=$(realpath "${1:-$(dirname "$here")/out/flash/l4t-36.4.4/Linux_for_Tegra}")
	docker build -q -t "$image" -f "$here/Dockerfile.l4t" "$here" >/dev/null
	new=$(mktemp)
	docker run --rm -v "$here:/flash:ro" -v "$l4t:/l4t:ro" "$image" /flash/lock-rootfs-debs.sh --inside /l4t >"$new"
	chmod 0644 "$new"
	mv "$new" "$here/rootfs-debs.lock"
	echo "wrote $here/rootfs-debs.lock" >&2
	exit 0
fi

l4t=${2:?Linux_for_Tegra}
arch=aarch64-linux-gnu
{
	apt-get update -qq
	apt-get install -qq -y apt-file
	# Then the arm64 release pocket alone, so apt-file indexes nothing else. Plain HTTP as in
	# Ubuntu's own sources: apt checks the signature of the index (the image has no CA bundle).
	dpkg --add-architecture arm64
	: >/etc/apt/sources.list
	echo "deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports jammy main universe" \
		>/etc/apt/sources.list.d/jammy-arm64.list
	apt-get update -qq
} >/dev/null 2>&1
[ -n "$(find /var/lib/apt/lists -name '*jammy_Contents-arm64*' -print -quit)" ] ||
	{ echo "no arm64 Contents index fetched" >&2; exit 1; }

# The rootfs-relative paths both lists copy, modules excluded.
# shellcheck disable=SC2016 # ${ROOTFS_DIR} is NVIDIA's literal text
paths=$(
	{
		grep -oE '"\$\{ROOTFS_DIR\}/[^"]+"' "$l4t/tools/kernel_flash/l4t_initrd_flash_internal.sh" |
			tr -d '"' | sed 's|^${ROOTFS_DIR}/||'
		grep -E '^(jammy|all):<ROOTFS>/' "$l4t/tools/ota_tools/version_upgrade/recovery_copy_binlist.txt" |
			cut -d: -f2 | sed -e 's|^<ROOTFS>/||' -e "s|<ARCH>|$arch|g"
	} | grep -vE '^((usr/)?lib/modules/|etc/nv_boot_control.conf$)' | sort -u
)

# Where a merged-/usr path may sit in a package: under / or under /usr.
alt() {
	case $1 in
	usr/bin/* | usr/sbin/* | usr/lib/*) echo "${1#usr/}" ;;
	bin/* | sbin/* | lib/*) echo "usr/$1" ;;
	*) echo "$1" ;;
	esac
}
patterns=$(mktemp)
while read -r p; do printf '/%s\n/%s\n' "$p" "$(alt "$p")"; done <<<"$paths" | sort -u >"$patterns"
declare -A shipped # /path -> packages that ship it
found=$(apt-file search -a arm64 -F -f "$patterns")
while IFS=': ' read -r pkg path; do
	shipped[$path]+="${shipped[$path]:+ }$pkg"
done <<<"$found"

declare -A files # package -> the listed files it ships
unshipped=()
choices=()
while read -r p; do
	pkgs=$(tr ' ' '\n' <<<"${shipped[/$p]:-} ${shipped[/$(alt "$p")]:-}" | grep . | sort -u || true)
	if [ -z "$pkgs" ]; then
		unshipped+=("$p")
		continue
	fi
	if [ "$(wc -l <<<"$pkgs")" -gt 1 ]; then
		# Where several packages ship a path, the one a standard Ubuntu 22.04 install has.
		pick=$(grep -xE 'busybox-static|iputils-ping' <<<"$pkgs" || true)
		[ -n "$pick" ] || { echo "no preference among $(xargs <<<"$pkgs") for $p" >&2; exit 1; }
		choices+=("$p: $pick, not $(grep -vx "$pick" <<<"$pkgs" | xargs)")
		pkgs=$pick
	fi
	case " ${files[$pkgs]:-} " in *" ${p##*/} "*) ;; *) files[$pkgs]+="${files[$pkgs]:+ }${p##*/}" ;; esac
done <<<"$paths"

echo "# The arm64 packages of the minimal rootfs (minimal-rootfs.sh): every file NVIDIA's flash tools"
echo "# copy out of Linux_for_Tegra/rootfs, from the flash initrd's list and the recovery image's list."
echo "# From Ubuntu 22.04's release pocket, which is frozen, so the pool keeps these files. Made by"
echo "# lock-rootfs-debs.sh on $(date -u +%F) from the signed jammy index on ports.ubuntu.com."
echo "# spec: docs/32-fleet-ota.md#jetson-test-image"
echo "#"
echo "# <sha256>  <path under https://ports.ubuntu.com/ubuntu-ports/>  <files NVIDIA's lists take from it>"
for pkg in $(printf '%s\n' "${!files[@]}" | sort); do
	apt-cache show --no-all-versions "$pkg:arm64" |
		awk -v f="${files[$pkg]}" '/^SHA256:/{s=$2} /^Filename:/{n=$2} END{print s "  " n "  " f}'
done
for c in "${choices[@]}"; do echo "# chosen for $c"; done
for p in "${unshipped[@]}"; do echo "# in no package: $p"; done
