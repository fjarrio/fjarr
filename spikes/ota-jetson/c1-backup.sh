#!/usr/bin/env bash
# shellcheck disable=SC2016 # the remote scripts take their arguments as $1, $2 there
# c1-backup.sh: phase C1, NVIDIA's full backup of the ZED Box Mini from the mini-PC (slice 8.0).
# spec: docs/32-fleet-ota.md#jetson-test-image (phase C; Q3; rules L1, L2, L7)
#
#   LAB_DESKTOP_OK=1 c1-backup.sh
#
# Writes nothing to the box. Under the lab lock: the mini-PC is set up (flash/host-setup.sh: the
# tree exported, the fjarr-l4t-flash image loaded) and holds no earlier backup in the tree; the box
# runs the stock system; it enters RCM by software (lib/lab.sh); NVIDIA's
# tools/backup_restore/l4t_backup_restore.sh -e nvme0n1 -b p3509-a02-p3767-0000 runs in the
# container with the host's network. It boots the box's flash initrd over RCM, which reads the GPT,
# every partition (APP as a zstd tar of its files, the rest with dd), and QSPI0 into the tree's
# tools/backup_restore/images/ over NFS. Then one power cycle back to the stock system, which must
# answer with a new boot id, started. The images stay on the mini-PC (C2 restores from them);
# c1-backup.sh pull copies them to .lab/factory-36.4.4/ on the workstation, the second copy.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=lib/lab.sh
. "$here/lib/lab.sh"

: "${C1_L4T:=/srv/fjarr-flash/l4t-36.4.4/Linux_for_Tegra}"
: "${C1_TMO:=5400}" # the backup itself, at most
: "${C1_BOARD:=p3509-a02-p3767-0000}"
images=$C1_L4T/tools/backup_restore/images
dest=$LAB_DIR/factory-36.4.4

if [[ ${1:-} == pull ]]; then
  lab_init c1-pull 3600 nolock
  mkdir -p "$dest"
  chmod 700 "$dest"
  lab_log "copying $LAB_MINIPC:$images to $dest"
  # shellcheck disable=SC2029 # the path is expanded here on purpose
  ssh "${LAB_SSH_MPC[@]}" "$LAB_MINIPC" "sudo -n tar -C '$images' --sparse -cf - . | zstd -q -T4 -1" |
    zstd -dq | tar -C "$dest" --sparse -xf -
  # The files only: the tool leaves a tmp/ directory beside them.
  (cd "$dest" && find . -maxdepth 1 -type f ! -name 'SHA256SUMS.*' -printf '%P\0' | sort -z | xargs -0 sha256sum >SHA256SUMS.local)
  # shellcheck disable=SC2029
  ssh "${LAB_SSH_MPC[@]}" "$LAB_MINIPC" "cd '$images' && sudo -n find . -maxdepth 1 -type f -printf '%P\0' | sort -z | sudo -n xargs -0 sha256sum" >"$dest/SHA256SUMS.minipc"
  diff <(sort -k2 "$dest/SHA256SUMS.minipc") <(sort -k2 "$dest/SHA256SUMS.local") ||
    lab_die "the copy differs from the mini-PC's"
  lab_log "copied and checked: $(wc -l <"$dest/SHA256SUMS.local") files, $(du -sh "$dest" | cut -f1)"
  exit 0
fi

lab_init c1-backup $((LAB_RCM_ROUND_WORST + C1_TMO + 2 * LAB_BACK_TMO + 600))
lab_plug_check
lab_desktop_guard
out=$(lab_mpc_root -t 60 'L4T=$1
showmount -e localhost | awk -v p="$L4T/tools/backup_restore" "\$1 == p {f = 1} END {exit !f}" || { echo "not exported: $L4T/tools/backup_restore"; exit 1; }
[ -e /run/nvidia_initrd_flash/docker_host_network ] || { echo "no docker_host_network marker"; exit 1; }
docker image inspect fjarr-l4t-flash >/dev/null 2>&1 || { echo "no fjarr-l4t-flash image"; exit 1; }
if [ -d "$L4T/tools/backup_restore/images" ] && [ -n "$(ls -A "$L4T/tools/backup_restore/images")" ]; then
  echo "an earlier backup is in $L4T/tools/backup_restore/images: move it first"; exit 1
fi
df -h --output=avail "$L4T" | tail -1' "$C1_L4T" 2>&1) || lab_die "the mini-PC is not ready: $out"
lab_log "mini-PC ready; free there: $out"
lab_usb_poll
[[ $LAB_USB_STATE == l4t ]] || lab_die "the mini-PC sees the box as '$LAB_USB_STATE', not L4T"
lab_find_box || lab_die "the box does not answer on its MAC"
lab_box_bootid || lab_die "no boot id: $LAB_SSH_ERR"
before=$LAB_BOOTID

lab_rcm_enter
lab_rcm_wait || lab_die "no RCM within $LAB_RCM_TMO s (the mini-PC sees '$LAB_USB_STATE')"
lab_log "box in RCM; NVIDIA's backup starts (at most $C1_TMO s)"
lab_lock_check
mkdir -p "$LAB_DIR/logs"
log=$LAB_DIR/logs/c1-backup-$(date -u +%Y%m%dT%H%M%SZ).log
t0=$SECONDS
rc=0
lab_mpc_root -t "$C1_TMO" 'L4T=$1 BOARD=$2
docker run --rm --privileged --net=host -v /dev:/dev \
  -v /run/nvidia_initrd_flash:/run/nvidia_initrd_flash:ro -v "$L4T:$L4T" -w "$L4T" fjarr-l4t-flash \
  ./tools/backup_restore/l4t_backup_restore.sh -e nvme0n1 -b "$BOARD"' "$C1_L4T" "$C1_BOARD" >"$log" 2>&1 || rc=$?
lab_log "backup ended with exit $rc after $((SECONDS - t0)) s; its log: ${log#"$LAB_HOME"/}"
tail -n 15 "$log" >&2
listing=$(lab_mpc_root -t 60 'cd "$1/tools/backup_restore/images" && ls -la && du -sh . && sha256sum nvpartitionmap.txt' "$C1_L4T" 2>&1) || true
printf '%s\n' "$listing" >>"$log"

lab_lock_check
lab_rcm_leave
t1=$SECONDS
while ((SECONDS - t1 < 2 * LAB_BACK_TMO)); do
  if lab_find_box && lab_box_bootid && [[ $LAB_BOOTID != "$before" ]]; then break; fi
  sleep 5
done
[[ -n ${LAB_BOOTID:-} && $LAB_BOOTID != "$before" ]] || lab_die "the stock system did not come back on ssh after the backup"
state=$(lab_wait_ready 300) || true
lab_log "stock system back: boot $LAB_BOOTID, systemd $state"
((rc == 0)) || lab_die "NVIDIA's backup failed (exit $rc): see ${log#"$LAB_HOME"/}"
grep -q 'nvpartitionmap.txt' <<<"$listing" || lab_die "no nvpartitionmap.txt in the backup"
lab_log "C1 done: $(grep -E '^[0-9.]+[KMGT]?\s+\.$' <<<"$listing" | cut -f1) in $C1_L4T/tools/backup_restore/images; next: c1-backup.sh pull"
