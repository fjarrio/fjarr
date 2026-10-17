#!/usr/bin/env bash
# Run one of the board package's scripts in its tools image (Dockerfile here), as the calling
# user, with spikes/ota-jetson mounted at /w so out/ and .lab/ are where the scripts expect.
#   spikes/ota-jetson/board/run.sh check-bct.sh | check-dtb.sh | fetch-check.sh | lint
# Run it on the host, never inside `dev`. spec: docs/32-fleet-ota.md#jetson-test-image
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
spike=$(dirname "$here")
image=fjarr-ota-jetson-board
what=${1:?usage: run.sh <script in board/> [args...] | lint}
shift

docker build -q -t "$image" "$here" >/dev/null
args=(--rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$spike:/w" -w /w "$image")
if [ "$what" = lint ]; then
  exec docker run "${args[@]}" bash -c 'shellcheck -x board/*.sh &&
    python3 -I -c "import ast, sys; ast.parse(open(sys.argv[1]).read())" board/dtbdiff.py &&
    echo "lint: clean"'
fi
[ -f "$here/$what" ] || { echo "run.sh: no board/$what" >&2; exit 2; }
exec docker run "${args[@]}" "board/$what" "$@"
