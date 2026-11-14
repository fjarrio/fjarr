#!/bin/bash
# mkswu.sh: the signed SWUpdate package of one Jetson slot image, for the applier (D3).
# spec: docs/32-fleet-ota.md#jetson-test-image; applier/README.md, "The SWUpdate package"
#
#   mkswu.sh <version> [<the version's .tegraflash.tar.gz>]
#
# The package carries the slot's three parts, taken from the image's own flash package so they
# are what a flash writes: the ext4 root (labelled fjarr_<version>, zstd), the Android boot image
# for the chain's kernel partition and the kernel device tree for its -dtb partition. Two
# selections, fjarr,slot-a and fjarr,slot-b, each write only that slot's three partitions, by
# partition label, never the ESP. sw-description is signed with the lab's key (.lab/swu.key; the
# image trusts .lab/swu.crt), as on x86. Out: out/swu/<version>/fjarr_<version>.swu and
# fjarr_<version>.fw (the L4T version of the firmware the release runs on), the applier's release
# directory.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
v=${1:?usage: mkswu.sh <version> [<tegraflash package>]}
deploy=$here/out/yocto/build/tmp/deploy/images/zedbox-mini-orin-nx16
pkg=${2:-$deploy/fjarr-spike-image-zedbox-mini-orin-nx16.rootfs.tegraflash.tar.gz}
fw=36.5.2
[[ $v =~ ^[0-9]+(\.[0-9]+)*$ ]] || { echo "mkswu.sh: not a version: $v" >&2; exit 2; }
[[ -s $pkg ]] || { echo "mkswu.sh: no flash package $pkg" >&2; exit 2; }
[[ -s $here/.lab/swu.key && -s $here/.lab/swu.crt ]] || { echo "mkswu.sh: no .lab/swu.key (yocto/build.sh makes it)" >&2; exit 2; }

out=$here/out/swu/$v
w=$out/work
rm -rf "$w"
mkdir -p "$w"
pkg=$(readlink -f "$pkg")
dtb=$(tar -tzf "$pkg" | sed -n 's|^\./||; /^tegra234-.*-zedbox-mini\.dtb$/p' | head -n 1)
[[ -n $dtb ]] || { echo "mkswu.sh: no kernel device tree in $pkg" >&2; exit 1; }
tar -C "$w" --sparse -xzf "$pkg" fjarr-spike-image.ext4 boot.img "$dtb"

# The root must be the version it claims: the applier checks the written slot's label.
label=$(/sbin/dumpe2fs -h "$w/fjarr-spike-image.ext4" 2>/dev/null | sed -n 's/^Filesystem volume name: *//p')
[[ $label == "fjarr_$v" ]] || { echo "mkswu.sh: the package's root is labelled '$label', not fjarr_$v (build.sh --image-version $v)" >&2; exit 1; }
[[ $(head -c 8 "$w/boot.img") == "ANDROID!" ]] || { echo "mkswu.sh: boot.img is not an Android boot image" >&2; exit 1; }

zstd -q -T2 -3 -f "$w/fjarr-spike-image.ext4" -o "$w/root.ext4.zst"
mv "$w/boot.img" "$w/kernel.img"
mv "$w/$dtb" "$w/kernel.dtb"
rs=$(sha256sum <"$w/root.ext4.zst" | cut -d' ' -f1)
ks=$(sha256sum <"$w/kernel.img" | cut -d' ' -f1)
ds=$(sha256sum <"$w/kernel.dtb" | cut -d' ' -f1)
slot() { # <selection> <root> <kernel> <kernel-dtb>: the partition labels it writes
  cat <<EOF
    $1: {
      images: (
        { filename = "root.ext4.zst"; type = "raw"; compressed = "zstd"; installed-directly = true;
          device = "/dev/disk/by-partlabel/$2"; sha256 = "$rs"; },
        { filename = "kernel.img"; type = "raw"; device = "/dev/disk/by-partlabel/$3"; sha256 = "$ks"; },
        { filename = "kernel.dtb"; type = "raw"; device = "/dev/disk/by-partlabel/$4"; sha256 = "$ds"; }
      );
    };
EOF
}
{
  printf 'software =\n{\n  version = "%s";\n  hardware-compatibility: [ "1.0" ];\n  fjarr: {\n' "$v"
  slot slot-a APP A_kernel A_kernel-dtb
  slot slot-b APP_b B_kernel B_kernel-dtb
  printf '  };\n}\n'
} >"$w/sw-description"
openssl cms -sign -in "$w/sw-description" -out "$w/sw-description.sig" -signer "$here/.lab/swu.crt" \
  -inkey "$here/.lab/swu.key" -outform DER -noattr -nosmimecap -binary
(cd "$w" && printf '%s\n' sw-description sw-description.sig root.ext4.zst kernel.img kernel.dtb |
  cpio -o -H crc --reproducible --owner 0:0 --quiet) >"$out/fjarr_$v.swu"
printf '%s\n' "$fw" >"$out/fjarr_$v.fw"
printf '%s  %s\n' "$(sha256sum <"$out/fjarr_$v.swu" | cut -d' ' -f1)" "fjarr_$v.swu" >"$out/SHA256SUMS"
rm -rf "$w"
ls -la "$out"
