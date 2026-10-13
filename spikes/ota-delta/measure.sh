#!/bin/sh
# measure.sh <pair-id> <old> <new> [avg KiB list]  — one CSV row per average chunk size, on stdout.
# Every diff is applied and compared with `cmp` before its size counts (docs/32#transport).
set -eu
pair=$1; old=$2; new=$3; avgs=${4:-16 32 64 128 256}
w=$(mktemp -d /work/m.XXXX); trap 'rm -rf "$w"' EXIT
size() { stat -c%s "$1"; }
RAM=$(awk '/MemTotal/ {print $2*1024}' /proc/meminfo)
f_full=$(zstd -19 --long=31 -T0 -q -c "$new" | wc -c)
# Binary diffs, each verified.
xd=n/a; zp=n/a; bd=n/a
B=$(( $(size "$old") < 2147483648 ? $(size "$old") : 2147483648 ))
if xdelta3 -e -9 -B "$B" -f -s "$old" "$new" "$w/p.xd3" 2>/dev/null && xdelta3 -d -f -s "$old" "$w/p.xd3" "$w/r" && cmp -s "$w/r" "$new"; then xd=$(size "$w/p.xd3"); fi
if [ "$(size "$old")" -le 2147483648 ] && zstd -q -19 -T0 --long=31 --memory=2048MiB --patch-from="$old" "$new" -o "$w/p.zst" -f \
   && zstd -q -d --long=31 --memory=2048MiB --patch-from="$old" "$w/p.zst" -o "$w/r" -f && cmp -s "$w/r" "$new"; then zp=$(size "$w/p.zst"); fi
if [ $((17 * $(size "$old"))) -lt "$RAM" ] && bsdiff "$old" "$new" "$w/p.bsd" && bspatch "$old" "$w/r" "$w/p.bsd" && cmp -s "$w/r" "$new"; then bd=$(size "$w/p.bsd"); fi
rm -f "$w/r"
echo "pair,raw_old,raw_new,F_full,avg_KiB,chunks_new,missing,F_chunk_L3,F_chunk_L19,index_B,F_chunk_http,F_xdelta3,F_zstd_patch,F_bsdiff"
for avg in $avgs; do
  m=$((avg/4)):$avg:$((avg*4)); S=$w/store-$avg; mkdir -p "$S"
  desync --digest sha256 make -m "$m" -s "$S" "$w/old.caibx" "$old" >/dev/null 2>&1
  desync --digest sha256 make -m "$m" -s "$S" "$w/new.caibx" "$new" >/dev/null 2>&1
  desync --digest sha256 inspect-chunks -s "$S" "$w/old.caibx" "$w/old.json" >/dev/null 2>&1
  desync --digest sha256 inspect-chunks -s "$S" "$w/new.caibx" "$w/new.json" >/dev/null 2>&1
  info=$(desync --digest sha256 info --seed "$w/old.caibx" --chunks-info "$w/new.json" -f json "$w/new.caibx" -s "$S" 2>/dev/null)
  l3=$(echo "$info" | jq '."dedup-size-not-in-seed-nor-cache-compressed"')
  n=$(echo "$info" | jq '.unique')
  r=$(recompress.py "$S" "$w/new.json" "$w/old.json")
  miss=$(echo "$r" | jq .missing); l19=$(echo "$r" | jq .missing_l19)
  idx=$(zstd -19 -q -c "$w/new.caibx" | wc -c)
  http=$((l19 + idx + miss * 500))
  echo "$pair,$(size "$old"),$(size "$new"),$f_full,$avg,$n,$miss,$l3,$l19,$idx,$http,$xd,$zp,$bd"
  rm -rf "$S" "$w"/*.caibx "$w"/*.json
done
