#!/usr/bin/env bash
# Boot the fake robot desktop: Xvfb + openbox + moving apps + noVNC viewer.
set -u

: "${DISPLAY:=:99}"
: "${SIM_RESOLUTION:=1920x1080x24}"

# The /tmp/.X11-unix volume is shared with the dev container; make sure a
# stale socket from a previous run doesn't block Xvfb. The lock file lives
# in the container's own /tmp and survives an unclean stop (host reboot,
# docker restart): "Server is already active for display 99" otherwise.
rm -f "/tmp/.X11-unix/X${DISPLAY#:}" "/tmp/.X${DISPLAY#:}-lock" 2>/dev/null || true
mkdir -p /tmp/.X11-unix && chmod 1777 /tmp/.X11-unix

echo "robot-sim: starting Xvfb ${DISPLAY} at ${SIM_RESOLUTION}"
Xvfb "${DISPLAY}" -screen 0 "${SIM_RESOLUTION}" -nolisten tcp &
XVFB_PID=$!

# Wait for the X server to accept connections.
for _ in $(seq 1 50); do
  if xdpyinfo -display "${DISPLAY}" >/dev/null 2>&1; then break; fi
  sleep 0.2
done

openbox &
# Moving content so captured video is never a static frame:
xclock -update 1 -geometry 300x300+50+50 &
xeyes -geometry 200x200+400+80 &
# No glxgears: on Xvfb it renders unthrottled through llvmpipe, burning ~3.5 cores for ever and
# pushing the laptop into package-power throttling — which pinned the iGPU (and the VA-API
# encoder under test) to its floor. The clock's second hand and the heartbeat terminal keep the
# desktop moving at 1 Hz, which is what capture tests need.
xterm -geometry 100x12+400+340 -e \
  'while true; do date "+robot-sim heartbeat  %F %T"; sleep 1; done' &

echo "robot-sim: starting x11vnc + noVNC on :6080"
x11vnc -display "${DISPLAY}" -forever -shared -nopw -quiet -bg
websockify --web=/usr/share/novnc 6080 localhost:5900 &

wait "${XVFB_PID}"
