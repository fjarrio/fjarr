# Fjarr robot-sim: a reproducible fake robot desktop for the agent to capture.
# Xvfb virtual X server + openbox + a few moving apps, viewable in a browser
# via x11vnc + noVNC on :6080. See docs/12-development-environment.md.
FROM ubuntu:26.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    xvfb openbox x11vnc novnc websockify \
    xterm x11-apps mesa-utils x11-utils \
    x11-xserver-utils xauth python3-xlib xclip \
    pipewire pipewire-bin wireplumber dbus util-linux \
    && rm -rf /var/lib/apt/lists/*

# The X11 fixture (SIM_FIXTURE=1, M3 3.7): the agent's account, so `xhost +si:localuser:fjarr` names
# the uid the agent's container runs as.
RUN useradd --system --uid 10003 --no-create-home --shell /usr/sbin/nologin fjarr
# The kiosk's own account (M3.5 3.5.2, ADR-0034): its session runs PipeWire and fjarr-x11-session, which
# hands the agent the speakers over the shared socket. fjarr-desktop is gid 10003, the x11-robot agent's
# group, which gives the socket that group; the kiosk is a member, as on a robot.
RUN groupadd --gid 10003 fjarr-desktop && useradd --uid 10004 --create-home --shell /bin/bash --groups fjarr-desktop kiosk
COPY speakers.conf /etc/pipewire/pipewire.conf.d/fixture-speakers.conf

COPY start.sh testwin_x11.py plugd_x11.py /usr/local/bin/
RUN chmod +x /usr/local/bin/start.sh /usr/local/bin/testwin_x11.py /usr/local/bin/plugd_x11.py

# DISPLAY is shared with the dev container through the x11sock volume.
ENV DISPLAY=:99 \
    SIM_RESOLUTION=1920x1080x24

EXPOSE 6080
HEALTHCHECK --interval=2s --timeout=2s --retries=30 CMD [ "${SIM_FIXTURE:-0}" != 1 ] || test -f /run/fixture/ready
CMD ["/usr/local/bin/start.sh"]
