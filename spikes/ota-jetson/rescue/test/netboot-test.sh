#!/bin/bash
# Run the network rescue's off-box bench (bench.sh) after `build.sh --test`: real UEFI PXE and HTTP
# boot (AAVMF in QEMU) against net-rescue.sh's own configuration, in a private network inside one
# privileged container that has no outside network. Logs go to out/rescue/<l4t>/test/logs-<time>/.
#   test/netboot-test.sh [36.4.4|36.5.2]
# spec: docs/32-fleet-ota.md#jetson-test-image ("Three rescue layers", item 2)
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
l4t=${1:-36.4.4}
out=$(dirname "$here")/out/rescue/$l4t
[ -f "$out/test/fjarr-net-rescue-test.efi" ] || { echo "netboot-test: run build.sh --l4t $l4t --test first" >&2; exit 1; }
log=$out/test/logs-$(date +%Y%m%d-%H%M%S)
mkdir -p "$log"
echo "logs: $log"
docker run --rm --privileged --network none -v "$here:/src:ro" -v "$out:/out:ro" -v "$log:/log" \
	fjarr-jetson-rescue-tools:26.04 bash /src/test/bench.sh
