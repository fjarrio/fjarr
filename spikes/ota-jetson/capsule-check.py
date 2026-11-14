#!/usr/bin/env python3
"""capsule-check.py: is a bootloader capsule the firmware a flash wrote (rule L4)?

spec: docs/32-fleet-ota.md#jetson-test-image (rule L4; phase E); README.md, "E"

    capsule-check.py <capsule> <flashed dir>

<flashed dir> is what d1-flash.sh keeps of a flash (.lab/d1/flashed-<package sha>/): the signed
partition images in signed/, the QSPI's two secondary GPTs and the board's version file. The
capsule's NVIDIA blob (V3) holds one image per QSPI partition. Each must be byte-identical to a
signed image the flash wrote, except three that a flash makes for the board in hand:

  secondary_gpt, secondary_gpt_backup  the same table; only the random GUIDs (each partition's
                                       and the disk's) and the two CRCs over them may differ
  VER                                  the same release; only the board's FAB, the build time
                                       and the line with the size and CRC may differ

Exit 0 when every image passes; 1 with the failing images; 2 on bad arguments.
"""
import hashlib
import os
import struct
import sys

MAGIC = b"NVIDIA__BLOB__V3"
HEADER = struct.Struct("<16s6I")  # magic, version, blob size, header size, images, type, size
ENTRY = 184  # name[40], offset, length, then 136 bytes this check does not read
GPT_HEADER = 16384  # a secondary GPT: 128 entries of 128 bytes, then the header
# Header fields a fresh table rewrites: its CRC, the disk GUID, the entry array's CRC.
GPT_HEADER_FREE = set(range(16, 20)) | set(range(56, 72)) | set(range(88, 92))
GPT_ENTRY_FREE = set(range(16, 32))  # each entry's unique GUID
GPT = {"secondary_gpt": "gpt_secondary_3_0.bin", "secondary_gpt_backup": "gpt_backup_secondary_3_0.bin"}
VER_FILE_SUFFIX = "_bootblob_ver.txt"


def blob_images(data):
    """The capsule's images as (name, bytes), from the NVIDIA blob inside it."""
    at = data.find(MAGIC)
    if at < 0:
        raise ValueError("no NVIDIA blob (V3) in the capsule")
    _, _, size, header, count, _, _ = HEADER.unpack_from(data, at)
    blob = data[at:at + size]
    if len(blob) != size:
        raise ValueError(f"the blob says {size} bytes, the capsule holds {len(blob)}")
    images = []
    for i in range(count):
        e = header + i * ENTRY
        name = blob[e:e + 40].split(b"\0")[0].decode()
        off, length = struct.unpack_from("<2I", blob, e + 40)
        if off + length > size:
            raise ValueError(f"image {name} runs past the blob")
        images.append((name, blob[off:off + length]))
    return images


def gpt_differs_only_in_guids(a, b):
    if len(a) != len(b) or len(a) <= GPT_HEADER or a[GPT_HEADER:GPT_HEADER + 8] != b"EFI PART":
        return False
    for i in range(len(a)):
        if a[i] == b[i]:
            continue
        if i < GPT_HEADER and i % 128 in GPT_ENTRY_FREE:
            continue
        if i >= GPT_HEADER and i - GPT_HEADER in GPT_HEADER_FREE:
            continue
        return False
    return True


def ver_lines(text):
    """The version file without what a flash or a build fills in for itself."""
    out = []
    for line in text.decode(errors="replace").splitlines():
        if line.startswith("BOARDID="):
            line = " ".join(f for f in line.split() if not f.startswith("FAB="))
        elif line.isdigit() and len(line) == 14:  # the build time, YYYYMMDDhhmmss
            line = "<time>"
        elif line.startswith("BYTES:"):
            line = "<size and crc>"
        out.append(line)
    return out


def check(capsule, flashed):
    images = blob_images(open(capsule, "rb").read())
    signed = {}
    sdir = os.path.join(flashed, "signed")
    for f in sorted(os.listdir(sdir)):
        p = os.path.join(sdir, f)
        if os.path.isfile(p):
            signed.setdefault(hashlib.sha256(open(p, "rb").read()).hexdigest(), f)
    vers = [f for f in os.listdir(flashed) if f.endswith(VER_FILE_SUFFIX)]
    bad = []
    rows = []
    for name, data in images:
        digest = hashlib.sha256(data).hexdigest()
        if digest in signed:
            rows.append((name, "same as " + signed[digest]))
        elif name in GPT:
            ref = open(os.path.join(flashed, GPT[name]), "rb").read()
            ok = gpt_differs_only_in_guids(data, ref)
            rows.append((name, "the flash's table, other GUIDs" if ok else "DIFFERS from " + GPT[name]))
            ok or bad.append(name)
        elif name == "VER" and len(vers) == 1:
            ref = open(os.path.join(flashed, vers[0]), "rb").read()
            ok = ver_lines(data) == ver_lines(ref)
            rows.append((name, "the flash's release " + ver_lines(data)[1].lstrip("# ") if ok else "DIFFERS from " + vers[0]))
            ok or bad.append(name)
        else:
            rows.append((name, "NOT what the flash wrote"))
            bad.append(name)
    return rows, bad


def main(argv):
    if len(argv) != 3 or not os.path.isdir(os.path.join(argv[2], "signed")):
        sys.stderr.write(__doc__.split("\n\n")[1] + "\n")
        return 2
    try:
        rows, bad = check(argv[1], argv[2])
    except (OSError, ValueError) as e:
        print(f"capsule-check: {e}", file=sys.stderr)
        return 1
    for name, verdict in rows:
        print(f"  {name:24} {verdict}")
    if bad:
        print(f"capsule-check: {len(bad)} of {len(rows)} images are not what the flash wrote: {' '.join(bad)}")
        return 1
    print(f"capsule-check: all {len(rows)} images are what the flash wrote")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
