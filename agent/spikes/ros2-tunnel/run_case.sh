#!/usr/bin/env bash
# THROWAWAY SPIKE FIXTURE: one cell of the probe matrix.
#   run_case.sh <rmw> <domain> [label]
# Restarts the relay for the domain's discovery port, publishes on the robot, and measures how
# long the client needs to see the topic and to receive a sample.
set -uo pipefail
RMW=$1; DOMAIN=${2:-0}; LABEL=${3:-"$RMW domain=$DOMAIN"}; EXTRA=${4:-}
ENVS="-e RMW_IMPLEMENTATION=$RMW"
[ -n "$EXTRA" ] && ENVS="$ENVS -e $EXTRA"
[ -n "${2:-}" ] && ENVS="$ENVS -e ROS_DOMAIN_ID=$DOMAIN"

for c in spike-robot spike-client; do
  docker exec $c pkill -f relay.py >/dev/null 2>&1
  docker exec $c pkill -f "ros2 topic" >/dev/null 2>&1
  docker exec $c pkill -f _ros2_daemon >/dev/null 2>&1
done
sleep 1
docker exec -d spike-robot sh -c "python3 /spike/relay.py $DOMAIN 172.19.0.3 7778 172.19.0.2 > /tmp/relay.log 2>&1"
docker exec -d spike-client sh -c "python3 /spike/relay.py $DOMAIN 172.19.0.2 7778 172.19.0.3 > /tmp/relay.log 2>&1"
sleep 1

docker exec -d $ENVS spike-robot bash -c "source /opt/ros/jazzy/setup.bash && ros2 topic pub -r 2 /spike_chatter std_msgs/String '{data: hello-from-robot}' > /tmp/pub.log 2>&1"
start=$(date +%s.%N)
seen=""
for i in $(seq 1 40); do
  out=$(docker exec $ENVS spike-client bash -c "source /opt/ros/jazzy/setup.bash && timeout 5 ros2 topic list --no-daemon 2>/dev/null" || true)
  if echo "$out" | grep -q spike_chatter; then seen=$(echo "$(date +%s.%N) - $start" | bc); break; fi
  sleep 0.5
done
if [ -z "$seen" ]; then
  echo "$LABEL | discovery: NOT SEEN within 20 s | data: -"
  docker exec spike-robot pkill -f "ros2 topic" >/dev/null 2>&1
  exit 0
fi
data=$(docker exec $ENVS spike-client bash -c "source /opt/ros/jazzy/setup.bash && timeout 15 ros2 topic echo --once --no-daemon /spike_chatter 2>/dev/null" || true)
got=$(echo "$data" | grep -c "hello-from-robot")
fwd=$(docker exec spike-robot sh -c 'tail -1 /tmp/relay.log' 2>/dev/null | tr -d '\n')
printf "%s | discovery: %.1f s | data: %s | robot relay: %s\n" "$LABEL" "$seen" "$([ "$got" -gt 0 ] && echo received || echo NONE)" "$fwd"
docker exec spike-robot pkill -f "ros2 topic" >/dev/null 2>&1
