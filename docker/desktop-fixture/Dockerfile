# The desktop fixture (M3 slice 3.0, ADR-0033, docs/15#the-desktop-test-lab): GNOME's compositor
# headless in a container, with no GPU, so backend E's capture and input are tested on every push.
# `mutter --headless --virtual-monitor` offers RemoteDesktop and ScreenCast on its session bus; a test
# window paints one known colour and logs every focus change, key and click it receives, so capture
# is proven by reading pixels and input by reading the log — never by a call returning success.
# The session helper, compiled from this tree so the fixture tests the helper of this commit on every
# push without building the whole agent (repo-root build context; .dockerignore keeps it to sources).
FROM ubuntu:26.04 AS helper
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends g++ pkg-config libglib2.0-dev nlohmann-json3-dev libpipewire-0.3-dev \
    && rm -rf /var/lib/apt/lists/*
COPY agent/desktop-session /src/agent/desktop-session
COPY agent/src/desktop/helper_protocol.hpp agent/src/desktop/helper_protocol.cpp /src/agent/src/desktop/
RUN g++ -std=c++20 -O2 -Wall -Wextra -Werror -I/src/agent/src \
      /src/agent/desktop-session/main.cpp /src/agent/desktop-session/mutter.cpp /src/agent/desktop-session/pipewire.cpp \
      /src/agent/src/desktop/helper_protocol.cpp \
      $(pkg-config --cflags --libs gio-unix-2.0 libpipewire-0.3) -o /fjarr-desktop-session

FROM ubuntu:26.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      mutter dbus pipewire pipewire-bin wireplumber libgl1-mesa-dri \
      gstreamer1.0-pipewire gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
      python3-dbus python3-gi gir1.2-gstreamer-1.0 gir1.2-gst-plugins-base-1.0 gir1.2-gtk-4.0 \
      libei1 procps \
    && rm -rf /var/lib/apt/lists/*
# The desktop's own account, as on a robot: an ordinary user whose session this is.
RUN useradd -m -u 10002 desktop && install -d -o desktop -g desktop -m 0700 /run/desktop
ENV XDG_RUNTIME_DIR=/run/desktop \
    DBUS_SESSION_BUS_ADDRESS=unix:path=/run/desktop/bus \
    WAYLAND_DISPLAY=wayland-0 \
    FJARR_FIXTURE_LOG=/run/desktop/testwin.log \
    FIXTURE_WIDTH=1280 FIXTURE_HEIGHT=720
COPY --chmod=0755 docker/desktop-fixture/session.sh /usr/local/bin/fixture-session
COPY --chmod=0755 docker/desktop-fixture/selftest.py /usr/local/bin/fixture-selftest
COPY --chmod=0755 docker/desktop-fixture/helper_check.py /usr/local/bin/fixture-helper-check
COPY --chmod=0755 docker/desktop-fixture/testwin.py /usr/local/bin/fixture-testwin
COPY --from=helper /fjarr-desktop-session /usr/local/bin/fjarr-desktop-session
USER desktop
WORKDIR /home/desktop
HEALTHCHECK --interval=2s --timeout=2s --start-period=30s --retries=15 CMD test -f /run/desktop/ready
ENTRYPOINT ["/usr/local/bin/fixture-session"]
