#!/bin/sh
# Rewrite a lab machine's disk over the network (docs/32#x86-test-image, "Reflashing the lab
# machine"). Run from the operator's machine; every step is over SSH as root.
#
#   reflash.sh rescue <ssh-target>                 boot the rescue UKI once (BootNext)
#   reflash.sh write  <ssh-target> <image.raw.zst> write the image from the RAM rescue system
#   reflash.sh verify <ssh-target>                 boot the on-disk rescue once, then slot A
#
# SSH_OPTS and DISK (default /dev/nvme0n1) can be set in the environment.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
RESCUE=${RESCUE:-$here/out/rescue/fjarr-rescue.efi}
DISK=${DISK:-/dev/nvme0n1}
cmd=${1:?rescue|write|verify}; target=${2:?ssh target}
ssh_() { ssh ${SSH_OPTS:-} "$target" "$@"; }
scp_() { scp -q ${SSH_OPTS:-} "$@"; }
bootid() { ssh_ cat /proc/sys/kernel/random/boot_id; }
wait_boot() { # a new boot id within 15 minutes
  for i in $(seq 1 90); do b=$(ssh_ cat /proc/sys/kernel/random/boot_id 2>/dev/null || true)
    [ -n "$b" ] && [ "$b" != "$1" ] && return 0; sleep 10; done
  echo "no new boot after 15 minutes: the machine needs a power cycle" >&2; exit 1; }
p() { case "$DISK" in *[0-9]) echo "${DISK}p$1";; *) echo "$DISK$1";; esac; }

helper() { ssh_ 'mkdir -p /run/reflash'; scp_ "$here/reflash-target.sh" "$target:/run/reflash/target.sh"; }
case "$cmd" in
rescue)
  # The current system copies the rescue UKI to its own ESP and boots it ONCE: BootNext is
  # consumed by the firmware, so any failure lands back in the current system.
  helper
  esp=$(ssh_ 'bootctl --print-esp-path 2>/dev/null || findmnt -n -o TARGET -t vfat | head -1')
  ssh_ "mkdir -p $esp/EFI/fjarr"; scp_ "$RESCUE" "$target:$esp/EFI/fjarr/rescue.efi"
  ssh_ "sh /run/reflash/target.sh once $esp"
  b=$(bootid); ssh_ 'systemctl reboot' || true; sleep 10; wait_boot "$b"
  h=$(ssh_ hostname); [ "$h" = fjarr-nuc-rescue ] || { echo "booted $h, not the rescue system" >&2; exit 1; }
  ssh_ 'touch /run/lab-ok'; echo "in the RAM rescue system; dead-man disarmed";;
write)
  img=${3:?image.raw.zst}
  [ "$(ssh_ hostname)" = fjarr-nuc-rescue ] || { echo "not in the rescue system" >&2; exit 1; }
  # /run is a fifth of RAM; the compressed image and the rescue UKI get their own tmpfs.
  ssh_ 'touch /run/lab-ok; mkdir -p /run/reflash; mountpoint -q /run/reflash || mount -t tmpfs -o size=75% reflash /run/reflash'
  helper
  scp_ "$img" "$RESCUE" "$target:/run/reflash/"
  want=$(sha256sum < "$img" | cut -d' ' -f1); got=$(ssh_ "sha256sum < /run/reflash/$(basename "$img") | cut -d' ' -f1")
  [ "$want" = "$got" ] || { echo "image checksum mismatch" >&2; exit 1; }
  ssh_ "DISK=$DISK sh /run/reflash/target.sh write /run/reflash/$(basename "$img")"
  echo "written and checked; run: reflash.sh verify $target";;
verify)
  # Boot the on-disk rescue once (proves the firmware's second entry), then slot A.
  helper; ssh_ 'sh /run/reflash/target.sh next-rescue-disk'
  b=$(bootid); ssh_ 'systemctl reboot' || true; sleep 10; wait_boot "$b"
  [ "$(ssh_ hostname)" = fjarr-nuc-rescue ] && echo "on-disk rescue boots" || { echo "on-disk rescue did not boot" >&2; exit 1; }
  b=$(bootid); ssh_ 'systemctl reboot' || true; sleep 10; wait_boot "$b"
  ssh_ '. /etc/os-release; echo "booted $IMAGE_ID $IMAGE_VERSION"; findmnt -n -o SOURCE,TARGET / /data; bootctl list --no-pager | grep -E "^ +id:"';;
*) sed -n '2,10p' "$0" >&2; exit 2;;
esac
