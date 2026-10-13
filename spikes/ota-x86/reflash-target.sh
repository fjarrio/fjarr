#!/bin/sh
# The target side of reflash.sh: runs as root on the machine being reflashed.
set -eu
entry() { efibootmgr | awk -v l="$1" '$1 ~ /^Boot[0-9A-F]+\*$/ && $2 == l { print substr($1, 5, 4) }' | head -1; }
order_first() { # put the given entries first in BootOrder, keep the rest after them
  rest=$(efibootmgr | sed -n 's/^BootOrder: //p' | tr ',' '\n' | grep -vxF -e "$1" ${2:+-e "$2"} | paste -sd,)
  efibootmgr -q -o "$1${2:+,$2}${rest:+,$rest}"; }
p() { case "$DISK" in *[0-9]) echo "${DISK}p$1";; *) echo "$DISK$1";; esac; }

case "$1" in
once) # once <esp-path>: boot \EFI\fjarr\rescue.efi on the next boot only
  esp=$2; src=$(findmnt -n -o SOURCE "$esp"); disk=/dev/$(lsblk -no PKNAME "$src")
  part=$(cat "/sys/class/block/$(basename "$src")/partition")
  n=$(entry fjarr-rescue-once); [ -n "$n" ] && efibootmgr -q -b "$n" -B
  efibootmgr -q -c -d "$disk" -p "$part" -L fjarr-rescue-once -l '\EFI\fjarr\rescue.efi'
  n=$(entry fjarr-rescue-once)
  # efibootmgr -c puts the new entry first in BootOrder: take it out, keep it only as BootNext.
  efibootmgr -q -o "$(efibootmgr | sed -n 's/^BootOrder: //p' | tr ',' '\n' | grep -vxF "$n" | paste -sd,)"
  efibootmgr -q -n "$n"; efibootmgr | grep -E '^Boot(Next|Order|Current)';;
write) # write <image.raw.zst>: run from the RAM rescue system
  DISK=${DISK:-/dev/nvme0n1}; img=$2
  zstd -dc "$img" | dd of="$DISK" bs=4M oflag=direct status=none
  sfdisk -q --relocate gpt-bak-std "$DISK"; echo ', +' | sfdisk -q --no-reread -N 5 "$DISK"
  partx -u "$DISK" || true; udevadm settle
  e2fsck -fy "$(p 5)" >/dev/null 2>&1 || true; resize2fs "$(p 5)" >/dev/null 2>&1
  mkdir -p /mnt/esp /mnt/resc; mount "$(p 1)" /mnt/esp; mount "$(p 2)" /mnt/resc
  mkdir -p /mnt/resc/EFI/BOOT; cp /run/reflash/fjarr-rescue.efi /mnt/resc/EFI/BOOT/BOOTX64.EFI
  cp /run/reflash/fjarr-rescue.efi /mnt/esp/EFI/Linux/fjarr-rescue.efi
  sync; umount /mnt/resc
  fsck.vfat -n "$(p 2)" >/dev/null
  # Old entries pointing at a partition table that no longer exists go; the firmware's own stay.
  for l in ubuntu Ubuntu fjarr fjarr-rescue-disk fjarr-rescue-once Linux; do
    while n=$(entry "$l") && [ -n "$n" ]; do efibootmgr -q -b "$n" -B; done; done
  efibootmgr -q -c -d "$DISK" -p 2 -L fjarr-rescue-disk -l '\EFI\BOOT\BOOTX64.EFI'
  efibootmgr -q -c -d "$DISK" -p 1 -L fjarr -l '\EFI\systemd\systemd-bootx64.efi'
  order_first "$(entry fjarr)" "$(entry fjarr-rescue-disk)"
  bootctl --esp-path=/mnt/esp --no-variables list --no-pager | grep -E '^ +id:'
  umount /mnt/esp; fsck.vfat -n "$(p 1)" >/dev/null
  efibootmgr -v | grep -E '^Boot(Order|[0-9A-F]+\* fjarr)';;
next-rescue-disk)
  efibootmgr -q -n "$(entry fjarr-rescue-disk)";;
esac
