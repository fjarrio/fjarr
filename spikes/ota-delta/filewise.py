#!/usr/bin/env python3
"""File-by-file delta between two trees (docs/32#transport, slice 8.0).

usage: filewise.py <old-dir> <new-dir>

For every regular file in NEW: unchanged (same SHA-256 at the same path) costs nothing; content
the robot already holds anywhere in OLD (same SHA-256 at another path, as hard links and moved files
are) costs nothing; changed costs the smallest of a zstd --patch-from diff, a bsdiff and the file
alone at zstd 19; added costs the file at zstd 19. Each distinct new content is paid for once. The file list (path, mode, size, hash, link targets) is added at zstd 19.
Every diff is applied and compared before its size counts. Prints one JSON object.
"""
import hashlib, json, os, stat, subprocess, sys, tempfile
from concurrent.futures import ThreadPoolExecutor

BSDIFF_MAX = 256 << 20   # bsdiff needs about 17x the old file in memory
PATCH_MAX = 2 << 30      # zstd --patch-from's window limit

def walk(root):
    out = {}
    for d, dirs, files in os.walk(root):
        for n in dirs + files:
            p = os.path.join(d, n)
            rel = os.path.relpath(p, root)
            st = os.lstat(p)
            if stat.S_ISLNK(st.st_mode):
                out[rel] = ("l", os.readlink(p), st.st_mode)
            elif stat.S_ISREG(st.st_mode):
                out[rel] = ("f", st.st_size, st.st_mode)
    return out

def sha(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()

def zsize(p):
    r = subprocess.run(["zstd", "-19", "-T1", "-q", "-c", p], capture_output=True, check=True)
    return len(r.stdout)

def cost_changed(old, new, tmp):
    best, how = zsize(new), "zstd"
    size = os.path.getsize(new)
    t = tempfile.mkdtemp(dir=tmp)
    patch, back = os.path.join(t, "p"), os.path.join(t, "r")
    if max(size, os.path.getsize(old)) <= PATCH_MAX:
        mem = ["--memory=2048MiB", "--long=31"]
        if subprocess.run(["zstd", "-q", "-19", "-T1", *mem, f"--patch-from={old}", new, "-o", patch, "-f"]).returncode == 0 \
           and subprocess.run(["zstd", "-q", "-d", *mem, f"--patch-from={old}", patch, "-o", back, "-f"]).returncode == 0 \
           and subprocess.run(["cmp", "-s", back, new]).returncode == 0 and os.path.getsize(patch) < best:
            best, how = os.path.getsize(patch), "patch-from"
    if os.path.getsize(old) <= BSDIFF_MAX:
        if subprocess.run(["bsdiff", old, new, patch], capture_output=True).returncode == 0 \
           and subprocess.run(["bspatch", old, back, patch], capture_output=True).returncode == 0 \
           and subprocess.run(["cmp", "-s", back, new]).returncode == 0 and os.path.getsize(patch) < best:
            best, how = os.path.getsize(patch), "bsdiff"
    subprocess.run(["rm", "-rf", t])
    return best, how

def main():
    old_root, new_root = sys.argv[1], sys.argv[2]
    old, new = walk(old_root), walk(new_root)
    tmp = tempfile.mkdtemp(dir=os.environ.get("TMPDIR", "/tmp"))
    listing, jobs = [], []
    res = {"files_new": 0, "unchanged": 0, "changed": 0, "added": 0, "removed": 0,
           "bytes_changed": 0, "bytes_added": 0, "raw_changed": 0, "raw_added": 0, "methods": {}}
    old_hashes = {sha(os.path.join(old_root, r)) for r, v in old.items() if v[0] == "f"}
    paid = set()
    res["duplicate"] = 0
    for rel, (kind, a, mode) in sorted(new.items()):
        if kind == "l":
            listing.append(f"l {mode:o} {rel} -> {a}")
            continue
        res["files_new"] += 1
        h = sha(os.path.join(new_root, rel))
        listing.append(f"f {mode:o} {a} {h} {rel}")
        o = old.get(rel)
        if h in old_hashes:
            res["unchanged"] += 1          # the robot has this content already, here or elsewhere
        elif h in paid:
            res["duplicate"] += 1          # another path of this release carries the same content
        else:
            paid.add(h)
            jobs.append(("changed" if o and o[0] == "f" else "added", rel))
    res["removed"] = sum(1 for r, v in old.items() if v[0] == "f" and r not in new)

    def run(job):
        kind, rel = job
        n = os.path.join(new_root, rel)
        if kind == "added":
            return kind, rel, zsize(n), "zstd", os.path.getsize(n)
        c, how = cost_changed(os.path.join(old_root, rel), n, tmp)
        return kind, rel, c, how, os.path.getsize(n)

    largest = []
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
        for kind, rel, c, how, raw in ex.map(run, jobs):
            res[kind] += 1
            res["bytes_" + kind] += c
            res["raw_" + kind] += raw
            res["methods"][how] = res["methods"].get(how, 0) + 1
            largest.append((c, kind, rel, how, raw))
    lp = os.path.join(tmp, "listing")
    with open(lp, "w") as f:
        f.write("\n".join(listing) + "\n")
    res["bytes_listing"] = zsize(lp)
    res["total"] = res["bytes_changed"] + res["bytes_added"] + res["bytes_listing"]
    res["largest"] = [{"bytes": c, "kind": k, "path": r, "method": h, "raw": raw}
                      for c, k, r, h, raw in sorted(largest, reverse=True)[:8]]
    subprocess.run(["rm", "-rf", tmp])
    print(json.dumps(res))

if __name__ == "__main__":
    main()
