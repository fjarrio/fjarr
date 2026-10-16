# The x86 OTA image builder (M8 slice 8.0, docs/32#x86-test-image): mkosi and the systemd
# image tools from Ubuntu 26.04, plus QEMU and OVMF to rehearse an image before any real machine
# boots it. Run from the host with `docker run --privileged` (mkosi needs namespaces and loop
# devices), never from inside `dev`.
FROM ubuntu:26.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    mkosi systemd-ukify systemd-repart systemd-boot-efi systemd-container \
    ubuntu-keyring debootstrap apt dpkg-dev zstd xz-utils cpio \
    dosfstools e2fsprogs mtools gdisk fdisk util-linux kmod \
    ovmf qemu-system-x86 qemu-utils swtpm openssh-client \
    python3 ca-certificates curl openssl \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
