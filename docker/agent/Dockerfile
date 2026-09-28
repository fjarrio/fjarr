# fjarr-agent image — the robot-side product artifact (docs/12, docs/26; slice 5b, M2.5).
#
# Built FROM the .debs, so an apt robot and a container robot run identical bits (ADR-0031,
# docs/26#releases). Run `make deb` first; the build context is the repo root:
#   make deb && docker build -f docker/agent/Dockerfile --target runtime -t fjarr-agent .
#   make deb && docker build -f docker/agent/Dockerfile --target demo    -t fjarr-agent-demo .
# Stages: runtime (Ubuntu 26.04 + the fjarr-agent package, non-root with the published uid, refuses
# anything GPL the agent could load — ADR-0011) → demo-build (demo-robot built against libfjarr-dev,
# the embedder's way) → demo (the demo robot on the runtime). Publishing and the variants: docs/26.
ARG UBUNTU=ubuntu:26.04

# --------------------------------------------------------------- runtime --
FROM ${UBUNTU} AS runtime
ARG TARGETARCH
ENV DEBIAN_FRONTEND=noninteractive
COPY dist/deb/${TARGETARCH}/fjarr-agent_*.deb /tmp/debs/
# The image's user is created FIRST, with the published uid (docs/26#containerized-robots), so the
# package's sysusers entry finds it and only adds video and render. On an apt robot the uid is dynamic.
# systemd-standalone-sysusers satisfies the package's sysusers dependency without pulling systemd in.
# intel-media-va-driver (iHD) exists for amd64 only; arm64 devices get VA-API from mesa-va-drivers.
RUN groupadd -r -g 10001 fjarr && useradd -r -u 10001 -g fjarr -d /var/lib/fjarr -s /usr/sbin/nologin fjarr \
    && apt-get update && apt-get install -y --no-install-recommends \
       systemd-standalone-sysusers ca-certificates curl gstreamer1.0-tools \
       mesa-va-drivers $([ "$TARGETARCH" = amd64 ] && echo intel-media-va-driver) \
       /tmp/debs/fjarr-agent_*.deb \
    && rm -rf /var/lib/apt/lists/* /tmp/debs \
    && install -d -o fjarr -g fjarr -m 0700 /var/lib/fjarr \
    && install -d -o fjarr -g fjarr -m 0755 /run/fjarr \
    && id fjarr | grep -q 'uid=10001' && id -nG fjarr | grep -qw render \
    && if gst-inspect-1.0 x264enc >/dev/null 2>&1; then echo "x264enc is present: GPL plugin in a shipped image (ADR-0011)"; exit 1; fi \
    && if gst-inspect-1.0 avdec_h264 >/dev/null 2>&1; then echo "avdec_h264 is present: decodebin would load GPL-built libavcodec into the agent (ADR-0011)"; exit 1; fi \
    && gpl=$(dpkg-query -W -f '${Package}\n' 'libx264-*' 'libxvidcore*' 2>/dev/null | tr '\n' ' ') \
    && if [ -n "$gpl" ]; then echo "GPL codec libraries in a shipped image (ADR-0011): $gpl"; exit 1; fi
# libx265 IS present: Ubuntu's plugins-bad depends on it for x265enc, an encoder nothing autoplugs and
# the agent never selects (docs/14).
ENV FJARR_INTROSPECT_VIEWER_DIR=/usr/share/fjarr/viewer
USER fjarr
EXPOSE 7381
# Live, not static: the running agent's introspection endpoint answers (docs/24).
HEALTHCHECK --interval=15s --timeout=5s --start-period=20s \
  CMD curl -fsS ${FJARR_INTROSPECT_TOKEN:+-H "Authorization: Bearer $FJARR_INTROSPECT_TOKEN"} "http://127.0.0.1:${FJARR_INTROSPECT_PORT:-7381}/pipelines" >/dev/null || exit 1
ENTRYPOINT ["fjarr-agent"]

# ------------------------------------------------------------ demo-build --
# demo-robot is the "customer's robot software" (docs/02): built here the way an embedder builds, from
# libfjarr-dev with find_package(fjarr) — the same thing make deb-embed-test proves.
FROM ${UBUNTU} AS demo-build
ARG TARGETARCH
ENV DEBIAN_FRONTEND=noninteractive
COPY dist/deb/${TARGETARCH}/libfjarr-dev_*.deb /tmp/debs/
RUN apt-get update && apt-get install -y --no-install-recommends cmake ninja-build g++ /tmp/debs/libfjarr-dev_*.deb \
    && rm -rf /var/lib/apt/lists/*
COPY demos/demo-robot/main.cpp /src/main.cpp
RUN minor=$(dpkg-query -W -f '${Version}' libfjarr-dev | sed -E 's/^([0-9]+\.[0-9]+).*/\1/') \
    && printf 'cmake_minimum_required(VERSION 3.25)\nproject(demo-robot CXX)\nset(CMAKE_CXX_STANDARD 20)\nfind_package(fjarr %s REQUIRED)\nadd_executable(demo-robot main.cpp)\ntarget_link_libraries(demo-robot PRIVATE fjarr::fjarr)\n' "$minor" > /src/CMakeLists.txt \
    && cmake -S /src -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build /build

# ------------------------------------------------------------------ demo --
FROM runtime AS demo
COPY --from=demo-build /build/demo-robot /usr/local/bin/demo-robot
ENTRYPOINT ["demo-robot"]
