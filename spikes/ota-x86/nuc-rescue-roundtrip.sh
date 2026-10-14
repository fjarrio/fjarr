#!/bin/sh
# The 8.0 B2 check on the lab NUC: boot the rescue UKI once by BootNext and back, N times, telling
# the two systems apart by SSH host key. Usage: HERE=spikes/ota-x86 nuc-rescue-roundtrip.sh 1 2 3
# Which system answers on .177: "rescue" (the lab host key), "ubuntu", or "none".
which_sys() { fp=$(ssh-keyscan -T 3 -t ed25519 192.168.10.177 2>/dev/null | ssh-keygen -lf - 2>/dev/null | awk '{print $2}')
  case "$fp" in SHA256:F3cqRCUU*) echo rescue;; SHA256:He4kXJu*) echo ubuntu;; *) echo none;; esac; }
wait_sys() { t0=$(date +%s); sleep 10; for i in $(seq 1 100); do s=$(which_sys); [ "$s" = "$1" ] && { echo "  $1 after $(( $(date +%s)-t0 )) s"; return 0; }; sleep 3; done; echo "  $1 NOT reached (last: $s)"; return 1; }
R="ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=4 root@192.168.10.177"
N="ssh -o BatchMode=yes -o ConnectTimeout=4 robot@192.168.10.177"
for n in "$@"; do
  echo "round trip $n"
  scp -q -o BatchMode=yes "$HERE/reflash-target.sh" robot@192.168.10.177:/tmp/reflash-target.sh || exit 1
  $N 'sudo sh /tmp/reflash-target.sh once /boot/efi | grep BootNext && sudo systemctl reboot' 2>/dev/null
  wait_sys rescue || exit 1
  $R 'touch /run/lab-ok; systemctl reboot' 2>/dev/null
  wait_sys ubuntu || exit 1
done
