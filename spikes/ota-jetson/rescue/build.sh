#!/bin/bash
# Build the Jetson network rescue: one aarch64 EFI file, a unified kernel image (UKI), that the ZED
# Box Mini's UEFI loads by PXE or HTTP boot from the mini-PC (net-rescue.sh). With --recovery, the
# same system as the recovery-partition rescue instead: an Android boot image for `recovery` and
# the device tree for `recovery-dtb` (phase A5, for B6's T6).
#
#   build.sh [--l4t 36.4.4|36.5.2] [--test | --recovery]
#
# Output in out/rescue/<l4t>/: fjarr-net-rescue.efi, its manifest, and known_hosts, a line for
# HostKeyAlias=fjarr-net-rescue. --recovery writes fjarr-recovery-rescue.img, .dtb, its manifest
# and known_hosts-recovery (HostKeyAlias=fjarr-recovery-rescue): the same tree under that name, with
# a host key of its own (.lab/rescue/recovery/), so the lab scripts tell the two apart; the device
# tree is the box's own recovery-dtb from the backup (B3), unchanged. --test adds test/fjarr-net-rescue-test.efi for the QEMU bench
# (test/netboot-test.sh): the same system with a throwaway key added, the dead-man at 180 s and a
# PL011 console. The kernel and its modules come from NVIDIA's BSP tarball in out/bsp (phase A1),
# the userland from Ubuntu 22.04 arm64 packages, root's keys from ~/.ssh/id_*.pub (or the file
# FJARR_LAB_PUBKEYS), the SSH host key from .lab/rescue/, created on the first run.
# spec: docs/32-fleet-ota.md#jetson-test-image ("Three rescue layers", item 2)
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
jet=$(dirname "$here")
l4t=36.4.4
test_variant=0
recovery=0
while [ $# -gt 0 ]; do
	case $1 in
	--l4t) l4t=${2:?--l4t needs a version}; shift 2 ;;
	--test) test_variant=1; shift ;;
	--recovery) recovery=1; shift ;;
	*) sed -n '8p' "$0" >&2; exit 2 ;;
	esac
done
[ "$test_variant$recovery" != 11 ] || { echo "build.sh: --test and --recovery are separate builds" >&2; exit 2; }

bsp_dir=$jet/out/bsp
bsp=$bsp_dir/$l4t/Linux_for_Tegra
out=$jet/out/rescue/$l4t
work=$jet/out/rescue/work/$l4t
kmod=$work/kmod # the BSP's modules, shared by both variants
lab=$jet/.lab/rescue
name=fjarr-net-rescue
keydir=$lab
if [ "$recovery" = 1 ]; then
	name=fjarr-recovery-rescue
	work=$work-recovery
	keydir=$lab/recovery
	# The device tree: the box's own recovery-dtb, as B3 backed it up (it matches no BSP tree).
	dtb_backup=$jet/.lab/zedbox-backup/nvme0n1p9-recovery-dtb.img.zst
	[ -f "$dtb_backup" ] || { echo "build.sh: no $dtb_backup (phase B3)" >&2; exit 1; }
	(cd "$(dirname "$dtb_backup")" && command grep -E "  ${dtb_backup##*/}\$" SHA256SUMS | sha256sum -c --quiet -) || {
		echo "build.sh: $dtb_backup does not match the backup's SHA256SUMS" >&2
		exit 1
	}
fi
userland_image=fjarr-jetson-rescue-userland:22.04
tools_image=fjarr-jetson-rescue-tools:26.04
user="$(id -u):$(id -g)"
say() { printf '\n== %s\n' "$*"; }

say "BSP $l4t: the kernel, its modules and NVIDIA's kernel arguments"
members=()
for f in kernel/Image kernel/kernel_supplements.tbz2 kernel/kernel_oot_modules.tbz2 p3767.conf.common; do
	[ -f "$bsp/$f" ] || members+=("Linux_for_Tegra/$f")
