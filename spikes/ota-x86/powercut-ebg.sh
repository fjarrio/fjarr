#!/bin/bash
# The power cuts of powercut.sh against the SWUpdate + EFI Boot Guard image (the ADR-0039
# comparison): the same cut points, plus one during the confirmation write. Both ESPs and both EBG
# config partitions are checked after every cut.
#
#   powercut-ebg.sh <point> <count>     point: install | set-next | trial | mark-good
#
# Needs the NUC booted in the A/B layout with 1.0 and a staged 1.2 in /data/fjarr-update/stage/1.2,
# the plug's password in ~/.config/fjarr/shelly-nuc.pass. One CSV line per cut on stdout.
set -u
NUC=${NUC:-192.168.10.177}; PLUG=${PLUG:-192.168.10.189}
PASSF=${PASSF:-$HOME/.config/fjarr/shelly-nuc.pass}
S="ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=4 root@$NUC"
point=${1:?install|set-next|trial|mark-good}; count=${2:-20}

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
  $S 'r=; for p in /dev/disk/by-partlabel/esp /dev/disk/by-partlabel/fjarr-rescue /dev/disk/by-partlabel/fjarr-boot0 /dev/disk/by-partlabel/fjarr-boot1; do
        o=$(fsck.vfat -n "$p" 2>&1) && { r="${r}clean "; continue; }
        rest=$(echo "$o" | grep -vE "^fsck\.fat|^/dev/|files, .* clusters|[Dd]irty bit|properly unmounted|Leaving filesystem unchanged|^ *[12]\) |^\[12\?q\]|^\s*$|differences between boot sector and its backup|mostly harmless|^ *65:01/00$|Not automatically fixing")
        if [ -z "$rest" ]; then r="${r}dirty "; else r="${r}errors:$(echo "$rest" | tr "\n" "|") "; fi
      done; echo $r'; }
baseline() { # back to: booted 1.0 and confirmed, the other slot retired, no test flags
  h=$(wait_up)
  if [ "$h" = fjarr-nuc-rescue ]; then # EBG found no bootable environment: count it, repair from rescue
    echo "RESCUE reached" >&2
    $S 'touch /run/lab-ok; for p in 0 1; do fsck.vfat -a /dev/disk/by-partlabel/fjarr-boot$p; done' >&2
    reboot_wait; fi
  $S 'rm -f /data/fjarr-test/*'
  [ "$(ver)" = 1.2 ] && reboot_wait '/usr/lib/fjarr/applier rollback-now'
  [ "$(ver)" = 1.0 ] || { echo "baseline: booted $(ver), not 1.0" >&2; return 1; }
  $S '/usr/lib/fjarr/applier state' | grep -q '"slot":"a"[^}]*"ustate":0' || { echo "baseline: slot a not confirmed" >&2; return 1; }; }
state() { $S '/usr/lib/fjarr/applier state' 2>/dev/null | tr ',' ';'; }

echo "point,cycle,offset_s,came_back_as,version,esps,state"
for c in $(seq 1 "$count"); do
  baseline || { echo "$point,$c,,BASELINE-FAILED,,,"; exit 1; }
  case $point in
  install)
    off=$(( RANDOM % 25 + 1 ))   # the EBG install takes about 24 s on the NUC
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
  mark-good) # hold the trial open, then cut within 300 ms of the confirmation write
    off=$(awk -v r=$RANDOM 'BEGIN{printf "%.2f", r%31/100}')
    $S 'touch /data/fjarr-test/hold-health; /usr/lib/fjarr/applier install 1.2 /data/fjarr-update/stage/1.2 >/dev/null 2>&1 && /usr/lib/fjarr/applier set-next 1.2 >/dev/null' \
      || { echo "$point,$c,,INSTALL-FAILED,,,"; exit 1; }
    # The held health check keeps startup from finishing, so wait for the trial itself, not for
    # "system running" (reboot_wait would block until the trial deadline reboots it).
    old=$($S cat /proc/sys/kernel/random/boot_id 2>/dev/null); $S 'systemctl reboot' >/dev/null 2>&1; sleep 10
    for i in $(seq 1 80); do b=$($S cat /proc/sys/kernel/random/boot_id 2>/dev/null); [ -n "$b" ] && [ "$b" != "$old" ] && break; sleep 3; done
    for i in $(seq 1 30); do $S '/usr/lib/fjarr/applier is-trial' 2>/dev/null && break; sleep 2; done
    [ "$(ver)" = 1.2 ] || { echo "$point,$c,,TRIAL-NOT-BOOTED,$(ver),,"; continue; }
    $S 'nohup /usr/lib/fjarr/applier mark-good </dev/null >/dev/null 2>&1 &'; sleep "$off"; cut;;
  esac
  h=$(wait_up); v=$(ver); e=$(fsck_esps); st=$(state)
  echo "$point,$c,$off,$h,$v,$e,$st"
done
# The last cut must leave a machine that still updates: install, try and bless 1.2 cleanly.
baseline && $S '/usr/lib/fjarr/applier install 1.2 /data/fjarr-update/stage/1.2 >/dev/null 2>&1 && /usr/lib/fjarr/applier set-next 1.2 >/dev/null' \
  && reboot_wait && sleep 5 && echo "$point,final,,$(host),$(ver),$(fsck_esps),$(state)"
