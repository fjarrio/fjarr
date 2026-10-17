#!/bin/bash
# Copy the network rescue's userland out of the Ubuntu 22.04 arm64 container (Dockerfile.userland)
# into the initramfs tree: BusyBox for the shell and the small tools, the real disk and boot
# tools beside it, every library they load, and reboot-recovery compiled from source.
#   collect-userland.sh <work dir> <rescue source dir>   (run by build.sh inside the container)
# The tree goes to <work dir>/root, the package versions to <work dir>/packages.txt.
# spec: docs/32-fleet-ota.md#jetson-test-image ("Three rescue layers", item 2)
set -euo pipefail
work=${1:?work dir}
src=${2:?rescue source dir}
root=$work/root

# Merged /usr, as on Ubuntu: one copy of each binary and library.
mkdir -p "$root/usr/bin" "$root/usr/sbin" "$root/usr/lib"
ln -sfn usr/bin "$root/bin"
ln -sfn usr/sbin "$root/sbin"
ln -sfn usr/lib "$root/lib"

# Every BusyBox applet at its usual place; the real tools below replace some of them. Ubuntu's
# ordinary busybox, not busybox-static: the static build's shell runs its own applets ahead of
# $PATH, so `mke2fs`, `fdisk` and `blkdiscard` would be BusyBox's, not the real tools.
install -m 0755 /bin/busybox "$root/usr/bin/busybox"
busybox --list-full | while read -r p; do
	case $p in
	*/busybox) ;; # BusyBox lists itself
	bin/* | usr/bin/*) ln -sfn busybox "$root/usr/bin/${p##*/}" ;;
	sbin/* | usr/sbin/*) ln -sfn ../bin/busybox "$root/usr/sbin/${p##*/}" ;;
	esac
done

copy_libs() { # the libraries an ELF file loads, at the paths the loader finds them
	{ ldd "$1" || true; } | while read -r a b c _; do
		local lib=
		case "$a $b $c" in
		*" => /"*) lib=$c ;;
		/*) lib=$a ;;
		esac
		[ -n "$lib" ] || continue
		[ -e "$root$lib" ] && continue
		mkdir -p "$root$(dirname "$lib")"
		cp -L --preserve=mode "$lib" "$root$lib"
	done
}

copy_bin() { # <path in the container> [<name in the tree>]
	local from to=${2:-$1}
	from=$(readlink -f "$1")
	case $to in /bin/* | /sbin/*) to=/usr$to ;; esac
	to=$root$to
	mkdir -p "$(dirname "$to")"
	rm -f "$to"
	cp --preserve=mode "$from" "$to"
	copy_libs "$from"
}

copy_libs /bin/busybox
for t in sshd sgdisk mke2fs e2fsck resize2fs tune2fs dumpe2fs chattr lsattr nvme efibootmgr mkfs.fat \
	fsck.fat fatlabel zstd lsblk blkid wipefs blkdiscard findmnt partx sfdisk fdisk; do
	copy_bin "$(command -v "$t")"
done
copy_bin /usr/lib/openssh/sftp-server
for l in mkfs.ext4:mke2fs mkfs.ext2:mke2fs fsck.ext4:e2fsck e2label:tune2fs mkfs.vfat:mkfs.fat \
	fsck.vfat:fsck.fat; do
	ln -sfn "${l#*:}" "$root/usr/sbin/${l%%:*}"
done
ln -sfn zstd "$root/usr/bin/unzstd"
ln -sfn zstd "$root/usr/bin/zstdcat"
install -D -m 0644 /etc/mke2fs.conf "$root/etc/mke2fs.conf"

gcc -O2 -Wall -Wextra -Werror -o "$root/usr/sbin/reboot-recovery" "$src/reboot-recovery.c"
copy_libs "$root/usr/sbin/reboot-recovery"

# What was copied, for the build manifest.
dpkg-query -W busybox openssh-server openssh-sftp-server gdisk e2fsprogs nvme-cli efibootmgr \
	dosfstools zstd fdisk util-linux libc6 libssl3 > "$work/packages.txt"
