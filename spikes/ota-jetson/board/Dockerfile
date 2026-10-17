# The board package's check tools (M8 slice 8.0, docs/32#jetson-test-image): the device-tree
# compiler and C preprocessor NVIDIA's flash tools and kernel build run, GNU patch, dpkg-deb to
# list Stereolabs' package without installing it, and shellcheck for the scripts here. Nothing in
# this image ships. Build and run it from the host, never inside `dev`:
#   docker build -q -t fjarr-ota-jetson-board spikes/ota-jetson/board
#   spikes/ota-jetson/board/run.sh check-bct.sh
FROM ubuntu:26.04@sha256:c098726dd9b9733f3e885f8de0ef375206a311b0a317eadc40d347b1115fb944
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    device-tree-compiler cpp patch diffutils python3 \
    curl ca-certificates bzip2 zstd dpkg shellcheck \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /w
