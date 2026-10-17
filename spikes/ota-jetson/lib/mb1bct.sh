#!/usr/bin/env bash
# lib/mb1bct.sh: does a QSPI dump's MB1 BCT carry NVIDIA's QSPI fix (trimmer2-val 0x04)?
# spec: docs/32-fleet-ota.md#jetson-test-image ("Measured": the factory QSPI and trimmer2-val)
#
#   lib/mb1bct.sh refs           build reference MB1 BCTs into out/mb1bct/ (Docker, no network)
#   lib/mb1bct.sh check <file>   look for the QSPI parameters in a QSPI dump (.img or .img.zst) or
#                                any BCT file, and report trimmer2-val per QSPI partition
#   lib/mb1bct.sh selftest       check synthetic dumps built from the references
#
# NVIDIA's critical QSPI update (overlay_mb1bct_36.x, folded into L4T 36.5) sets trimmer2-val of
# the MB1 BCT's qspiflash@0 device from 0x0c to 0x04. L4T 36.4.4 without the overlay keeps 0x0c.
# refs compiles the MB1 BCT's device section three ways with NVIDIA's own tegrabct_v2: 36.4.4,
# 36.4.4 with the overlay, and 36.5.2. In all three the QSPI parameters hold the two clock
# frequencies of the device tree as little-endian words (199999999, 99999999), then trimmer1-val,
# then trimmer2-val; refs refuses if that layout or those values differ. check searches a dump
# for the two frequencies and reads the two words after them. A dump in which they are not found
# in plain form (an encrypted BCT, another layout) is reported as "cannot tell".
#
# Needs out/bsp/{36.4.4,36.5.2}/Linux_for_Tegra and out/bsp/ovl-mb1bct/Linux_for_Tegra, extracted
# from NVIDIA's Jetson_Linux_R36.{4.4,5.2}_aarch64.tbz2 and overlay_mb1bct_36.x.tbz2 (A1).
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
bsp=$here/out/bsp
work=$here/out/mb1bct
image=fjarr-ota-jetson-bct:local

die() {
  echo "mb1bct: $*" >&2
  exit 1
}

