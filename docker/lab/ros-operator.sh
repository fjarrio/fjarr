#!/usr/bin/env bash
# The operator's ROS 2 side: nothing runs here until a query is made, so this only waits. The query
# comes from `tunnel-checks.sh ros2`, which runs with a tunnel link up — the operator's interface
# has a carrier only then, and DDS ignores an interface whose carrier was down when the participant
# was created (docs/27#lifecycle).
#
# No DDS configuration: stock Fast DDS discovers the robot over the link, which is what docs/27
# claims and what ADR-0026 made true by letting multicast through.
# spec: docs/27-network-tunnel.md#ros2
set -euo pipefail
echo "operator-ros: ready, stock Fast DDS (no profile — none is needed, ADR-0026)"
exec sleep infinity
