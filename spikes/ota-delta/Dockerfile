# The 8.0 delta harness (docs/32#transport, slice 8.0): chunks against binary diffs on pinned
# update pairs. Runs on a build host, never on a lab machine. GPL tools here (Ubuntu's xdelta3,
# diffoscope) are harness-only; a robot-side decoder would be Apache xdelta 3.1+ or zstd.
FROM ubuntu:26.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    xdelta3 bsdiff zstd zchunk casync diffoscope fuse2fs e2fsprogs \
    python3 python3-zstandard jq time curl ca-certificates \
    && rm -rf /var/lib/apt/lists/*
ARG DESYNC_VERSION=1.1.4
RUN cd /tmp && curl -fsSLO https://github.com/folbricht/desync/releases/download/v${DESYNC_VERSION}/desync_${DESYNC_VERSION}_linux_amd64.tar.gz \
    && curl -fsSLO https://github.com/folbricht/desync/releases/download/v${DESYNC_VERSION}/checksums.txt \
    && sha256sum -c --ignore-missing checksums.txt \
    && tar -xzf desync_${DESYNC_VERSION}_linux_amd64.tar.gz desync && install -m 0755 desync /usr/local/bin/desync \
    && rm -rf /tmp/*
COPY measure.sh recompress.py /usr/local/bin/
WORKDIR /work
