#!/bin/sh
# Boot out/vm/disk.raw (an NVMe disk) under OVMF with persistent firmware variables, like a
# real machine (docs/32#x86-test-image rehearsal). SSH on 127.0.0.1:2222, serial log in
# `docker logs ota-qemu`. `qemu-disk.sh new <image.raw>` starts from a fresh copy.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$here/out/vm"
if [ "${1:-}" = new ]; then
  cp --sparse=always "${2:?image}" "$here/out/vm/disk.raw"
  docker run --rm -v "$here/out:/out" fjarr-ota-build cp /usr/share/OVMF/OVMF_VARS_4M.fd /out/vm/vars.fd
fi
docker rm -f ota-qemu >/dev/null 2>&1 || true
exec docker run -d --name ota-qemu --device /dev/kvm -p 127.0.0.1:2222:2222 -v "$here/out/vm:/vm" fjarr-ota-build \
  qemu-system-x86_64 -machine q35,accel=kvm -cpu host -m 4096 -smp 2 -nographic \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
    -drive if=pflash,format=raw,file=/vm/vars.fd \
    -drive if=none,id=d0,format=raw,file=/vm/disk.raw,cache=unsafe -device nvme,drive=d0,serial=fjarr0 \
    -netdev user,id=n0,hostfwd=tcp::2222-:22 -device virtio-net-pci,netdev=n0 \
    -device i6300esb -serial mon:stdio
