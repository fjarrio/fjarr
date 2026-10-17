#!/bin/bash
# Build the Jetson network rescue: one aarch64 EFI file, a unified kernel image (UKI), that the ZED
# Box Mini's UEFI loads by PXE or HTTP boot from the mini-PC (net-rescue.sh).
#
#   build.sh [--l4t 36.4.4|36.5.2] [--test]
#
# Output in out/rescue/<l4t>/: fjarr-net-rescue.efi, its manifest, and known_hosts, a line for
# HostKeyAlias=fjarr-net-rescue. --test adds test/fjarr-net-rescue-test.efi for the QEMU bench
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
while [ $# -gt 0 ]; do
	case $1 in
	--l4t) l4t=${2:?--l4t needs a version}; shift 2 ;;
	--test) test_variant=1; shift ;;
	*) sed -n '5p' "$0" >&2; exit 2 ;;
	esac
done

bsp_dir=$jet/out/bsp
bsp=$bsp_dir/$l4t/Linux_for_Tegra
out=$jet/out/rescue/$l4t
work=$jet/out/rescue/work/$l4t
lab=$jet/.lab/rescue
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
if [ ! -f "$lab/ssh_host_ed25519_key" ]; then
	mkdir -p "$lab"
	chmod 700 "$lab"
	ssh-keygen -q -t ed25519 -N '' -C fjarr-net-rescue -f "$lab/ssh_host_ed25519_key"
fi
mkdir -p "$work" "$out"
if [ -n "${FJARR_LAB_PUBKEYS:-}" ]; then
	cat "$FJARR_LAB_PUBKEYS" > "$work/authorized_keys"
else
	cat ~/.ssh/id_*.pub > "$work/authorized_keys"
fi
ssh-keygen -l -f "$work/authorized_keys" > /dev/null # every line must be a public key
printf 'fjarr-net-rescue %s\n' "$(cut -d' ' -f1,2 "$lab/ssh_host_ed25519_key.pub")" > "$out/known_hosts"

say "container images"
docker build -q --platform linux/arm64 -f "$here/Dockerfile.userland" -t "$userland_image" "$here" > /dev/null
docker build -q --platform linux/amd64 -f "$here/Dockerfile.tools" -t "$tools_image" "$here" > /dev/null

say "userland from Ubuntu 22.04 arm64 (qemu-user)"
rm -rf "$work/root"
docker run --rm --platform linux/arm64 --user "$user" -v "$here:/src:ro" -v "$work:/work" \
	"$userland_image" bash /src/collect-userland.sh /work /src
cp -a "$here/rootfs/." "$work/root/"
mkdir -p "$work/root"/{dev,proc,sys,run,tmp,mnt,var/log,root/.ssh}
install -m 0600 "$lab/ssh_host_ed25519_key" "$work/root/etc/ssh/ssh_host_ed25519_key"
install -m 0644 "$lab/ssh_host_ed25519_key.pub" "$work/root/etc/ssh/ssh_host_ed25519_key.pub"
install -m 0600 "$work/authorized_keys" "$work/root/root/.ssh/authorized_keys"

say "kernel modules"
kmod=$work/kmod
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
NAME="Fjarr network rescue"
ID=fjarr-net-rescue
PRETTY_NAME="Fjarr network rescue (L4T $l4t, kernel $krel)"
VERSION_ID=$l4t
IMAGE_ID=fjarr-net-rescue
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
