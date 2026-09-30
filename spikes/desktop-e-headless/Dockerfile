# Throwaway spike (M3 slice 3.0, ADR-0033): does mutter --headless in a container with no GPU give
# RemoteDesktop, ScreenCast, a PipeWire frame and input? Reuses the M2 probe and oracle.
FROM ubuntu:26.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      mutter dbus dbus-user-session pipewire pipewire-bin wireplumber \
      libgl1-mesa-dri mesa-vulkan-drivers \
      gstreamer1.0-pipewire gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-tools \
      python3-dbus python3-gi gir1.2-gst-plugins-base-1.0 gir1.2-gstreamer-1.0 gir1.2-gtk-4.0 \
      libei1 procps \
    && rm -rf /var/lib/apt/lists/*
RUN useradd -m robot
USER robot
WORKDIR /home/robot
COPY --chown=robot run.sh /home/robot/run.sh
