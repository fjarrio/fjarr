#!/bin/sh
# The 8.0 A/B rehearsal in QEMU (docs/32#x86-test-image): a fresh 1.0 disk, then the applier's
# verbs and the failure cases that need no power cut. Needs out/image (1.0) and out/image-1.2.
set -eu
here=$(cd "$(dirname "$0")" && pwd); cd "$here"
S="ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=5 -p 2222 root@127.0.0.1"
up() { for i in $(seq 1 60); do $S true 2>/dev/null && return 0; sleep 5; done; echo "VM not reachable" >&2; return 1; }
bootid() { $S cat /proc/sys/kernel/random/boot_id; }
reboot_and_wait() { # waits for a NEW boot id, then for the system to settle
  old=$(bootid); $S 'systemctl reboot' 2>/dev/null || true; sleep 10
  for i in $(seq 1 90); do b=$($S cat /proc/sys/kernel/random/boot_id 2>/dev/null || true); [ -n "$b" ] && [ "$b" != "$old" ] && break; sleep 5; done
  $S 'systemctl is-system-running --wait >/dev/null 2>&1; true'; }
wait_new_boot() { old=$1; for i in $(seq 1 120); do b=$($S cat /proc/sys/kernel/random/boot_id 2>/dev/null || true); [ -n "$b" ] && [ "$b" != "$old" ] && { $S 'systemctl is-system-running --wait >/dev/null 2>&1; true'; return 0; }; sleep 5; done; return 1; }
state() { $S /usr/lib/fjarr/applier state; }
check() { got=$($S '. /etc/os-release; echo $IMAGE_VERSION'); [ "$got" = "$2" ] && echo "PASS $1: booted $got" || { echo "FAIL $1: booted $got, expected $2"; state; exit 1; }; }

step=${1:-all}
if [ "$step" = all ] || [ "$step" = fresh ]; then
  docker rm -f ota-qemu >/dev/null 2>&1 || true
  cp --sparse=always out/image/fjarr_1.0.raw out/vm/disk.raw; truncate -s +20G out/vm/disk.raw
  docker run --rm -v "$here/out:/out" fjarr-ota-build cp /usr/share/OVMF/OVMF_VARS_4M.fd /out/vm/vars.fd
  docker run --rm --privileged -v /dev:/dev -v "$here/out/vm:/vm" fjarr-ota-build sh -c '
    sfdisk -q --relocate gpt-bak-std /vm/disk.raw && echo ", +" | sfdisk -q -N 5 /vm/disk.raw
    L=$(losetup -fP --show /vm/disk.raw); e2fsck -fy ${L}p5 >/dev/null 2>&1; resize2fs ${L}p5 >/dev/null 2>&1; losetup -d $L'
  ./qemu-disk.sh >/dev/null; up; check fresh 1.0
  $S 'mkdir -p /data/fjarr-update/stage/1.2'
  scp -q -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -P 2222 \
    out/image-1.2/fjarr_1.2.root-x86-64.raw.zst out/image-1.2/fjarr_1.2.efi root@127.0.0.1:/data/fjarr-update/stage/1.2/
fi
if [ "$step" = all ] || [ "$step" = ab ]; then
  $S '/usr/lib/fjarr/applier install 1.2 /data/fjarr-update/stage/1.2 | tail -1'; state
  reboot_and_wait; check "installed but not activated" 1.0
  $S '/usr/lib/fjarr/applier set-next 1.2'; reboot_and_wait; check "set-next" 1.2
  sleep 5; state
  $S '/usr/lib/fjarr/applier state' | grep -q '"version":"1.2","tries_left":null' && echo "PASS blessed: 1.2 has no counter" || { echo "FAIL blessed"; exit 1; }
  b=$(bootid); $S '/usr/lib/fjarr/applier rollback-now' 2>/dev/null || true; wait_new_boot "$b"; check rollback-now 1.0; state
