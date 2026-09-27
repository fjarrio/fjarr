#!/usr/bin/env bash
# N runs per message size; one RESULT line per run appended to results/<label>.txt, full logs beside it.
#   N=10 LOSS=1% ./batch.sh 1184 1185 1280
# SCTP_PLUGIN (see run.sh) is passed through; the label records which plugin ran.
set -uo pipefail
N=${N:-10}; LOSS=${LOSS:-1%}; TOTAL=${TOTAL:-1024}
here=$(cd "$(dirname "$0")" && pwd); mkdir -p "$here/results"
for size in "$@"; do
  variant=$(basename "${SCTP_PLUGIN:-distro}" .so); variant=${variant#libgstsctp-}
  label="$variant-size${size}-loss${LOSS%\%}pct-delay${DELAY:-0ms}"
  for i in $(seq "$N"); do
    LOSS=$LOSS DELAY=${DELAY:-0ms} timeout 900 "$here/run.sh" --size "$size" --total-mb "$TOTAL" > "$here/results/$label-run$i.log" 2>&1
    echo "run $i rc=$? $(grep RESULT "$here/results/$label-run$i.log")" | tee -a "$here/results/$label.txt"
  done
done