done
if [ ${#members[@]} -gt 0 ]; then
	tarball=Jetson_Linux_R${l4t}_aarch64.tbz2
	(cd "$bsp_dir" && command grep -E "  $tarball\$" SHA256SUMS | sha256sum -c --quiet -) || {
		echo "build.sh: $bsp_dir/$tarball is missing or does not match SHA256SUMS (phase A1)" >&2
		exit 1
	}
	mkdir -p "$bsp_dir/$l4t"
	tar -x -C "$bsp_dir/$l4t" -I bzip2 -f "$bsp_dir/$tarball" "${members[@]}"
fi
nvidia_args=$(sed -n 's/^CMDLINE_ADD="\(.*\)"$/\1/p' "$bsp/p3767.conf.common" | tail -n 1)
[ -n "$nvidia_args" ] || { echo "build.sh: no CMDLINE_ADD in $bsp/p3767.conf.common" >&2; exit 1; }

say "keys"
if [ ! -f "$keydir/ssh_host_ed25519_key" ]; then
	mkdir -p "$keydir"
	chmod 700 "$lab" "$keydir"
	ssh-keygen -q -t ed25519 -N '' -C "$name" -f "$keydir/ssh_host_ed25519_key"
fi
mkdir -p "$work" "$out"
if [ -n "${FJARR_LAB_PUBKEYS:-}" ]; then
	cat "$FJARR_LAB_PUBKEYS" > "$work/authorized_keys"
else
	cat ~/.ssh/id_*.pub > "$work/authorized_keys"
fi
ssh-keygen -l -f "$work/authorized_keys" > /dev/null # every line must be a public key
if [ "$recovery" = 1 ]; then known=$out/known_hosts-recovery; else known=$out/known_hosts; fi
printf '%s %s\n' "$name" "$(cut -d' ' -f1,2 "$keydir/ssh_host_ed25519_key.pub")" > "$known"

say "container images"
docker build -q --platform linux/arm64 -f "$here/Dockerfile.userland" -t "$userland_image" "$here" > /dev/null
docker build -q --platform linux/amd64 -f "$here/Dockerfile.tools" -t "$tools_image" "$here" > /dev/null

say "userland from Ubuntu 22.04 arm64 (qemu-user)"
rm -rf "$work/root"
docker run --rm --platform linux/arm64 --user "$user" -v "$here:/src:ro" -v "$work:/work" \
	"$userland_image" bash /src/collect-userland.sh /work /src
cp -a "$here/rootfs/." "$work/root/"
if [ "$recovery" = 1 ]; then
	# The same files under the recovery rescue's name: its hostname, prompt, DHCP name and log tags.
	(cd "$here/rootfs" && find . -type f -print0) | (cd "$work/root" && xargs -0 sed -i "s/fjarr-net-rescue/$name/g")
fi
mkdir -p "$work/root"/{dev,proc,sys,run,tmp,mnt,var/log,root/.ssh}
install -m 0600 "$keydir/ssh_host_ed25519_key" "$work/root/etc/ssh/ssh_host_ed25519_key"
install -m 0644 "$keydir/ssh_host_ed25519_key.pub" "$work/root/etc/ssh/ssh_host_ed25519_key.pub"
install -m 0600 "$work/authorized_keys" "$work/root/root/.ssh/authorized_keys"

say "kernel modules"
if [ ! -f "$kmod/.complete" ]; then
	rm -rf "$kmod"
	mkdir -p "$kmod"
	tar -xjf "$bsp/kernel/kernel_supplements.tbz2" -C "$kmod"
	tar -xjf "$bsp/kernel/kernel_oot_modules.tbz2" -C "$kmod"
	krel=$(basename "$(find "$kmod/lib/modules" -mindepth 1 -maxdepth 1 -type d)")
	mv "$kmod/usr/lib/modules/$krel/updates" "$kmod/lib/modules/$krel/updates"
	docker run --rm --user "$user" -v "$kmod:/k" "$tools_image" depmod -b /k "$krel"
	touch "$kmod/.complete"
fi
krel=$(basename "$(find "$kmod/lib/modules" -mindepth 1 -maxdepth 1 -type d)")
# The modules NVIDIA's stock recovery image loads (README), with what they depend on.
mods=(phy-tegra194-p2u pcie-tegra194 nvme tegra-bpmp-thermal pwm-tegra pwm-fan r8168)
mdir=$work/root/usr/lib/modules/$krel
mkdir -p "$mdir"
for m in "${mods[@]}"; do
	line=$(command grep -m 1 -E "(^|/)$m\.ko:" "$kmod/lib/modules/$krel/modules.dep") || {
		echo "build.sh: module $m is not in the $l4t BSP" >&2
		exit 1
	}
	read -ra files <<< "${line/:/}"
	for f in "${files[@]}"; do
		install -D -m 0644 "$kmod/lib/modules/$krel/$f" "$mdir/$f"
	done
done
cp "$kmod/lib/modules/$krel"/modules.{builtin,builtin.modinfo,order} "$mdir/"
docker run --rm --user "$user" -v "$work/root:/r" "$tools_image" depmod -b /r "$krel"

say "os-release and command line"
cat > "$work/root/etc/os-release" << EOF
NAME="Fjarr $([ "$recovery" = 1 ] && echo recovery-partition || echo network) rescue"
ID=$name
PRETTY_NAME="Fjarr $([ "$recovery" = 1 ] && echo recovery-partition || echo network) rescue (L4T $l4t, kernel $krel)"
VERSION_ID=$l4t
IMAGE_ID=$name
IMAGE_VERSION=$(date -u +%Y%m%d%H%M)
EOF
# NVIDIA's own arguments for the board (what L4TLauncher passes), then the rescue's.
cmdline="$nvidia_args panic=10"
case " $cmdline " in *" efi=runtime "*) ;; *) cmdline="$cmdline efi=runtime" ;; esac
echo "$cmdline" > "$work/cmdline"
# The test UKI runs only in QEMU: earlycon finds QEMU's UART through its device tree.
echo "$cmdline earlycon console=ttyAMA0 fjarr.deadman=180" > "$work/cmdline.test"

