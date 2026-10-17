#!/usr/bin/env python3
"""ppm2png.py IN.ppm OUT.png: a binary P6 PPM to PNG, standard library only.

For the frames capclient.py keeps with CAP_DUMP=path.ppm (and fjarr-opsim's frame dumps).
"""
import struct, sys, zlib
d = open(sys.argv[1], "rb").read()
_, rest = d.split(b"\n", 1); dims, rest = rest.split(b"\n", 1); _, px = rest.split(b"\n", 1)
w, h = map(int, dims.split())
def chunk(t, b): return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xffffffff)
raw = b"".join(b"\x00" + px[y * w * 3:(y + 1) * w * 3] for y in range(h))
open(sys.argv[2], "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
print(w, h)
