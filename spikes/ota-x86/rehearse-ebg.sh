#!/bin/sh
# The SWUpdate + EFI Boot Guard rehearsal in QEMU (the ADR-0039 comparison): the same cases as
# rehearse.sh where they apply, with EBG's own semantics (one try per update), plus the loader-armed
# watchdog and a damaged environment. Needs out/image-ebg (1.0), out/image-ebg-1.2 with its .swu,
# and out/rescue. Usage: rehearse-ebg.sh [fresh|ab|health|panic|badroot|hang|torn|rescue|all]
set -eu
here=$(cd "$(dirname "$0")" && pwd); cd "$here"
C=${VM_SSH_CONF:-/tmp/claude-1000/-home-erik-Code-fjarr/d738621b-945f-4b37-9450-81f00f5d2f44/scratchpad/vm-ssh.conf}
S="ssh -F $C vm"
up() { for i in $(seq 1 60); do $S true 2>/dev/null && return 0; sleep 5; done; echo "VM not reachable" >&2; return 1; }
bootid() { for i in 1 2 3 4 5 6; do $S cat /proc/sys/kernel/random/boot_id 2>/dev/null && return 0; sleep 5; done; return 1; }
wait_new_boot() { old=$1; for i in $(seq 1 120); do b=$($S cat /proc/sys/kernel/random/boot_id 2>/dev/null || true)
  [ -n "$b" ] && [ "$b" != "$old" ] && { $S 'systemctl is-system-running --wait >/dev/null 2>&1; true'; return 0; }; sleep 5; done; return 1; }
reboot_wait() { b=$(bootid); $S "${1:-systemctl reboot}" 2>/dev/null || true; sleep 10; wait_new_boot "$b"; }
ver() { $S '. /etc/os-release; echo $IMAGE_VERSION' 2>/dev/null; }
state() { $S /usr/lib/fjarr/applier state; }
check() { got=$(ver); [ "$got" = "$2" ] && echo "PASS $1: booted $got" || { echo "FAIL $1: booted $got, expected $2"; state; exit 1; }; }
until_ver() { for i in $(seq 1 ${2:-60}); do [ "$(ver)" = "$1" ] && return 0; sleep 5; done; return 1; }
console() { docker logs --since "${1:-5m}" ota-qemu 2>&1 | tr -d '\r' | grep -aE "$2" | tail -${3:-3}; }

step=${1:-all}
if [ "$step" = all ] || [ "$step" = fresh ]; then
  docker rm -f ota-qemu >/dev/null 2>&1 || true
  cp --sparse=always out/image-ebg/fjarr_1.0.raw out/vm/disk.raw; truncate -s +20G out/vm/disk.raw
  docker run --rm -v "$here/out:/out" fjarr-ota-build cp /usr/share/OVMF/OVMF_VARS_4M.fd /out/vm/vars.fd
  docker run --rm --privileged -v /dev:/dev -v "$here/out:/out" fjarr-ota-build sh -c '
    sfdisk -q --relocate gpt-bak-std /out/vm/disk.raw 2>/dev/null && echo ", +" | sfdisk -q -N 7 /out/vm/disk.raw 2>/dev/null
    L=$(losetup -fP --show /out/vm/disk.raw); e2fsck -fy ${L}p7 >/dev/null 2>&1; resize2fs ${L}p7 >/dev/null 2>&1
    mkdir -p /m; mount ${L}p2 /m; mkdir -p /m/EFI/BOOT; cp /out/rescue/fjarr-rescue.efi /m/EFI/BOOT/BOOTX64.EFI; umount /m
    losetup -d $L'
  ./qemu-disk.sh >/dev/null; up; check fresh 1.0
  $S 'mkdir -p /data/fjarr-update/stage/1.2 /data/fjarr-test'
  scp -q -F "$C" out/image-ebg-1.2/fjarr_1.2.swu vm:/data/fjarr-update/stage/1.2/
fi
install12() { $S '/usr/lib/fjarr/applier install 1.2 /data/fjarr-update/stage/1.2 2>&1 | tail -1'; }
if [ "$step" = all ] || [ "$step" = ab ]; then
  install12; state
  reboot_wait; check "installed but not activated" 1.0
  $S '/usr/lib/fjarr/applier set-next 1.2'; reboot_wait; check "set-next" 1.2
  sleep 5; state | grep -q '"booted_slot":"b".*"slot":"b"[^}]*"ustate":0' && echo "PASS confirmed: 1.2's environment is OK" || { echo "FAIL confirm"; state; exit 1; }
  reboot_wait '/usr/lib/fjarr/applier rollback-now'; check rollback-now 1.0; state
fi
if [ "$step" = all ] || [ "$step" = health ]; then
  install12 >/dev/null; $S 'touch /data/fjarr-test/fail-health; /usr/lib/fjarr/applier set-next 1.2'
  b=$(bootid); $S 'systemctl reboot' 2>/dev/null || true; sleep 15; wait_new_boot "$b"
  until_ver 1.0 || true; check "failed health check rolls back" 1.0; state; $S 'rm -f /data/fjarr-test/fail-health'
fi
if [ "$step" = all ] || [ "$step" = panic ]; then
  # One try: a crash on the trial boot rolls back (systemd-boot would have tried twice more).
  install12 >/dev/null; $S 'touch /data/fjarr-test/panic; /usr/lib/fjarr/applier set-next 1.2'
  b=$(bootid); $S 'systemctl reboot' 2>/dev/null || true; sleep 15; wait_new_boot "$b"
  until_ver 1.0 || true; check "panic on the trial boot rolls back (one try)" 1.0; state
