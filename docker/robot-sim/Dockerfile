# Fjarr robot-sim: a reproducible fake robot desktop for the agent to capture.
# Xvfb virtual X server + openbox + a few moving apps, viewable in a browser
# via x11vnc + noVNC on :6080. See docs/12-development-environment.md.
FROM ubuntu:26.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    xvfb openbox x11vnc novnc websockify \
    xterm x11-apps mesa-utils x11-utils \
    x11-xserver-utils xauth python3-xlib xclip \
    && rm -rf /var/lib/apt/lists/*

# The X11 fixture (SIM_FIXTURE=1, M3 3.7): the agent's account, so `xhost +si:localuser:fjarr` names
# the uid the agent's container runs as.
RUN useradd --system --uid 10003 --no-create-home --shell /usr/sbin/nologin fjarr

COPY start.sh testwin_x11.py plugd_x11.py /usr/local/bin/
RUN chmod +x /usr/local/bin/start.sh /usr/local/bin/testwin_x11.py /usr/local/bin/plugd_x11.py

# DISPLAY is shared with the dev container through the x11sock volume.
ENV DISPLAY=:99 \
    SIM_RESOLUTION=1920x1080x24

EXPOSE 6080
HEALTHCHECK --interval=2s --timeout=2s --retries=30 CMD [ "${SIM_FIXTURE:-0}" != 1 ] || test -f /run/fixture/ready
CMD ["/usr/local/bin/start.sh"]
