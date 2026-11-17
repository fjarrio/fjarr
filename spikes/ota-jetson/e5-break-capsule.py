#!/usr/bin/env python3
"""E5's broken capsule: one byte flipped in the middle of one image of a good capsule.

spec: docs/32-fleet-ota.md#jetson-test-image (phase E, E5; the one exception to rule L4, with Q4)

    python3 -I e5-break-capsule.py <good capsule> <out> [<image name, default cpu-bootloader>]

The capsule's FMP headers and NVIDIA's blob (V3) carry no checksum over the images, so UEFI writes
the image as it is; the stage that loads it is expected to refuse it, and the chain to fall back.
Every other byte stays the good capsule's: the tool checks that only the named image differs
(capsule-check.py's parser). It never writes in place.
"""
import hashlib
import importlib.util
import os
import sys

here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("cc", os.path.join(here, "capsule-check.py"))
cc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cc)


def image_offset(data, name):
    """(offset in the capsule, length) of the named image."""
    at = data.find(cc.MAGIC)
    if at < 0:
        raise ValueError("no NVIDIA blob (V3) in the capsule")
    _, _, _, header, count, _, _ = cc.HEADER.unpack_from(data, at)
    for i in range(count):
        e = at + header + i * cc.ENTRY
        if data[e:e + 40].split(b"\0")[0].decode() == name:
            off, length = cc.struct.unpack_from("<2I", data, e + 40)
            return at + off, length
    raise ValueError(f"no image {name} in the capsule")


def main(argv):
    if len(argv) not in (3, 4):
        print(__doc__.strip().splitlines()[0], file=sys.stderr)
        return 2
    src, out = argv[1], argv[2]
    name = argv[3] if len(argv) == 4 else "cpu-bootloader"
    if os.path.exists(out):
        print(f"e5-break-capsule: {out} exists; not overwritten", file=sys.stderr)
        return 2
    good = open(src, "rb").read()
    off, length = image_offset(good, name)
    at = off + length // 2
    bad = bytearray(good)
    bad[at] ^= 0xFF
    bad = bytes(bad)
    before = dict(cc.blob_images(good))
    after = dict(cc.blob_images(bad))
    differ = sorted(n for n in before if before[n] != after.get(n))
    if differ != [name] or sorted(before) != sorted(after):
        print(f"e5-break-capsule: images that differ: {differ}, not [{name}]", file=sys.stderr)
        return 1
    with open(out + ".part", "wb") as f:
        f.write(bad)
    os.rename(out + ".part", out)
    print(f"{name}: {length} bytes at capsule offset {off}; byte {at} (image offset {length // 2}) "
          f"0x{good[at]:02x} -> 0x{bad[at]:02x}")
    print(f"good {hashlib.sha256(good).hexdigest()}  {src}")
    print(f"bad  {hashlib.sha256(bad).hexdigest()}  {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
