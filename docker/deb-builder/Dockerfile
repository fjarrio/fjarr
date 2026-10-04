# The package builder (docs/26#releases, ADR-0031): Ubuntu 26.04 with debhelper, the build
# dependencies of debian/control, the exact Rust pin (rust-toolchain.toml) and Node for the viewer.
# Native per architecture: CI runs it on amd64 and arm64 runners, never emulated.
FROM ubuntu:26.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential debhelper devscripts equivs fakeroot curl ca-certificates git \
    cmake ninja-build pkg-config \
    libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libgstreamer-plugins-bad1.0-dev \
    libva-dev nlohmann-json3-dev libtomlplusplus-dev libsystemd-dev libsoup-3.0-dev libnice-dev libei-dev libxkbcommon-dev libpipewire-0.3-dev libx11-dev libxtst-dev libxfixes-dev libxrandr-dev \
    nodejs npm \
    && rm -rf /var/lib/apt/lists/*
ARG RUST_VERSION=1.98.0
ENV RUSTUP_HOME=/opt/rustup CARGO_HOME=/opt/cargo PATH=/opt/cargo/bin:$PATH
RUN curl -fsSL https://sh.rustup.rs | sh -s -- -y --profile minimal --default-toolchain ${RUST_VERSION} \
    && chmod -R a+rwX /opt/rustup /opt/cargo
RUN npm install -g corepack && corepack enable
COPY build.sh /usr/local/bin/fjarr-deb-build
ENTRYPOINT ["fjarr-deb-build"]