refs() {
  local v dts inc tool
  for v in 36.4.4 36.5.2; do
    [[ -x $bsp/$v/Linux_for_Tegra/bootloader/tegrabct_v2 ]] || die "no $bsp/$v/Linux_for_Tegra: extract NVIDIA's L4T $v BSP there first"
  done
  [[ -r $bsp/ovl-mb1bct/Linux_for_Tegra/bootloader/generic/BCT/tegra234-mb1-bct-device-p3767-0000.dts ]] ||
    die "no $bsp/ovl-mb1bct: extract NVIDIA's overlay_mb1bct_36.x.tbz2 there first"
  command -v docker >/dev/null || die "docker is needed for refs"
  # cpp and dtc only; NVIDIA's tegrabct_v2 is a static 32-bit x86 program from the BSP.
  docker build -q -t "$image" - >/dev/null <<'EOF'
FROM ubuntu:26.04
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends cpp device-tree-compiler \
 && rm -rf /var/lib/apt/lists/*
EOF
  rm -rf "$work"
  mkdir -p "$work"
  # variant, the device tree source, the include directory, the BSP whose tegrabct_v2 compiles it
  while read -r v dts inc tool; do
    mkdir -p "$work/$v"
    docker run --rm --network none -u "$(id -u):$(id -g)" \
      -v "$bsp:/bsp:ro" -v "$work/$v:/w" -w /w "$image" bash -c '
        set -euo pipefail
        cpp -nostdinc -I "$(dirname "$1")" -I "$2" -I "$2/generic/BCT" -x assembler-with-cpp \
          -D IN_DTS_CONTEXT "$1" -o device.pp
        dtc -q -I dts -O dtb -o device.dtb device.pp
        "$3" --chip 0x23 0 --mb1bct mb1.cfg --device device.dtb >tegrabct.log 2>&1
        test -s mb1_MB1.bct' refs "/bsp/$dts" "/bsp/$inc" "/bsp/$tool/Linux_for_Tegra/bootloader/tegrabct_v2" ||
      die "building the $v reference failed (see $work/$v)"
  done <<'EOF'
36.4.4 36.4.4/Linux_for_Tegra/bootloader/generic/BCT/tegra234-mb1-bct-device-p3767-0000.dts 36.4.4/Linux_for_Tegra/bootloader 36.4.4
36.4.4+overlay ovl-mb1bct/Linux_for_Tegra/bootloader/generic/BCT/tegra234-mb1-bct-device-p3767-0000.dts 36.4.4/Linux_for_Tegra/bootloader 36.4.4
36.5.2 36.5.2/Linux_for_Tegra/bootloader/generic/BCT/tegra234-mb1-bct-device-p3767-0000.dts 36.5.2/Linux_for_Tegra/bootloader 36.5.2
EOF
  analyse refs "$work/36.4.4/mb1_MB1.bct" "$work/36.4.4+overlay/mb1_MB1.bct" "$work/36.5.2/mb1_MB1.bct" |
    tee "$work/refs.txt"
}

# The analysis itself, in Python: binary search and the GPT at the end of a QSPI dump.
analyse() {
  python3 -I - "$@" <<'PY'
import struct, subprocess, sys, zlib

FREQS = struct.pack("<II", 199999999, 99999999)  # clock-source-frequency, interface-frequency
FIXED, UNFIXED = 0x04, 0x0C                       # trimmer2-val with and without NVIDIA's fix


def load(path):
    if path.endswith(".zst"):
        return subprocess.run(["zstd", "-dcq", path], check=True, stdout=subprocess.PIPE).stdout
    with open(path, "rb") as f:
        return f.read()


def hits(data):
    """(offset, trimmer1, trimmer2) for every place the two QSPI frequencies appear."""
    out, i = [], data.find(FREQS)
    while i >= 0:
        if i + 16 <= len(data):
            t1, t2 = struct.unpack_from("<II", data, i + 8)
            out.append((i, t1, t2))
        i = data.find(FREQS, i + 1)
    return out


def gpt(data, sector=512):
    """Partitions from the GPT header in the last MiB (a QSPI has no primary GPT: the BCT is at 0)."""
    for off in range(len(data) - sector, max(len(data) - (1 << 20), 0) - 1, -sector):
        if data[off:off + 8] != b"EFI PART":
            continue
        hsize = struct.unpack_from("<I", data, off + 12)[0]
        crc_ok = zlib.crc32(data[off:off + 16] + b"\0" * 4 + data[off + 20:off + hsize]) == \
            struct.unpack_from("<I", data, off + 16)[0]
        lba, n, esize = struct.unpack_from("<QII", data, off + 72)
        parts = []
        for k in range(n):
            e = lba * sector + k * esize
            if e + 128 > len(data) or data[e:e + 16] == b"\0" * 16:
                continue
            first, last = struct.unpack_from("<QQ", data, e + 32)
            name = data[e + 56:e + 128].decode("utf-16-le", "replace").rstrip("\0")
            parts.append((first * sector, (last + 1) * sector, name))
        return parts, crc_ok
    return [], None


def part_of(parts, off):
    return next((name for start, end, name in parts if start <= off < end), "?")


def verdict(t2):
    return {FIXED: "the fix (0x04)", UNFIXED: "no fix (0x0c, the 36.4.4 default)"}.get(t2, f"unknown value {t2:#x}")


def check(path):
    data = load(path)
    parts, crc_ok = gpt(data)
    print(f"file: {path.rsplit('/', 1)[-1]}, {len(data)} bytes")
    if crc_ok is None:
        print("GPT: none found at the end: offsets only")
    else:
        print(f"GPT: {len(parts)} partitions, header CRC {'ok' if crc_ok else 'WRONG'}")
    found = hits(data)
    mb1 = []
    for off, t1, t2 in found:
        name = part_of(parts, off)
        print(f"QSPI parameters at {off:#010x} ({name}): trimmer1-val {t1:#x}, trimmer2-val {t2:#x}: {verdict(t2)}")
        if "MB1_BCT" in name or not parts:
            mb1.append(t2)
    if not found:
        print("RESULT: cannot tell: the QSPI parameters are not in the file in plain form")
    elif not mb1:
        print("RESULT: cannot tell: found only outside the MB1 BCT partitions")
    elif all(t == FIXED for t in mb1):
        print(f"RESULT: carries NVIDIA's QSPI fix ({len(mb1)} of {len(mb1)} MB1 BCT copies)")
    elif all(t == UNFIXED for t in mb1):
        print(f"RESULT: does not carry NVIDIA's QSPI fix ({len(mb1)} of {len(mb1)} MB1 BCT copies)")
    else:
        print("RESULT: mixed: the MB1 BCT copies differ (" + ", ".join(f"{t:#x}" for t in mb1) + ")")


def refs(stock, overlay, new):
    expect = {stock: UNFIXED, overlay: FIXED, new: FIXED}
    for path, want in expect.items():
        found = hits(load(path))
        label = path.rsplit("/", 2)[-2]
        if len(found) != 1 or found[0][1] != 0 or found[0][2] != want:
            sys.exit(f"refs: {label}: expected one QSPI block with trimmer1 0 and trimmer2 {want:#x}, found {found}")
        print(f"{label}: QSPI frequencies at {found[0][0]:#x}, trimmer1-val at +8 = 0, trimmer2-val at +12 = {want:#x}")
    print("refs: the layout holds in all three: trimmer2-val is the little-endian word 12 bytes after the frequencies")


def selftest(stock, overlay, new, out):
    """Synthetic 64 MiB QSPI dumps: 0xff, a secondary GPT at the end, BCTs in A_ and B_MB1_BCT."""
    size, sector = 64 << 20, 512
    a, b = 0x00160000, 0x01e70000  # any two offsets; the real ones come from the dump's GPT
    def image(name, bct_a, bct_b, with_gpt=True):
        img = bytearray(b"\xff" * size)
        for off, path in ((a, bct_a), (b, bct_b)):
            if path:
                blob = load(path)
                img[off:off + len(blob)] = blob
        if with_gpt:
            entries = bytearray(128 * 128)
            for k, (off, n) in enumerate(((a, "A_MB1_BCT"), (b, "B_MB1_BCT"))):
                e = k * 128
                entries[e:e + 16] = b"\x11" * 16
                entries[e + 16:e + 32] = bytes([k + 1]) * 16
                struct.pack_into("<QQ", entries, e + 32, off // sector, (off + 0x20000) // sector - 1)
                entries[e + 56:e + 56 + 2 * len(n)] = n.encode("utf-16-le")
            last = size // sector - 1
            elba = last - 32
            img[elba * sector:elba * sector + len(entries)] = entries
            hdr = bytearray(92)
            hdr[0:8] = b"EFI PART"
            struct.pack_into("<IIIIQQQQ", hdr, 8, 0x10000, 92, 0, 0, last, 1, 34, last - 33)
            struct.pack_into("<QIII", hdr, 72, elba, 128, 128, zlib.crc32(entries))
            struct.pack_into("<I", hdr, 16, zlib.crc32(hdr))
            img[last * sector:last * sector + 92] = hdr
        with open(f"{out}/{name}", "wb") as f:
            f.write(img)
    image("factory-like.img", stock, stock)
    image("fixed.img", new, new)
    image("mixed.img", stock, overlay)
    image("none.img", None, None, with_gpt=False)


if __name__ == "__main__":
    cmd, args = sys.argv[1], sys.argv[2:]
    {"check": check, "refs": refs, "selftest": selftest}[cmd](*args)
PY
}

selftest() {
  local t got want rc=0
  [[ -s $work/refs.txt ]] || die "run 'lib/mb1bct.sh refs' first"
  t=$(mktemp -d)
  trap 'rm -rf "$t"' RETURN
  analyse selftest "$work/36.4.4/mb1_MB1.bct" "$work/36.4.4+overlay/mb1_MB1.bct" "$work/36.5.2/mb1_MB1.bct" "$t"
  zstd -q "$t/factory-like.img" -o "$t/factory-like.img.zst"
  while IFS='|' read -r file want; do
    got=$(analyse check "$t/$file" | grep '^RESULT:') || got=
    if [[ $got == "RESULT: $want"* ]]; then echo "PASS $file: $got"; else
      echo "FAIL $file: got '$got', want 'RESULT: $want'"
      rc=1
    fi
  done <<'EOF'
factory-like.img|does not carry NVIDIA's QSPI fix (2 of 2
factory-like.img.zst|does not carry NVIDIA's QSPI fix (2 of 2
fixed.img|carries NVIDIA's QSPI fix (2 of 2
mixed.img|mixed: the MB1 BCT copies differ (0xc, 0x4)
none.img|cannot tell
EOF
  return "$rc"
}

case ${1:-} in
  refs) refs ;;
  check)
    [[ -r ${2:-} ]] || die "usage: lib/mb1bct.sh check <QSPI dump or BCT file>"
    analyse check "$2"
    ;;
  selftest) selftest ;;
  *)
    sed -n '4,9p' "$0" >&2
    exit 2
    ;;
esac
