#!/usr/bin/env python3
# A monitor identity for a forced connector (docs/07, headless robots): a real 1920x1080 monitor's
# EDID with its serial replaced — numeric serial and the 0xFF serial-string descriptor — so the fake
# monitor is never mistaken for the real one it was copied from. Fixes the base block checksum.
# Usage: make_edid.py SRC DST [SERIAL]. Throwaway spike code.
import sys

src, dst = sys.argv[1], sys.argv[2]
serial = (sys.argv[3] if len(sys.argv) > 3 else "FJARRVIRT1").encode()[:13]
e = bytearray(open(src, "rb").read())
assert e[:8] == bytes([0, 255, 255, 255, 255, 255, 255, 0]), "not an EDID"
e[12:16] = (0x464A4152).to_bytes(4, "little")  # numeric serial
for off in (54, 72, 90, 108):  # the four 18-byte descriptors
    if e[off:off + 3] == b"\0\0\0" and e[off + 3] == 0xFF:
        e[off + 5:off + 18] = (serial + b"\n").ljust(13, b" ")[:13]
e[127] = (-sum(e[:127])) & 0xFF
assert sum(e[:128]) & 0xFF == 0
open(dst, "wb").write(e)
print(f"wrote {dst}: {len(e)} bytes, serial {serial.decode()}")