fi
if [ "$step" = all ] || [ "$step" = health ]; then
  $S 'touch /data/fjarr-test/fail-health; /usr/lib/fjarr/applier set-next 1.2'
  b=$(bootid); $S 'systemctl reboot' 2>/dev/null || true; sleep 15; wait_new_boot "$b"
  # the trial boot of 1.2 fails its health check, marks itself bad and reboots into 1.0
  for i in $(seq 1 60); do v=$($S '. /etc/os-release; echo $IMAGE_VERSION' 2>/dev/null || true); [ "$v" = 1.0 ] && break; sleep 5; done
  check "failed health check rolls back" 1.0; state
  $S 'rm -f /data/fjarr-test/fail-health; journalctl --list-boots --no-pager | tail -3'
fi
if [ "$step" = all ] || [ "$step" = panic ]; then
  $S 'touch /data/fjarr-test/panic; /usr/lib/fjarr/applier set-next 1.2'
  b=$(bootid); $S 'systemctl reboot' 2>/dev/null || true; sleep 15; wait_new_boot "$b"
  # first trial panics (panic=10), the second trial has no flag and is blessed
  for i in $(seq 1 60); do v=$($S '. /etc/os-release; echo $IMAGE_VERSION' 2>/dev/null || true); [ "$v" = 1.2 ] && $S '/usr/lib/fjarr/applier state' | grep -q '"version":"1.2","tries_left":null' && break; sleep 5; done
  check "panic on trial boot costs one try" 1.2; state
  $S 'journalctl --list-boots --no-pager | tail -4'
fi
if [ "$step" = all ] || [ "$step" = badroot ]; then
  # A UKI whose root partition does not exist: the initrd waits 90 s, its emergency mode reboots,
  # three tries are used up and the machine comes back on the old version by itself.
  scp -q -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -P 2222 \
    out/bad/fjarr_9.9+3-0.efi root@127.0.0.1:/efi/EFI/Linux/
  want=$($S '. /etc/os-release; echo $IMAGE_VERSION'); t0=$(date +%s)
  b=$(bootid); $S 'systemctl reboot' 2>/dev/null || true; sleep 30
  for i in $(seq 1 120); do
    b2=$($S cat /proc/sys/kernel/random/boot_id 2>/dev/null || true)
    [ -n "$b2" ] && [ "$b2" != "$b" ] && break; sleep 10; done
  echo "back after $(( $(date +%s) - t0 )) s"
  check "missing root uses up its tries" "$want"; state
  $S '/usr/lib/fjarr/applier state' | grep -q '"version":"9.9","tries_left":0,"tries_done":3' \
    && echo "PASS 9.9 exhausted (+0-3)" || echo "FAIL 9.9 not exhausted"
fi
if [ "$step" = all ] || [ "$step" = rescue ]; then
  # Every slot exhausted: systemd-boot falls to the rescue UKI, which sorts after healthy slots.
  scp -q -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -P 2222 \
    out/rescue/fjarr-rescue.efi root@127.0.0.1:/efi/EFI/Linux/fjarr-rescue.efi
  $S 'bootctl list --no-pager | grep -E "^ +id:"'
  $S 'cd /efi/EFI/Linux && for f in fjarr_*.efi; do case $f in *+*) ;; *) mv "$f" "${f%.efi}+0-3.efi";; esac; done; ls'
  b=$(bootid); $S 'systemctl reboot' 2>/dev/null || true; wait_new_boot "$b"
  h=$($S hostname); [ "$h" = fjarr-nuc-rescue ] && echo "PASS all slots exhausted boots rescue" || echo "FAIL booted $h"
  $S 'touch /run/lab-ok; mkdir -p /mnt/esp && mount /dev/disk/by-partlabel/esp /mnt/esp && cd /mnt/esp/EFI/Linux && mv fjarr_1.0+0-3.efi fjarr_1.0.efi && ls && cd / && umount /mnt/esp && systemctl reboot' 2>/dev/null || true
  sleep 20; up; check "rescue repairs the ESP and boots 1.0 again" 1.0
fi
