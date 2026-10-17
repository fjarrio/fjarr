#!/usr/bin/env python3
"""Write a staged directory tree as an initramfs (cpio "newc") without root.

Every entry is owned by root, carries one fixed timestamp and comes in sorted order, so the same
tree gives the same archive. --mode sets a path's permission bits (keys, /root), --dev adds a
device node (the kernel needs /dev/console before /init runs), --file replaces a file's content
(the test variant's authorized_keys).

spec: docs/32-fleet-ota.md#jetson-test-image ("Three rescue layers", item 2)
"""

import argparse
import os
import stat
import sys


def header(ino, mode, nlink, mtime, size, rdevmajor, rdevminor, name):
    fields = (ino, mode, 0, 0, nlink, mtime, size, 0, 0, rdevmajor, rdevminor, len(name) + 1, 0)
    return b"070701" + b"".join(b"%08X" % f for f in fields) + name + b"\0"


def pad(n):
    return b"\0" * (-n % 4)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("root")
    ap.add_argument("output")
    ap.add_argument("--mode", action="append", default=[], metavar="PATH=OCTAL")
    ap.add_argument("--dev", action="append", default=[], metavar="PATH=c:MAJOR:MINOR:OCTAL")
    ap.add_argument("--file", action="append", default=[], metavar="PATH=SOURCE")
    ap.add_argument("--mtime", type=int, default=int(os.environ.get("SOURCE_DATE_EPOCH", "0")))
    args = ap.parse_args()

    modes = {p.strip("/"): int(m, 8) for p, m in (a.split("=", 1) for a in args.mode)}
    files = {p.strip("/"): s for p, s in (a.split("=", 1) for a in args.file)}
    devs = {}
    for a in args.dev:
        p, spec = a.split("=", 1)
        kind, major, minor, mode = spec.split(":")
        devs[p.strip("/")] = ((stat.S_IFCHR if kind == "c" else stat.S_IFBLK) | int(mode, 8),
                              int(major), int(minor))

    entries = []
    for dirpath, dirnames, filenames in os.walk(args.root):
        dirnames.sort()
        rel = os.path.relpath(dirpath, args.root)
        for name in [None] + sorted(filenames + [d for d in dirnames
                                                 if os.path.islink(os.path.join(dirpath, d))]):
            path = dirpath if name is None else os.path.join(dirpath, name)
            arc = rel if name is None else os.path.normpath(os.path.join(rel, name))
            if arc == ".":
                continue
            entries.append((arc, path))
    known = {a for a, _ in entries}
    missing = [p for p in list(modes) + list(files) if p not in known]
    if missing:
        sys.exit(f"mkcpio: not in the tree: {', '.join(missing)}")

    ino = 1
    with open(args.output, "wb") as out:
        def emit(arc, mode, data=b"", rdev=(0, 0)):
            nonlocal ino
            h = header(ino, mode, 2 if stat.S_ISDIR(mode) else 1, args.mtime, len(data),
                       rdev[0], rdev[1], arc.encode())
            out.write(h + pad(len(h)) + data + pad(len(data)))
            ino += 1

        for arc, path in entries:
            st = os.lstat(path)
            if stat.S_ISLNK(st.st_mode):
                emit(arc, stat.S_IFLNK | 0o777, os.readlink(path).encode())
            elif stat.S_ISDIR(st.st_mode):
                emit(arc, stat.S_IFDIR | modes.get(arc, 0o755))
            elif stat.S_ISREG(st.st_mode):
                src = files.get(arc, path)
                with open(src, "rb") as f:
                    data = f.read()
                perm = modes.get(arc, 0o755 if st.st_mode & 0o111 else 0o644)
                emit(arc, stat.S_IFREG | perm, data)
            else:
                sys.exit(f"mkcpio: unsupported file type: {path}")
            if stat.S_ISDIR(st.st_mode):
                for dev in sorted(d for d in devs if os.path.dirname(d) == arc):
                    mode, major, minor = devs.pop(dev)
                    emit(dev, mode, rdev=(major, minor))
        if devs:
            sys.exit(f"mkcpio: no directory for {', '.join(devs)}")
        emit("TRAILER!!!", 0)
        out.write(b"\0" * (-out.tell() % 512))


if __name__ == "__main__":
    main()
