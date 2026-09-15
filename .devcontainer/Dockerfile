# Fjarr development container — Ubuntu 24.04 (noble), GStreamer 1.24
#
# Package groups are commented with the spec that requires them.
# See docs/12-development-environment.md and docs/14-dependencies.md.
FROM ubuntu:24.04

ARG USER_NAME=dev
ARG USER_UID=1000
ARG USER_GID=1000
# GID of the host's `render` group so /dev/dri is usable without --privileged.
ARG RENDER_GID=110
ARG VIDEO_GID=44

ENV DEBIAN_FRONTEND=noninteractive \
    TZ=Etc/UTC

RUN apt-get update && apt-get install -y --no-install-recommends \
    # --- base tooling -----------------------------------------------------
    ca-certificates curl wget gnupg git sudo less nano jq unzip xz-utils \
    python3 \
    # --- C++ build (docs/09 agent tier) -----------------------------------
    build-essential cmake ninja-build ccache pkg-config gdb \
    clang-18 clangd-18 clang-tidy-18 clang-format-18 \
    # --- GStreamer 1.24 (ADR-0001, ADR-0002) ------------------------------
    # NOTE: gstreamer1.0-plugins-ugly (GPL: x264enc) is deliberately absent —
    #       ADR-0011 license policy. gstreamer1.0-nice is required for
    #       webrtcbin ICE (camera-streamer lesson, docs/11-prior-art.md).
    libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
    libgstreamer-plugins-bad1.0-dev \
    gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad gstreamer1.0-nice gstreamer1.0-libav \
    gstreamer1.0-tools gstreamer1.0-pipewire \
    # --- VA-API hardware encode (docs/16 budgets; Intel iHD) --------------
    intel-media-va-driver libva-dev vainfo mesa-va-drivers \
    # --- X11 desktop backend (docs/07) ------------------------------------
    libx11-dev libxtst-dev libxfixes-dev libxrandr-dev libxi-dev x11-utils \
    # --- Wayland desktop backend (docs/07) --------------------------------
    libpipewire-0.3-dev libei-dev libdbus-1-dev xdg-desktop-portal \
    # --- uinput input backend (docs/07, ADR-0009) -------------------------
    libevdev-dev libudev-dev \
    # --- agent misc (docs/09) ---------------------------------------------
    nlohmann-json3-dev \
    # --- agent signaling client (ADR-0017) --------------------------------
    libsoup-3.0-dev glib-networking \
    # --- pipeline DOT graph debugging (camera-streamer lesson) ---------------------
    graphviz \
    && rm -rf /var/lib/apt/lists/*

# --- Node 22 + pnpm via corepack (web tier, website, docs lint) -----------
RUN curl -fsSL https://deb.nodesource.com/setup_22.x | bash - \
    && apt-get install -y --no-install-recommends nodejs \
    && rm -rf /var/lib/apt/lists/* \
    && corepack enable \
    && npm install -g markdownlint-cli2

# --- lychee link checker (docs gate; non-fatal if release URL moves) ------
RUN curl -fsSL -o /tmp/lychee.tar.gz \
      https://github.com/lycheeverse/lychee/releases/latest/download/lychee-x86_64-unknown-linux-musl.tar.gz \
    && tar -xzf /tmp/lychee.tar.gz -C /usr/local/bin --strip-components=1 \
         lychee-x86_64-unknown-linux-musl/lychee \
    && rm -f /tmp/lychee.tar.gz || echo "WARN: lychee install skipped"

# --- non-root user matching the host UID/GID ------------------------------
# ubuntu:24.04 ships a default `ubuntu` user at UID 1000; replace it.
RUN userdel -r ubuntu 2>/dev/null || true \
    && groupadd -g "${USER_GID}" "${USER_NAME}" 2>/dev/null || true \
    && useradd -m -s /bin/bash -u "${USER_UID}" -g "${USER_GID}" "${USER_NAME}" \
    && echo "${USER_NAME} ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/90-${USER_NAME} \
    && chmod 0440 /etc/sudoers.d/90-${USER_NAME} \
    # host GPU access: render + video groups by host GID
    && (getent group "${RENDER_GID}" || groupadd -g "${RENDER_GID}" hostrender) \
    && (getent group "${VIDEO_GID}"  || groupadd -g "${VIDEO_GID}"  hostvideo) \
    && usermod -aG "${RENDER_GID}","${VIDEO_GID}" "${USER_NAME}"

USER ${USER_NAME}
ENV HOME=/home/${USER_NAME} \
    CARGO_HOME=/home/${USER_NAME}/.cargo \
    RUSTUP_HOME=/home/${USER_NAME}/.rustup \
    # Intel Meteor Lake and newer want the iHD driver.
    LIBVA_DRIVER_NAME=iHD \
    PATH=/home/${USER_NAME}/.cargo/bin:${PATH}

# --- Rust toolchain (signaling tier). Exact version pinned by the repo's
#     rust-toolchain.toml; this preinstalls a matching default. ------------
RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs \
    | sh -s -- -y --profile minimal --default-toolchain 1.89.0 \
         --component clippy --component rustfmt

# Pre-create cache mount points so named volumes inherit dev ownership.
RUN mkdir -p /home/${USER_NAME}/.cargo/registry \
             /home/${USER_NAME}/.cache/ccache \
             /home/${USER_NAME}/.local/share/pnpm

WORKDIR /workspace
CMD ["sleep", "infinity"]
