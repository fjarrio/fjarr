#!/usr/bin/env python3
"""Sum the zstd-19 size of the chunks of NEW that the robot does not have (docs/32#transport).

usage: recompress.py <store> <new-chunks.json> <old-chunks.json>
desync stores chunks at about zstd level 3; a production chunk store would use a higher level,
so the harness recompresses each missing chunk to compare fairly with level-19 diffs.
"""
import json, os, sys, zstandard

store, new_json, old_json = sys.argv[1:4]
old = {c["id"] for c in json.load(open(old_json))}
seen, raw, l19 = set(), 0, 0
d, c19 = zstandard.ZstdDecompressor(), zstandard.ZstdCompressor(level=19)
for c in json.load(open(new_json)):
    cid = c["id"]
    if cid in old or cid in seen:
        continue
    seen.add(cid)
    data = d.decompress(open(os.path.join(store, cid[:4], cid + ".cacnk"), "rb").read(), max_output_size=1 << 24)
    raw += len(data)
    l19 += min(len(c19.compress(data)), len(data))
print(json.dumps({"missing": len(seen), "missing_raw": raw, "missing_l19": l19}))
