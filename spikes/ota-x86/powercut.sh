#!/bin/bash
# 8.0's x86 gate on the lab NUC (docs/32#x86-test-image, docs/17 M8): cut the power through the
# Shelly plug at random moments of an update and check the machine always comes back, by itself,
# on the old or the new slot, with both ESPs intact.
#
#   powercut.sh <point> <count>     point: install | set-next | trial
#
# Needs the NUC booted in the A/B layout with 1.0 and a staged 1.2 in /data/fjarr-update/stage/1.2,
# the plug's password in ~/.config/fjarr/shelly-nuc.pass. One CSV line per cut on stdout.
set -u
NUC=${NUC:-192.168.10.177}; PLUG=${PLUG:-192.168.10.189}
PASSF=${PASSF:-$HOME/.config/fjarr/shelly-nuc.pass}
S="ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=4 root@$NUC"
point=${1:?install|set-next|trial}; count=${2:-20}

plug() { curl -s -m 5 --digest -u "admin:$(cat "$PASSF")" "http://$PLUG/rpc/$1"; }
cut() { # off now, back on by the plug's own timer even if we lose contact
  plug 'Switch.Set?id=0&on=false&toggle_after=6' >/dev/null; sleep 1.5
  plug 'Switch.GetStatus?id=0' | grep -q '"output":false' || echo "WARN: plug did not report off" >&2; }
host() { $S hostname 2>/dev/null; }
ver() { $S '. /etc/os-release; echo $IMAGE_VERSION' 2>/dev/null; }
wait_up() { # up to 4 minutes; a machine that does not come back gets one more power cycle
  for i in $(seq 1 80); do h=$(host); [ -n "$h" ] && { $S 'systemctl is-system-running --wait >/dev/null 2>&1; true'; echo "$h"; return 0; }; sleep 3; done
  echo "HANG: no answer after 4 minutes, power cycling" >&2; cut
  for i in $(seq 1 80); do h=$(host); [ -n "$h" ] && { echo "$h"; return 0; }; sleep 3; done
  echo none; return 1; }
reboot_wait() { old=$($S cat /proc/sys/kernel/random/boot_id 2>/dev/null); $S "${1:-systemctl reboot}" >/dev/null 2>&1; sleep 10
  for i in $(seq 1 80); do b=$($S cat /proc/sys/kernel/random/boot_id 2>/dev/null); [ -n "$b" ] && [ "$b" != "$old" ] && break; sleep 3; done
  $S 'systemctl is-system-running --wait >/dev/null 2>&1; true'; }
fsck_esps() { # per ESP: clean | dirty (only the FAT dirty bit: byte 65 of the boot sector, set
  # while the ESP is mounted, so it differs from the backup) | errors:<all other lines>
  $S 'r=; for p in /dev/disk/by-partlabel/esp /dev/disk/by-partlabel/fjarr-rescue; do
        o=$(fsck.vfat -n "$p" 2>&1) && { r="${r}clean "; continue; }
        rest=$(echo "$o" | grep -vE "^fsck\.fat|^/dev/|files, .* clusters|[Dd]irty bit|properly unmounted|Leaving filesystem unchanged|^ *[12]\) |^\[12\?q\]|^\s*$|differences between boot sector and its backup|mostly harmless|^ *65:01/00$|Not automatically fixing")
        if [ -z "$rest" ]; then r="${r}dirty "; else r="${r}errors:$(echo "$rest" | tr "\n" "|") "; fi
      done; echo $r'; }
baseline() { # back to: booted 1.0, 1.2 neither installed nor staged as a slot, ESPs clean
  h=$(wait_up)
  if [ "$h" = fjarr-nuc-rescue ]; then # the loader or ESP failed: repair from rescue, count it
    echo "RESCUE reached" >&2
    $S 'touch /run/lab-ok; fsck.vfat -a /dev/disk/by-partlabel/esp; mkdir -p /mnt/esp; mount /dev/disk/by-partlabel/esp /mnt/esp
        cd /mnt/esp/EFI/Linux && for f in fjarr_1.0+*.efi; do [ -e "$f" ] && mv "$f" fjarr_1.0.efi; done; cd /; umount /mnt/esp' >&2
    reboot_wait; fi
  [ "$(ver)" = 1.2 ] && reboot_wait '/usr/lib/fjarr/applier rollback-now'
  [ "$(ver)" = 1.0 ] || { echo "baseline: booted $(ver), not 1.0" >&2; return 1; }
  $S 'set -e; rm -f /efi/EFI/Linux/fjarr_1.2*.efi /efi/EFI/Linux/.#sysupdate*; rm -f /data/fjarr-test/*
      for d in /dev/disk/by-partlabel/fjarr_*; do n=$(basename "$d"); [ "$n" = fjarr_1.0 ] && continue
        p=$(readlink -f "$d"); sfdisk -q --part-label /dev/nvme0n1 "$(cat /sys/class/block/$(basename $p)/partition)" _empty; done
      partx -u /dev/nvme0n1 2>/dev/null || true; udevadm trigger --action=change --settle /dev/nvme0n1 /dev/nvme0n1p*; sync' ; }
state() { $S '/usr/lib/fjarr/applier state' 2>/dev/null | tr ',' ';'; }

echo "point,cycle,offset_s,came_back_as,version,esps,state"
for c in $(seq 1 "$count"); do
  baseline || { echo "$point,$c,,BASELINE-FAILED,,,"; exit 1; }
  case $point in
  install)
    off=$(( RANDOM % 22 + 1 ))
    $S 'nohup /usr/lib/fjarr/applier install 1.2 /data/fjarr-update/stage/1.2 </dev/null >/data/fjarr-test/install.log 2>&1 &'
    sleep "$off"; cut;;
  set-next)
    off=$(awk -v r=$RANDOM 'BEGIN{printf "%.1f", r%21/10}')
    $S '/usr/lib/fjarr/applier install 1.2 /data/fjarr-update/stage/1.2 >/dev/null 2>&1' || { echo "$point,$c,,INSTALL-FAILED,,,"; exit 1; }
    $S '/usr/lib/fjarr/applier set-next 1.2 >/dev/null'; sleep "$off"; cut;;
  trial)
    off=$(( RANDOM % 33 + 3 ))
    $S '/usr/lib/fjarr/applier install 1.2 /data/fjarr-update/stage/1.2 >/dev/null 2>&1 && /usr/lib/fjarr/applier set-next 1.2 >/dev/null' \
      || { echo "$point,$c,,INSTALL-FAILED,,,"; exit 1; }
    $S 'systemctl reboot' >/dev/null 2>&1; sleep "$off"; cut;;
  esac
  h=$(wait_up); v=$(ver); e=$(fsck_esps); st=$(state)
  echo "$point,$c,$off,$h,$v,$e,$st"
done
# The last cut must leave a machine that still updates: install, try and bless 1.2 cleanly.
baseline && $S '/usr/lib/fjarr/applier install 1.2 /data/fjarr-update/stage/1.2 >/dev/null 2>&1 && /usr/lib/fjarr/applier set-next 1.2 >/dev/null' \
  && reboot_wait && sleep 5 && echo "$point,final,,$(host),$(ver),$(fsck_esps),$(state)"