say "checking every binary loads in the tree (chroot, qemu-user)"
docker run --rm -i --platform linux/arm64 -v "$work/root:/stage-ro:ro" "$userland_image" sh -euc '
	cp -a /stage-ro /stage
	mknod -m 666 /stage/dev/null c 1 3
	mkdir -p /stage/run/sshd
	exec chroot /stage /bin/sh -eu' << 'EOF'
for f in $(find /usr/bin /usr/sbin /usr/lib/openssh -type f); do
	/lib/ld-linux-aarch64.so.1 --verify "$f" > /dev/null 2>&1 || continue # static, or no ELF
	if /lib/ld-linux-aarch64.so.1 --list "$f" 2>&1 | grep -q 'not found'; then
		echo "missing libraries: $f"
		exit 1
	fi
done
grep -v '^#' /etc/inittab | cut -d: -f4 | cut -d' ' -f1 | while read -r p; do
	[ -x "$p" ] || { echo "inittab: no $p"; exit 1; }
done
sshd -t -f /etc/ssh/sshd_config
# The rescue's shell must find the real tools, not BusyBox applets of the same name.
mke2fs -V 2>&1 | grep '^mke2fs 1\.'
fdisk --version | grep 'util-linux'
blkdiscard --version | grep 'util-linux'
sgdisk --version | head -n 1
nvme version
efibootmgr --version
zstd -V
lsblk --version
busybox | head -n 1
EOF

