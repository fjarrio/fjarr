#!/usr/bin/env python3
"""Off-box tests of capsule-check.py, on a synthetic capsule and flash record.

spec: docs/32-fleet-ota.md#jetson-test-image (rule L4)

    python3 -I test-capsule-check.py
"""
import importlib.util
import os
import struct
import tempfile

here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("cc", os.path.join(here, "capsule-check.py"))
cc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cc)

VER = b"NV4\n# R36 , REVISION: 5.2\nBOARDID=3767 BOARDSKU=0000 FAB=%s\n%s\n0x240502\nBYTES:85 CRC32:%s\n"


def gpt(guid_byte):
    t = bytearray(16896)
    for e in range(4):
        t[e * 128:e * 128 + 16] = bytes([0xA0 + e]) * 16  # type GUID: fixed
        t[e * 128 + 16:e * 128 + 32] = bytes([guid_byte + e]) * 16  # unique GUID: random
        t[e * 128 + 56:e * 128 + 60] = b"p%d\0" % e
    t[16384:16392] = b"EFI PART"
    t[16384 + 16:16384 + 20] = bytes([guid_byte]) * 4  # header CRC
    t[16384 + 56:16384 + 72] = bytes([guid_byte]) * 16  # disk GUID
    t[16384 + 80:16384 + 84] = struct.pack("<I", 128)  # entries: fixed
    return bytes(t)


def capsule(images):
    header = 40
    table = bytearray()
    body = bytearray()
    off = header + 184 * len(images)
    for name, data in images:
        entry = name.encode().ljust(40, b"\0") + struct.pack("<2I", off + len(body), len(data))
        table += entry.ljust(184, b"\0")
        body += data
    size = off + len(body)
    blob = struct.pack("<16s6I", cc.MAGIC, 0x1030622, size, header, len(images), 0, size) + table + body
    return b"\x00" * 100 + blob  # the FMP capsule's own headers come first


def record(d):
    os.makedirs(os.path.join(d, "signed"))
    for f, data in (("mb1_sig.bin.encrypt", b"mb1" * 100), ("uefi_sig.bin.encrypt", b"uefi" * 200)):
        open(os.path.join(d, "signed", f), "wb").write(data)
    open(os.path.join(d, "gpt_secondary_3_0.bin"), "wb").write(gpt(1))
    open(os.path.join(d, "gpt_backup_secondary_3_0.bin"), "wb").write(gpt(2))
    open(os.path.join(d, "box_bootblob_ver.txt"), "wb").write(VER % (b"303", b"20261114024845", b"2088C67"))


def images(**change):
    base = {
        "mb1": b"mb1" * 100,
        "cpu-bootloader": b"uefi" * 200,
        "secondary_gpt": gpt(7),
        "secondary_gpt_backup": gpt(9),
        "VER": VER % (b"000", b"20261114013212", b"DF6A2B4"),
    }
    base.update(change)
    return list(base.items())


def run(name, imgs, want_bad):
    with tempfile.TemporaryDirectory() as d:
        record(os.path.join(d, "rec"))
        cap = os.path.join(d, "c.cap")
        open(cap, "wb").write(capsule(imgs))
        _, bad = cc.check(cap, os.path.join(d, "rec"))
    ok = bad == want_bad
    print(("ok  " if ok else "FAIL") + f" {name}: bad {bad}, want {want_bad}")
    return ok


def flip(data, at):
    b = bytearray(data)
    b[at] ^= 0xFF
    return bytes(b)


cases = [
    ("the flash's firmware, other GUIDs, FAB and time", images(), []),
    ("a byte of mb1 changed", images(mb1=flip(b"mb1" * 100, 5)), ["mb1"]),
    ("an image the flash never wrote", images() + [("tsec-fw", b"x" * 64)], ["tsec-fw"]),
    ("a partition's type GUID changed", images(secondary_gpt=flip(gpt(7), 3)), ["secondary_gpt"]),
    ("a partition's name changed", images(secondary_gpt_backup=flip(gpt(9), 128 + 57)), ["secondary_gpt_backup"]),
    ("another release in VER", images(VER=VER.replace(b"5.2", b"4.4") % (b"000", b"20261114013212", b"1")), ["VER"]),
]
results = [run(*c) for c in cases]
try:
    cc.blob_images(b"no blob here")
    print("FAIL no blob: accepted")
    results.append(False)
except ValueError:
    print("ok   no blob: refused")
    results.append(True)
print(f"{sum(results)}/{len(results)} capsule-check cases")
raise SystemExit(0 if all(results) else 1)
