#!/usr/bin/env bash
# The robot's ROS 2 side for the docs/27#ros2 gate: a participant publishing something an operator
# can find over the tunnel. It waits for the tunnel interface first, because DDS binds its
# interfaces when a participant is created and one that appears later is invisible to it forever —
# which is the whole reason docs/27's lifecycle rule exists.
set -euo pipefail
IFACE=${FJARR_TUN_DEV:-fjarr0}

# This image has no iproute2, and adding one would mean maintaining a ROS image; /proc/net/dev
# lists the interfaces in this namespace, which is all the wait needs.
for i in $(seq 1 120); do
  grep -q "^ *$IFACE:" /proc/net/dev && break
  [ "$i" = 1 ] && echo "robot-ros: waiting for $IFACE (docs/27#lifecycle: a participant created before it exists never sees it)"
  sleep 1
done
grep -q "^ *$IFACE:" /proc/net/dev \
  || { echo "robot-ros: $IFACE never appeared — run 'make tun-up ROS=1', which orders this correctly"; exit 1; }
echo "robot-ros: $IFACE is present in this namespace"

# ROS's setup script reads unset variables, so `set -u` has to stand down for it.
set +u
source /opt/ros/jazzy/setup.bash
set -u

echo "robot-ros: publishing /fjarr/robot_heartbeat with $RMW_IMPLEMENTATION on domain $ROS_DOMAIN_ID"
exec ros2 topic pub --rate 2 /fjarr/robot_heartbeat std_msgs/msg/String "{data: 'demo-robot-01'}"
