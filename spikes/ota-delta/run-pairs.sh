#!/bin/bash
# Measure the 8.0 pairs (fetch-pairs.sh first). Chunks and whole-file diffs by measure.sh, file by
# file by filewise.py; the OS slots of spikes/ota-x86 file by file too. Output: CSV + JSON lines.
set -u
here=$(cd "$(dirname "$0")" && pwd); P=$here/out/pairs; X=$here/../ota-x86/out; R=$here/out/results; mkdir -p "$R"
D="docker run --rm -v $P:/in:ro -v $here/out/work:/work fjarr-ota-delta"
docker build -q -t fjarr-ota-delta "$here" >/dev/null
# M3 is made from M2-new
[ -s "$P/m3/new.onnx" ] || { mkdir -p "$P/m3"; cp "$P/m2/new.onnx" "$P/m3/old.onnx"
  docker run --rm -v "$P:/in" fjarr-ota-delta m3_patch.py /in/m3/old.onnx /in/m3/new.onnx; }
m() { $D measure.sh "$@" | tail -n +2 >> "$R/chunks.csv"; }
f() { echo "{\"pair\":\"$1\",\"filewise\":$($D filewise.py "$2" "$3")}" >> "$R/filewise.jsonl"; }
echo "pair,raw_old,raw_new,F_full,avg_KiB,chunks_new,missing,F_chunk_L3,F_chunk_L19,index_B,F_chunk_http,F_xdelta3,F_zstd_patch,F_bsdiff" > "$R/chunks.csv"
: > "$R/filewise.jsonl"
for s in M1 M2 M3 M4; do m $s /in/${s,,}/old.onnx /in/${s,,}/new.onnx "16 64 256"; done
m C2-binary /in/c2-old/rootfs/usr/bin/caddy /in/c2-new/rootfs/usr/bin/caddy "16 64 256"
m C2-layers /in/c2-old/layers.tar /in/c2-new/layers.tar "16 64 256"; f C2-files /in/c2-old/rootfs /in/c2-new/rootfs
m CU1-libs /in/cu1/old.so.cat /in/cu1/new.so.cat "16 64 256"; f CU1-files /in/cu1/old-libs /in/cu1/new-libs
m C1-layers /in/c1-old/layers.tar /in/c1-new/layers.tar "16 64 256"; f C1-files /in/c1-old/rootfs /in/c1-new/rootfs
# The OS slots of spikes/ota-x86, mounted read-only: U1 (OpenSSH), K1 (kernel), W1 (both).
docker run --rm --privileged -v /dev:/dev -v "$X:/x:ro" -v "$here/out/work:/work" fjarr-ota-delta bash -c '
  set -e; for v in w1old u1new r0a; do l=$(losetup -rf --show /x/$v/fjarr_1.0.root-x86-64.raw); mkdir -p /mnt/$v; mount -o ro,noload $l /mnt/$v; done
  for p in "U1 w1old u1new" "K1 u1new r0a" "W1 w1old r0a"; do set -- $p
    echo "{\"pair\":\"$1-files\",\"filewise\":$(filewise.py /mnt/$2 /mnt/$3)}"; done' >> "$R/filewise.jsonl"
echo "compressed registry bytes:" > "$R/registry.txt"
for c in c1 c2; do for s in old new; do echo "$c-$s $(paste -sd+ "$P/$c-$s/compressed-sizes" | bc 2>/dev/null || awk '{s+=$1} END{print s}' "$P/$c-$s/compressed-sizes")" >> "$R/registry.txt"; done; done
echo done
