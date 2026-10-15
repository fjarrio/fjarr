# ota-delta — M8 slice 8.0 delta measurements

Chunks (desync, casync-compatible, SHA-256) against binary diffs (xdelta3, `zstd --patch-from`,
bsdiff) on pinned update pairs, for [docs/32#transport](../../docs/32-fleet-ota.md#transport).
Run on a build host, never on a lab machine:

```bash
docker build -t fjarr-ota-delta spikes/ota-delta
docker run --rm -v <dir>:/in:ro fjarr-ota-delta measure.sh <pair> /in/old /in/new "16 32 64 128 256"
```

One CSV row per average chunk size. `F_chunk_L19` is the missing chunks recompressed at zstd 19
(desync stores them at about level 3); `F_chunk_http` adds the zstd'd index and an assumed 500 B
per request. Every diff is applied and compared with `cmp` before its size counts; `n/a` means
the tool's limit (bsdiff needs 17× the old size in RAM and is capped at 400 MB, `--patch-from` stops at 2 GiB).

`filewise.py <old-dir> <new-dir>` is the file-by-file variant: unchanged files cost nothing, a changed
file costs the smallest of its `zstd --patch-from` diff, its bsdiff and itself at zstd 19, plus the
compressed file list. `fetch-pairs.sh` and `run-pairs.sh` fetch the pinned 2026-10-15 pairs (containers,
CUDA, models) and measure them, with the OS slots of `spikes/ota-x86`.
