# fjarr-agent image — the robot-side product artifact (docs/12, docs/26; slice 5b).
#
# Build context is the REPO ROOT, because the runtime carries the web viewer (docs/24#the-viewer):
#   docker build -f docker/agent/Dockerfile --target runtime -t fjarr-agent .
#   docker build -f docker/agent/Dockerfile --target demo    -t fjarr-agent-demo .
# Stages: viewer (node) → build (C++) → runtime (Ubuntu 26.04, runtime packages only, non-root,
# refuses x264enc per ADR-0011) → demo (the demo robot on top, the topology an embedder uses).
# Publishing, arm64 and the per-vendor variants are M2.5 (docs/17).

# ---------------------------------------------------------------- viewer --
FROM node:22-bookworm-slim AS viewer
RUN corepack enable
WORKDIR /src
# The lockfile's importer set: every workspace package's manifest, sources only for what is built.
COPY pnpm-workspace.yaml package.json pnpm-lock.yaml ./
COPY web/packages ./web/packages
COPY web/apps/introspect-viewer ./web/apps/introspect-viewer
COPY web/e2e/package.json ./web/e2e/package.json
COPY demos/demo-backend/package.json ./demos/demo-backend/package.json
COPY demos/demo-dashboard/package.json ./demos/demo-dashboard/package.json
COPY website/package.json ./website/package.json
RUN pnpm install --frozen-lockfile --filter "@fjarr/introspect-viewer..." \
    && pnpm -r --filter "@fjarr/introspect-viewer..." build

# ----------------------------------------------------------------- build --
FROM ubuntu:26.04 AS build
ENV DEBIAN_FRONTEND=noninteractive
# The same groups as .devcontainer/Dockerfile, build half only (docs/14).
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build pkg-config git ca-certificates \
    libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libgstreamer-plugins-bad1.0-dev \
    libva-dev nlohmann-json3-dev libtomlplusplus-dev libsystemd-dev libsoup-3.0-dev libgtest-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY agent ./agent
COPY demos/demo-robot ./demos/demo-robot
RUN cmake -S . -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /build --target fjarr-agent demo-robot

# --------------------------------------------------------------- runtime --
FROM ubuntu:26.04 AS runtime
ENV DEBIAN_FRONTEND=noninteractive
# NOTE: gstreamer1.0-plugins-ugly (GPL: x264enc) is deliberately absent — ADR-0011; asserted below.
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates curl \
    gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad \
    gstreamer1.0-nice gstreamer1.0-libav gstreamer1.0-tools \
    intel-media-va-driver mesa-va-drivers libva2 libva-drm2 \
    libsoup-3.0-0 glib-networking libsystemd0 libtomlplusplus3t64 \
    && rm -rf /var/lib/apt/lists/* \
    && if gst-inspect-1.0 x264enc >/dev/null 2>&1; then echo "x264enc is present: GPL plugin in a shipped image (ADR-0011)"; exit 1; fi
RUN useradd -r -u 10001 -m -d /var/lib/fjarr fjarr
COPY --from=build /build/agent/daemon/fjarr-agent /usr/local/bin/fjarr-agent
# The viewer where the fjarr-agent package installs it (docs/24, docs/26).
COPY --from=viewer /src/web/apps/introspect-viewer/dist /usr/share/fjarr/viewer
ENV FJARR_INTROSPECT_VIEWER_DIR=/usr/share/fjarr/viewer
USER fjarr
EXPOSE 7381
# The endpoint answers on loopback inside the container; the token, when set, is sent along.
HEALTHCHECK --interval=15s --timeout=5s --start-period=20s \
  CMD curl -fsS ${FJARR_INTROSPECT_TOKEN:+-H "Authorization: Bearer $FJARR_INTROSPECT_TOKEN"} "http://127.0.0.1:${FJARR_INTROSPECT_PORT:-7381}/pipelines" >/dev/null || exit 1
ENTRYPOINT ["fjarr-agent"]

# ------------------------------------------------------------------ demo --
# The demo robot layered on the runtime image: a customer's own binary on the base image.
FROM runtime AS demo
COPY --from=build /build/demos/demo-robot/demo-robot /usr/local/bin/demo-robot
ENTRYPOINT ["demo-robot"]