if [ "$recovery" = 1 ]; then
	say "initramfs, the Android boot image and the device tree"
	SOURCE_DATE_EPOCH=$(git -C "$here" log -1 --format=%ct 2> /dev/null || echo 0)
	docker run --rm --user "$user" -e SOURCE_DATE_EPOCH="$SOURCE_DATE_EPOCH" -v "$here:/src:ro" -v "$work:/work" \
		"$tools_image" bash -euo pipefail -c '
		python3 -I /src/mkcpio.py /work/root /work/initramfs.cpio --mode root=700 --mode root/.ssh=700 \
			--mode root/.ssh/authorized_keys=600 --mode etc/ssh/ssh_host_ed25519_key=600 --mode etc/shadow=600 \
			--mode tmp=1777 --dev dev/console=c:5:1:600 --dev dev/null=c:1:3:666
		xz -9 --check=crc32 --threads=1 -c /work/initramfs.cpio > /work/initramfs.cpio.xz'
	# The partitions' sizes in NVIDIA's NVMe layout (docs/32): recovery 80 MiB, recovery-dtb 512 KiB.
	python3 -I "$here/mkbootimg.py" --kernel "$bsp/kernel/Image" --ramdisk "$work/initramfs.cpio.xz" \
		--cmdline-file "$work/cmdline" --out "$out/$name.img" --max $((80 * 1024 * 1024))
	zstd -dqc "$dtb_backup" | python3 -I -c '
import struct, sys
d = sys.stdin.buffer.read()
magic, total = struct.unpack(">II", d[:8])
if magic != 0xD00DFEED or total > len(d) or total > 512 * 1024:
    sys.exit("build.sh: the backed-up recovery-dtb is not a device tree that fits")
sys.stdout.buffer.write(d[:total])' > "$out/$name.dtb"
	{
		echo "Fjarr recovery-partition rescue, L4T $l4t, built $(date -u +%Y-%m-%dT%H:%MZ)"
		echo "kernel $krel: Image sha256 $(sha256sum < "$bsp/kernel/Image" | cut -d' ' -f1), from the BSP tarball in out/bsp"
		echo "modules: $(cd "$mdir" && find . -name '*.ko' | sed 's#.*/##' | sort | tr '\n' ' ')"
		echo "command line: $cmdline"
		echo "userland image: $(docker image inspect --format '{{.Id}}' "$userland_image")"
		echo "packages:"
		sed 's/^/  /' "$work/packages.txt"
		echo "initramfs: $(stat -c %s "$work/initramfs.cpio") bytes, $(stat -c %s "$work/initramfs.cpio.xz") with xz"
		echo "boot image (Android v0, page 2048): $(stat -c %s "$out/$name.img") bytes, sha256 $(sha256sum < "$out/$name.img" | cut -d' ' -f1)"
		echo "device tree: the box's recovery-dtb from the backup, $(stat -c %s "$out/$name.dtb") bytes, sha256 $(sha256sum < "$out/$name.dtb" | cut -d' ' -f1)"
		echo "host key: $(ssh-keygen -l -f "$keydir/ssh_host_ed25519_key.pub")"
		echo "root's authorized keys:"
		ssh-keygen -l -f "$work/authorized_keys" | sed 's/^/  /'
	} > "$out/$name.manifest"
	(cd "$out" && sha256sum "$name.img" "$name.dtb" > SHA256SUMS-recovery)
	sed -n '1,4p;/^initramfs/,/^host key/p' "$out/$name.manifest"
	echo "out: $out/$name.img, $out/$name.dtb"
	exit 0
fi

say "initramfs and UKI"
if [ "$test_variant" = 1 ]; then
	mkdir -p "$out/test"
	[ -f "$out/test/id_ed25519" ] || ssh-keygen -q -t ed25519 -N '' -C fjarr-net-rescue-test -f "$out/test/id_ed25519"
	cat "$work/authorized_keys" "$out/test/id_ed25519.pub" > "$work/authorized_keys.test"
