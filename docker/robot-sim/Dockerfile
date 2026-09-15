# Fjarr robot-sim: a reproducible fake robot desktop for the agent to capture.
# Xvfb virtual X server + openbox + a few moving apps, viewable in a browser
# via x11vnc + noVNC on :6080. See docs/12-development-environment.md.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    xvfb openbox x11vnc novnc websockify \
    xterm x11-apps mesa-utils x11-utils \
    && rm -rf /var/lib/apt/lists/*

COPY start.sh /usr/local/bin/start.sh
RUN chmod +x /usr/local/bin/start.sh

# DISPLAY is shared with the dev container through the x11sock volume.
ENV DISPLAY=:99 \
    SIM_RESOLUTION=1920x1080x24

EXPOSE 6080
CMD ["/usr/local/bin/start.sh"]