fi
if [ "$step" = all ] || [ "$step" = badroot ]; then
  # A UKI whose root never appears, in slot B's boot partition: one try (about two minutes), then 1.0.
  docker run --rm -v "$here/out:/out" fjarr-ota-build sh -c 'cd /tmp; for s in linux initrd osrel; do objcopy -O binary --only-section=.$s /out/image-ebg-1.2/fjarr_1.2.efi $s; done
    sed "s/^IMAGE_VERSION=.*/IMAGE_VERSION=9.9/" osrel > osrel2; mkdir -p /out/bad-ebg
    ukify build --linux=linux --initrd=initrd --os-release=@osrel2 --cmdline="root=LABEL=fjarr_9.9 rootflags=x-systemd.device-timeout=90s rw systemd.gpt_auto=0 panic=10 systemd.watchdog_sec=30 watchdog.handle_boot_enabled=0 console=ttyS0,115200 console=tty0" --output=/out/bad-ebg/fjarr.efi >/dev/null 2>&1; chmod 644 /out/bad-ebg/*'
  scp -q -F "$C" out/bad-ebg/fjarr.efi vm:/tmp/bad.efi
  $S 'set -e; m=$(mktemp -d); mount -o sync /dev/disk/by-partlabel/fjarr-boot1 $m; cp /tmp/bad.efi $m/fjarr.efi
      bg_setenv -f $m/BGENV.DAT -P -x fjarr_version=9.9 >/dev/null; umount $m; /usr/lib/fjarr/applier set-next 9.9'
  t0=$(date +%s); b=$(bootid); $S 'systemctl reboot' 2>/dev/null || true; sleep 30; wait_new_boot "$b"; until_ver 1.0 90 || true
  echo "back after $(( $(date +%s) - t0 )) s"; check "missing root rolls back after one try" 1.0; state
fi
if [ "$step" = all ] || [ "$step" = hang ]; then
  # The loader-armed watchdog: a trial kernel that never starts userspace and has no watchdog driver.
  # Without EBG's own watchdog nothing would reset it; with it, the board resets in about 60 s and
  # EBG marks the trial FAILED.
  docker run --rm -v "$here/out:/out" fjarr-ota-build sh -c 'cd /tmp; for s in linux initrd osrel; do objcopy -O binary --only-section=.$s /out/image-ebg-1.2/fjarr_1.2.efi $s; done
    sed "s/^IMAGE_VERSION=.*/IMAGE_VERSION=9.8/" osrel > osrel2; mkdir -p /out/bad-ebg
    ukify build --linux=linux --initrd=initrd --os-release=@osrel2 --cmdline="root=LABEL=fjarr_none rootflags=x-systemd.device-timeout=infinity rd.emergency=halt panic=0 modprobe.blacklist=wdat_wdt,iTCO_wdt,i6300esb watchdog.handle_boot_enabled=0 systemd.watchdog_sec=0 console=ttyS0,115200" --output=/out/bad-ebg/hang.efi >/dev/null 2>&1; chmod 644 /out/bad-ebg/*'
  scp -q -F "$C" out/bad-ebg/hang.efi vm:/tmp/hang.efi
  install12 >/dev/null
  $S 'set -e; m=$(mktemp -d); mount -o sync /dev/disk/by-partlabel/fjarr-boot1 $m; cp /tmp/hang.efi $m/fjarr.efi
      bg_setenv -f $m/BGENV.DAT -P -x fjarr_version=9.8 >/dev/null; umount $m; /usr/lib/fjarr/applier set-next 9.8'
  t0=$(date +%s); b=$(bootid); $S 'systemctl reboot' 2>/dev/null || true; sleep 30; wait_new_boot "$b"; until_ver 1.0 90 || true
  echo "back after $(( $(date +%s) - t0 )) s"; check "a hung trial kernel is reset by the loader's watchdog" 1.0
  console 10m 'watchdog|Watchdog|Boot caused' 4; state
fi
if [ "$step" = all ] || [ "$step" = torn ]; then
  # A damaged environment on the booted slot's partition: EBG boots the other environment.
  install12 >/dev/null; $S '/usr/lib/fjarr/applier set-next 1.2'; reboot_wait; check "set-next (for torn)" 1.2; sleep 5
  $S 'm=$(mktemp -d); mount -o sync /dev/disk/by-partlabel/fjarr-boot1 $m; truncate -s 1000 $m/BGENV.DAT; umount $m'
  reboot_wait; check "a torn environment falls back to the other slot" 1.0; state
  $S 'm=$(mktemp -d); mount -o sync /dev/disk/by-partlabel/fjarr-boot1 $m; rm -f $m/BGENV.DAT
      bg_setenv -f $m/BGENV.DAT -r 0 -s FAILED -k C:BOOT1:fjarr.efi -w 60 -i 0 -x fjarr_version=none >/dev/null; umount $m'; state
fi
if [ "$step" = all ] || [ "$step" = rescue ]; then
  # No valid environment at all: EBG hands back to the firmware, which boots fjarr-rescue.
  $S 'for p in 0 1; do m=$(mktemp -d); mount -o sync /dev/disk/by-partlabel/fjarr-boot$p $m; mv $m/BGENV.DAT $m/BGENV.OFF; umount $m; done'
  b=$(bootid); $S 'systemctl reboot' 2>/dev/null || true; sleep 15; wait_new_boot "$b"
  h=$($S hostname); [ "$h" = fjarr-nuc-rescue ] && echo "PASS no valid environment boots rescue" || echo "FAIL booted $h"
  $S 'touch /run/lab-ok; for p in 0 1; do m=$(mktemp -d); mount /dev/disk/by-partlabel/fjarr-boot$p $m; mv $m/BGENV.OFF $m/BGENV.DAT; umount $m; done; systemctl reboot' 2>/dev/null || true
  sleep 20; up; check "rescue restores the environments and boots 1.0" 1.0
fi
