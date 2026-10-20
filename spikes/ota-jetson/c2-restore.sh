#!/usr/bin/env bash
# shellcheck disable=SC2016 # the remote scripts take their arguments as $1, $2 there
# c2-restore.sh: phase C2, one proven restore of C1's backup onto the ZED Box Mini (slice 8.0, Q3).
# spec: docs/32-fleet-ota.md#jetson-test-image (phase C; Q3, Q4; rules L1, L2, L4, L7)
#
#   LAB_DESKTOP_OK=1 C2_PERSON_AT_BOX=1 c2-restore.sh
#
# The first write. NVIDIA's tools/backup_restore/l4t_backup_restore.sh -e nvme0n1 -r restores the
# whole NVMe from the backup C1 left on the mini-PC, and, because the backup's index lists QSPI0,
# erases and rewrites the QSPI with it (C2a and C2b together): Q4, so only with a person at the box
# with the recovery buttons (C2_PERSON_AT_BOX=1), the button way into RCM proven (button.sh). L2:
# the plug answers, the box answers on its MAC, the B3 backup verifies, the network rescue serves
# (b6.sh net-up), and the C1 backup matches its own index. Before: the box's fingerprint (the C2
# criteria: nv_boot_control.conf, the SSH host keys, the root PARTUUID, Stereolabs' modules, both
# chains' slot state, the boot entries). Then RCM, the restore, one power cycle, and the stock
# system must come back with a new boot id, started, and the same fingerprint.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=lib/lab.sh
. "$here/lib/lab.sh"

: "${C2_L4T:=/srv/fjarr-flash/l4t-36.4.4/Linux_for_Tegra}"
: "${C2_TMO:=5400}"
: "${C2_BOARD:=p3509-a02-p3767-0000}"
[[ ${C2_PERSON_AT_BOX:-} == 1 ]] || { echo "c2-restore.sh: the QSPI write needs a person at the box (Q4): C2_PERSON_AT_BOX=1" >&2; exit 2; }

lab_init c2-restore $((LAB_RCM_ROUND_WORST + C2_TMO + 2 * LAB_BACK_TMO + 600))
lab_plug_check
lab_desktop_guard
lab_backup_check || lab_die "no verified B3 backup (L2)"
out=$(lab_mpc_root -t 300 'cd "$1/tools/backup_restore/images" || exit 1
awk -F, "NF>=6 && \$6!=\"\" {print \$6\"  \"\$1}" nvpartitionmap.txt | sha256sum -c --quiet || { echo "the backup does not match its index"; exit 1; }
grep -q "^QSPI0.img,qspi0," nvpartitionmap.txt || { echo "no QSPI0 in the index"; exit 1; }
systemctl is-active -q fjarr-net-rescue-dhcp.service || { echo "the network rescue is not serving (b6.sh net-up)"; exit 1; }
echo ok' "$C2_L4T" 2>&1) || lab_die "not ready on the mini-PC: $out"
lab_log "C1 backup matches its index; the network rescue serves (L2)"

fingerprint() { # the C2 criteria, from the stock system
  lab_box_root -t 60 'cat /etc/nv_boot_control.conf
for k in /etc/ssh/ssh_host_*_key.pub; do ssh-keygen -l -f "$k"; done
echo "root PARTUUID $(findmnt -no PARTUUID /)"
echo "modules: $(lsmod | awk "/^sl_/{print \$1}" | sort | tr "\n" " ")"
nvbootctrl dump-slots-info 2>&1
nvbootctrl -t rootfs dump-slots-info 2>&1
efibootmgr 2>&1 | grep -v "^BootCurrent"' 2>&1
}
lab_usb_poll
[[ $LAB_USB_STATE == l4t ]] || lab_die "the mini-PC sees the box as '$LAB_USB_STATE', not L4T"
lab_find_box || lab_die "the box does not answer on its MAC"
lab_box_bootid || lab_die "no boot id: $LAB_SSH_ERR"
before=$LAB_BOOTID
mkdir -p "$LAB_DIR/c2"
fingerprint >"$LAB_DIR/c2/before.txt" || lab_die "could not read the box's fingerprint"
grep -q 'modules: sl_' "$LAB_DIR/c2/before.txt" || lab_log "note: no sl_ modules loaded before the restore"

lab_rcm_enter
lab_rcm_wait || lab_die "no RCM within $LAB_RCM_TMO s (the mini-PC sees '$LAB_USB_STATE')"
lab_log "box in RCM; NVIDIA's restore starts (at most $C2_TMO s): the NVMe, then the QSPI"
lab_lock_check
mkdir -p "$LAB_DIR/logs"
log=$LAB_DIR/logs/c2-restore-$(date -u +%Y%m%dT%H%M%SZ).log
t0=$SECONDS
rc=0
lab_mpc_root -t "$C2_TMO" 'L4T=$1 BOARD=$2
docker run --rm --privileged --net=host -v /dev:/dev \
  -v /run/nvidia_initrd_flash:/run/nvidia_initrd_flash:ro -v "$L4T:$L4T" -w "$L4T" fjarr-l4t-flash \
  ./tools/backup_restore/l4t_backup_restore.sh -e nvme0n1 -r "$BOARD"' "$C2_L4T" "$C2_BOARD" >"$log" 2>&1 || rc=$?
dur=$((SECONDS - t0))
lab_log "restore ended with exit $rc after $dur s; its log: ${log#"$LAB_HOME"/}"
tail -n 15 "$log" >&2
((rc == 0)) || lab_die "NVIDIA's restore failed (exit $rc): the box stays as it is for the person at it (button, then C2 again)"

lab_lock_check
lab_rcm_leave
t1=$SECONDS
while ((SECONDS - t1 < 2 * LAB_BACK_TMO)); do
  if lab_find_box && lab_box_bootid && [[ $LAB_BOOTID != "$before" ]]; then break; fi
  sleep 5
done
[[ -n ${LAB_BOOTID:-} && $LAB_BOOTID != "$before" ]] ||
  lab_die "the stock system did not come back on ssh after the restore (the network rescue serves; then the button)"
back=$((SECONDS - t1))
state=$(lab_wait_ready 300) || true
lab_log "stock system back $back s after the power cycle: boot $LAB_BOOTID, systemd $state"
fingerprint >"$LAB_DIR/c2/after.txt" || lab_die "could not read the box's fingerprint after the restore"
if diff "$LAB_DIR/c2/before.txt" "$LAB_DIR/c2/after.txt" >"$LAB_DIR/c2/diff.txt"; then
  lab_log "C2 done: the fingerprint is the same before and after (restore $dur s, back $back s)"
else
  cat "$LAB_DIR/c2/diff.txt" >&2
  lab_die "the fingerprint changed (L7): .lab/c2/diff.txt"
fi
