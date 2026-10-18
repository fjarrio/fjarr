#!/usr/bin/env python3
"""Write an Android boot image, header version 0, for the ZED Box Mini's `recovery` partition.

    mkbootimg.py --kernel Image --ramdisk initramfs --cmdline-file FILE --out IMG [--max BYTES]

L4TLauncher boots `recovery` as such an image (bootmode=recovery, or both root slots unbootable).
The load addresses, page size and header version are those of NVIDIA's stock recovery image on the
lab box (36.4.4, read from its backup); L4TLauncher places the kernel and ramdisk itself. The id
is AOSP's: SHA-1 over each part and its size.
spec: docs/32-fleet-ota.md#jetson-test-image ("Three rescue layers", item 1)
"""
import argparse
import hashlib
import struct
import sys

PAGE = 2048
KERNEL_ADDR, RAMDISK_ADDR, SECOND_ADDR, TAGS_ADDR = 0x10008000, 0x11000000, 0x10F00000, 0x10000100


def pad(b):
    return b + b"\0" * (-len(b) % PAGE)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--kernel", required=True)
    p.add_argument("--ramdisk", required=True)
    p.add_argument("--cmdline-file", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--max", type=int, default=0, help="refuse an image larger than this")
    a = p.parse_args()
    kernel = open(a.kernel, "rb").read()
    ramdisk = open(a.ramdisk, "rb").read()
    cmdline = open(a.cmdline_file, "rb").read().strip()
    if len(cmdline) > 511:
        sys.exit("mkbootimg.py: the command line is %d bytes; header v0 holds 511" % len(cmdline))
    sha = hashlib.sha1()
    for part in (kernel, ramdisk, b""):
        sha.update(part)
        sha.update(struct.pack("<I", len(part)))
    header = b"ANDROID!" + struct.pack(
        "<10I", len(kernel), KERNEL_ADDR, len(ramdisk), RAMDISK_ADDR, 0, SECOND_ADDR, TAGS_ADDR, PAGE, 0, 0
    )
    header += b"\0" * 16  # name
    header += cmdline.ljust(512, b"\0")
    header += sha.digest().ljust(32, b"\0")
    header += b"\0" * 1024  # extra command line
    image = pad(header) + pad(kernel) + pad(ramdisk)
    if a.max and len(image) > a.max:
        sys.exit("mkbootimg.py: %d bytes, larger than %d" % (len(image), a.max))
    with open(a.out, "wb") as f:
        f.write(image)
    print("%s: %d bytes (kernel %d, ramdisk %d)" % (a.out, len(image), len(kernel), len(ramdisk)))


if __name__ == "__main__":
    main()
