#!/usr/bin/env bash
# Run repro.py in a throwaway container with netem loss on lo. The container needs an eth0 (libnice
# gathers no loopback candidates), but both peers use its address, so every packet crosses lo.
# Nothing Fjarr-specific: the image is only a convenient Ubuntu 26.04 + GStreamer 1.28.2.
#   LOSS=1% DELAY=25ms ./run.sh --size 1280 --total-mb 1024
# SCTP_PLUGIN=/path/libgstsctp.so replaces the distribution's sctp plugin inside the container
# (see build-plugin.sh), so a stock rebuild and a one-line-fixed rebuild can be compared.
set -euo pipefail
IMAGE=${IMAGE:-fjarr-dev}
LOSS=${LOSS:-1%}
DELAY=${DELAY:-0ms}   # one-way on lo, so both directions: RTT = 2 x DELAY
here=$(cd "$(dirname "$0")" && pwd)
plugin=()
[ -n "${SCTP_PLUGIN:-}" ] && plugin=(-v "$(realpath "$SCTP_PLUGIN"):/usr/lib/x86_64-linux-gnu/gstreamer-1.0/libgstsctp.so:ro")
exec docker run --rm --cap-add NET_ADMIN -u root -v "$here:/r:ro" "${plugin[@]}" "$IMAGE" \
  sh -c "ip link set lo up; if [ '$LOSS' != 0 ]; then tc qdisc add dev lo root netem delay $DELAY loss $LOSS limit 100000; fi; \
         exec setpriv --reuid=1000 --regid=1000 --clear-groups python3 /r/repro.py $*"
