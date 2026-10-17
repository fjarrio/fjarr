#!/bin/bash
# The least Linux_for_Tegra/rootfs NVIDIA's flash tools need, in place of the 36.4.4 sample rootfs
# and apply_binaries.sh: every file they copy out of rootfs, which NVIDIA's own two lists name (the
# flash initrd's and the recovery image's), from the arm64 packages in rootfs-debs.lock, plus the
# BSP's kernel modules, extlinux.conf for flash.sh, and merged /usr as on Ubuntu 22.04. Enough to
# generate every image (--no-flash) and to boot the flash initrd for NVIDIA's backup and restore
# tools; not enough for a network flash, where the box chroots into this rootfs over NFS and runs
# the flash script with its tools (README.md).
# spec: docs/32-fleet-ota.md#jetson-test-image
#
# Runs as root inside the fjarr-l4t-flash container; prepare-l4t.sh calls it:
#   minimal-rootfs.sh <Linux_for_Tegra> <dir with the .debs> <rootfs-debs.lock>
set -euo pipefail

l4t=$(realpath "${1:?Linux_for_Tegra}")
debs=$(realpath "${2:?directory with the .deb files}")
lock=$(realpath "${3:?rootfs-debs.lock}")
rootfs=$l4t/rootfs
marker=$l4t/.fjarr-minimal-rootfs
arch=aarch64-linux-gnu

[ "$(id -u)" = 0 ] || { echo "run as root, inside the fjarr-l4t-flash container" >&2; exit 1; }
if [ ! -f "$l4t/flash.sh" ] || [ ! -d "$rootfs" ]; then
	echo "$l4t is not a Linux_for_Tegra tree" >&2
	exit 1
fi

# Replace only an empty rootfs (NVIDIA ships it holding README.txt) or one this script made, whole
# or half built: the marker says "building" until the checks below pass, then "ready".
if [ ! -e "$marker" ] && [ -n "$(find "$rootfs" -mindepth 1 -maxdepth 1 ! -name README.txt -print -quit)" ]; then
	echo "$rootfs holds more than NVIDIA's README.txt and was not made here; not replacing it" >&2
	exit 1
fi
echo building >"$marker"
find "$rootfs" -mindepth 1 -maxdepth 1 ! -name README.txt -exec rm -rf {} +

install -d -m 0755 "$rootfs/usr/bin" "$rootfs/usr/sbin" "$rootfs/usr/lib" "$rootfs/etc" "$rootfs/boot/extlinux"
ln -s usr/bin "$rootfs/bin"
ln -s usr/sbin "$rootfs/sbin"
ln -s usr/lib "$rootfs/lib"
# As apply_binaries.sh does; flash.sh refuses a rootfs without it.
install -m 0644 "$l4t/bootloader/extlinux.conf" "$rootfs/boot/extlinux/extlinux.conf"

n=0
while read -r sha path _; do
	case $sha in '' | '#'*) continue ;; esac
	deb=$debs/$(basename "$path")
	echo "$sha  $deb" | sha256sum -c --quiet - || { echo "checksum mismatch: $deb" >&2; exit 1; }
	# The packages' files only; their maintainer scripts never run.
	dpkg-deb --fsys-tarfile "$deb" | tar -x --keep-directory-symlink -C "$rootfs"
	n=$((n + 1))
done <"$lock"
echo "unpacked $n packages from $(basename "$lock")"
# The one listed file no package ships: openssh-server's postinst makes it from this copy, unchanged
# with the default answers.
install -m 0644 "$rootfs/usr/share/openssh/sshd_config" "$rootfs/etc/ssh/sshd_config"

# The kernel modules, unpacked as apply_binaries.sh unpacks them, then indexed as the kernel
# package's postinst does (depmod -a), which also makes the index files the recovery image lists.
for t in kernel_supplements.tbz2 kernel_oot_modules.tbz2; do
	(cd "$rootfs" && tar --keep-directory-symlink -I lbzip2 -xpmf "$l4t/kernel/$t")
	echo "unpacked kernel/$t"
done
kver=$(strings "$l4t/kernel/Image" | grep -oE 'Linux version [0-9a-zA-Z.-]+[+]* ' | cut -d' ' -f3 | head -1)
[ -n "$kver" ] || { echo "no kernel version in $l4t/kernel/Image" >&2; exit 1; }
depmod -b "$rootfs" "$kver"

# Check every path NVIDIA's two lists copy, read from NVIDIA's files so the check cannot drift. The
# recovery image only warns about a missing file, so a gap here would otherwise go unnoticed.
missing=0
while read -r p; do
	if [ ! -e "$p" ] && ! compgen -G "$p*" >/dev/null; then
		echo "missing: ${p#"$rootfs"/}" >&2
		missing=1
	fi
done < <(
	{
		grep -oE '"\$\{ROOTFS_DIR\}[^"]*"' "$l4t/tools/kernel_flash/l4t_initrd_flash_internal.sh" |
			tr -d '"' | sed -e "s|\${ROOTFS_DIR}|$rootfs|" -e "s|\${KERNEL_VERSION}|$kver|"
		grep -E '^(jammy|all):<ROOTFS>/' "$l4t/tools/ota_tools/version_upgrade/recovery_copy_binlist.txt" |
			cut -d: -f2 | sed -e "s|<ROOTFS>|$rootfs|" -e "s|<ARCH>|$arch|g" -e "s|<KERNEL_VERSION>|$kver|g"
	} | grep -vx "$rootfs/etc/nv_boot_control.conf" | sort -u # nv_boot_control.conf: written by flash.sh
)
for m in spi-tegra210-quad tegra-mce nvethernet; do
	grep -q "/$m.ko:" "$rootfs/lib/modules/$kver/modules.dep" || { echo "modules.dep lacks $m" >&2; missing=1; }
done
[ "$missing" = 0 ] || exit 1

echo ready >"$marker"
echo "minimal rootfs for $kver: $(find "$rootfs" -xdev | wc -l) entries, $(du -sh --apparent-size "$rootfs" | cut -f1)"
