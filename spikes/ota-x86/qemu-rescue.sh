#!/bin/sh
# Boot the rescue UKI in QEMU with OVMF (docs/32#x86-test-image rehearsal). SSH on 127.0.0.1:2222.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
docker rm -f ota-qemu >/dev/null 2>&1 || true
exec docker run -d --name ota-qemu --device /dev/kvm -p 127.0.0.1:2222:2222 -v "$here:/src" fjarr-ota-build sh -c '
  cp /usr/share/OVMF/OVMF_VARS_4M.fd /tmp/vars.fd; mkdir -p /tmp/esp/EFI/BOOT
  cp /src/out/rescue/fjarr-rescue.efi /tmp/esp/EFI/BOOT/BOOTX64.EFI
  exec qemu-system-x86_64 -machine q35,accel=kvm -cpu host -m 4096 -smp 2 -nographic \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
    -drive if=pflash,format=raw,file=/tmp/vars.fd -drive format=raw,file=fat:rw:/tmp/esp \
    -netdev user,id=n0,hostfwd=tcp::2222-:22 -device virtio-net-pci,netdev=n0 \
    -device i6300esb -serial mon:stdio'