fi
SOURCE_DATE_EPOCH=$(git -C "$here" log -1 --format=%ct 2> /dev/null || echo 0)
docker run --rm --user "$user" -e SOURCE_DATE_EPOCH="$SOURCE_DATE_EPOCH" -e KREL="$krel" \
	-e TEST="$test_variant" -v "$here:/src:ro" -v "$work:/work" -v "$out:/out" \
	-v "$bsp/kernel/Image:/Image:ro" "$tools_image" bash -euo pipefail -c '
	cpio_args=(--mode root=700 --mode root/.ssh=700 --mode root/.ssh/authorized_keys=600
		--mode etc/ssh/ssh_host_ed25519_key=600 --mode etc/shadow=600 --mode tmp=1777
		--dev dev/console=c:5:1:600 --dev dev/null=c:1:3:666)
	uki() { # <initramfs> <cmdline file> <output>
		ukify build --linux /Image --initrd "$1" --cmdline "@$2" --os-release @/work/root/etc/os-release \
			--uname "$KREL" --stub /opt/stub/linuxaa64.efi.stub --efi-arch aa64 --output "$3" > /dev/null
	}
	python3 -I /src/mkcpio.py /work/root /work/initramfs.cpio "${cpio_args[@]}"
	xz -9 --check=crc32 --threads=1 -c /work/initramfs.cpio > /work/initramfs.cpio.xz
	uki /work/initramfs.cpio.xz /work/cmdline /out/fjarr-net-rescue.efi
	ukify inspect /out/fjarr-net-rescue.efi > /work/inspect.txt
	if [ "$TEST" = 1 ]; then
		python3 -I /src/mkcpio.py /work/root /work/initramfs-test.cpio "${cpio_args[@]}" \
			--file root/.ssh/authorized_keys=/work/authorized_keys.test
		xz -9 --check=crc32 --threads=1 -c /work/initramfs-test.cpio > /work/initramfs-test.cpio.xz
		uki /work/initramfs-test.cpio.xz /work/cmdline.test /out/test/fjarr-net-rescue-test.efi
	fi
	echo "ukify $(ukify --version | cut -d" " -f2-), stub $(cat /opt/stub/VERSION)" > /work/tools.txt'

say "manifest"
{
	echo "Fjarr network rescue, L4T $l4t, built $(date -u +%Y-%m-%dT%H:%MZ)"
	echo "kernel $krel: Image sha256 $(sha256sum < "$bsp/kernel/Image" | cut -d' ' -f1), from the BSP tarball in out/bsp"
	echo "modules: $(cd "$mdir" && find . -name '*.ko' | sed 's#.*/##' | sort | tr '\n' ' ')"
	echo "command line: $cmdline"
	echo "tools: $(cat "$work/tools.txt")"
	echo "userland image: $(docker image inspect --format '{{.Id}}' "$userland_image")"
	echo "packages:"
	sed 's/^/  /' "$work/packages.txt"
	echo "initramfs: $(stat -c %s "$work/initramfs.cpio") bytes, $(stat -c %s "$work/initramfs.cpio.xz") with xz"
	echo "UKI: $(stat -c %s "$out/fjarr-net-rescue.efi") bytes, sha256 $(sha256sum < "$out/fjarr-net-rescue.efi" | cut -d' ' -f1)"
	echo "host key: $(ssh-keygen -l -f "$lab/ssh_host_ed25519_key.pub")"
	echo "root's authorized keys:"
	ssh-keygen -l -f "$work/authorized_keys" | sed 's/^/  /'
	echo "largest files in the initramfs:"
	(cd "$work/root" && find . -type f -printf '%s %p\n' | sort -rn | head -n 12 | sed 's/^/  /')
	echo "sections:"
	sed 's/^/  /' "$work/inspect.txt"
} > "$out/fjarr-net-rescue.manifest"
(cd "$out" && sha256sum fjarr-net-rescue.efi > SHA256SUMS)
sed -n '1,4p;/^initramfs/,/^host key/p' "$out/fjarr-net-rescue.manifest"
echo "out: $out/fjarr-net-rescue.efi"
