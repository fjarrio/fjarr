#!/usr/bin/env bash
# shellcheck disable=SC2016 # the remote scripts take their arguments as $1, $2 there
# d1-flash.sh: phase D1, the first Fjarr flash of the ZED Box Mini (slice 8.0).
# spec: docs/32-fleet-ota.md#jetson-test-image (phase D; rules L1, L2, L3, L4, L7; Q4)
#
#   LAB_DESKTOP_OK=1 D1_PERSON_AT_BOX=1 d1-flash.sh [<the image's .tegraflash.tar.gz>]
#
# The first write of Fjarr's firmware and image: meta-tegra's initrd-flash, whole, run natively on
# the mini-PC (it mounts the box's disks through the host's udisks; flash/host-setup.sh installs
# its tools), writes the QSPI (both chains) and the NVMe (both slots, the ESP, the rescue in
# recovery and recovery-dtb). The QSPI write needs Q4: a person at the box with the recovery
# buttons (D1_PERSON_AT_BOX=1). L4: only a package D0 booted from RAM as built (.lab/d0/passed).
# L2: the plug answers, the box answers on its MAC, the B3 backup verifies, the network rescue
# serves, and C1's backup on the mini-PC matches its index (C2 restores the stock box from it).
# L3: the image carries the lab dead-man (no /etc/fjarr/NOT-FOR-THE-BOX), and the Fjarr image
# passes one RCM round trip before the run ends. The box's console goes to .lab/logs/ throughout.
#
# Before: the stock box's fingerprint (.lab/d1/before.txt). After the flash, the Fjarr image must
# answer on the box's MAC as root with the image's host key, its slot state, the dead-man, the
# kernel's panic=10 and the rescue in recovery go to .lab/d1/after.txt; then reboot
# forced-recovery, RCM, one power cycle, and the Fjarr image back with a new boot id.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=lib/lab.sh
. "$here/lib/lab.sh"

deploy=$LAB_HOME/out/yocto/build/tmp/deploy/images/zedbox-mini-orin-nx16
pkg=${1:-$deploy/fjarr-spike-image-zedbox-mini-orin-nx16.rootfs.tegraflash.tar.gz}
: "${D0_DIR:=/srv/fjarr-flash/d0}" # the package unpacked by D0, reused
: "${D1_TMO:=3600}"                # the whole initrd-flash, at most
: "${D1_FIRST_BOOT_S:=900}"        # from the flash's end to the Fjarr image on ssh
: "${C1_L4T:=/srv/fjarr-flash/l4t-36.4.4/Linux_for_Tegra}"
[[ ${D1_PERSON_AT_BOX:-} == 1 ]] || { echo "d1-flash.sh: the QSPI write needs a person at the box (Q4): D1_PERSON_AT_BOX=1" >&2; exit 2; }
[[ -s $pkg ]] || { echo "d1-flash.sh: no flash package $pkg" >&2; exit 2; }
sha=$(sha256sum <"$(readlink -f "$pkg")" | cut -d' ' -f1)
grep -q "^$sha " "$LAB_DIR/d0/passed" 2>/dev/null ||
  { echo "d1-flash.sh: package ${sha:0:16} has not passed D0 as built (L4): run d0-rcmboot.sh on it first" >&2; exit 2; }

mkdir -p "$LAB_DIR/d1"
lab_init d1-flash $((600 + LAB_RCM_ROUND_WORST + D1_TMO + D1_FIRST_BOOT_S + LAB_RCM_ROUND_WORST + 600))
lab_plug_check
lab_desktop_guard
lab_backup_check || lab_die "no verified B3 backup (L2)"
lab_log "flash package ${pkg##*/}, sha256 ${sha:0:16}, passed D0: $(grep "^$sha " "$LAB_DIR/d0/passed" | tail -n 1 | cut -d' ' -f2,3)"

# The mini-PC: the network rescue, C1's backup (the way back), the native flasher's tools and the
# rule that keeps the desktop off the box's disks.
out=$(lab_mpc_root -t 300 'cd "$1/tools/backup_restore/images" || { echo "no C1 backup in $1"; exit 1; }
awk -F, "NF>=6 && \$6!=\"\" {print \$6\"  \"\$1}" nvpartitionmap.txt | sha256sum -c --quiet || { echo "the C1 backup does not match its index"; exit 1; }
systemctl is-active -q fjarr-net-rescue-dhcp.service || { echo "the network rescue is not serving (b6.sh net-up)"; exit 1; }
systemctl is-active -q udisks2 || { echo "udisks2 is not running"; exit 1; }
for t in dtc bmaptool udisksctl sgdisk debugfs; do command -v $t >/dev/null || { echo "no $t (flash/host-setup.sh)"; exit 1; }; done
grep -q "Linux for Tegra\", ENV{UDISKS_AUTO}=\"0\"" /etc/udev/rules.d/70-fjarr-box.rules || { echo "no 70-fjarr-box.rules (flash/host-setup.sh)"; exit 1; }
echo ok' "$C1_L4T" 2>&1) || lab_die "not ready on the mini-PC: $out"
lab_log "L2 on the mini-PC: the network rescue serves, C1's backup matches its index, the native flasher is set up"

# The package, unpacked once per build (as D0 does), and a fresh copy to flash from.
lab_mpc_root -t 60 'install -d -m 0755 "$1"; [ "$(cat "$1/pkg.sha256" 2>/dev/null)" = "$2" ]' "$D0_DIR" "$sha" ||
  {
    lab_log "copying the flash package to the mini-PC"
    # shellcheck disable=SC2029
    ssh "${LAB_SSH_MPC[@]}" "$LAB_MINIPC" "sudo -n rm -rf '$D0_DIR/pkg' && sudo -n install -d '$D0_DIR/pkg' && sudo -n tar -C '$D0_DIR/pkg' -xzf - && echo '$sha' | sudo -n tee '$D0_DIR/pkg.sha256' >/dev/null" <"$(readlink -f "$pkg")"
  }
out=$(lab_mpc_root -t 300 'rm -rf "$1/d1" && cp -a "$1/pkg" "$1/d1" && cd "$1/d1" || exit 1
slot=$(ls ./*.ext4 | head -n 1)
[ -f "$slot" ] || { echo "no slot image"; exit 1; }
debugfs -R "stat /etc/fjarr/NOT-FOR-THE-BOX" "$slot" 2>&1 | grep -q "File not found" || { echo "$slot carries /etc/fjarr/NOT-FOR-THE-BOX (L3)"; exit 1; }
debugfs -R "stat /usr/lib/systemd/system/fjarr-lab-deadman.service" "$slot" 2>&1 | grep -q "Inode:" || { echo "$slot has no lab dead-man (L3)"; exit 1; }
[ "$(head -c 8 fjarr-recovery-rescue.img)" = "ANDROID!" ] || { echo "no recovery rescue in the package"; exit 1; }
grep -q "fjarr-recovery-rescue.img" external-flash.xml.in || { echo "the layout does not write the recovery rescue"; exit 1; }
grep -lE "cvb_eeprom_read_size = <0x0+>" ./*mb2-bct-misc*zedbox-mini*.dts* >/dev/null 2>&1 || { echo "the package lacks the MB2 configuration of Fjarr"; exit 1; }
echo "slot ${slot#./}, recovery $(sha256sum fjarr-recovery-rescue.img | cut -c1-16)"' "$D0_DIR" 2>&1) ||
  lab_die "the package is not for the box: $out"
lab_log "package checked: ${out##*$'\n'}; the dead-man in the slot, no NOT-FOR-THE-BOX (L3)"
read -r rec_sha rec_size < <(lab_mpc_root -t 60 'printf "%s %s\n" "$(sha256sum <"$1/d1/fjarr-recovery-rescue.img" | cut -d" " -f1)" "$(stat -c %s "$1/d1/fjarr-recovery-rescue.img")"' "$D0_DIR")
[[ $rec_sha =~ ^[0-9a-f]{64}$ && $rec_size =~ ^[0-9]+$ ]] || lab_die "could not read the rescue's checksum on the mini-PC"

lab_usb_poll
[[ $LAB_USB_STATE == l4t ]] || lab_die "the mini-PC sees the box as '$LAB_USB_STATE', not the stock system's L4T"
lab_find_box || lab_die "the box does not answer on its MAC (L2)"
lab_box_bootid || lab_die "no boot id: $LAB_SSH_ERR"
lab_box_root -t 60 'cat /etc/nv_boot_control.conf; for k in /etc/ssh/ssh_host_*_key.pub; do ssh-keygen -l -f "$k"; done
echo "root PARTUUID $(findmnt -no PARTUUID /)"; nvbootctrl dump-slots-info 2>&1; nvbootctrl -t rootfs dump-slots-info 2>&1
efibootmgr 2>&1 | grep -v "^BootCurrent"' >"$LAB_DIR/d1/before.txt" 2>&1 || lab_die "could not read the stock box's fingerprint"

lab_rcm_enter
lab_rcm_wait || lab_die "no RCM within $LAB_RCM_TMO s (the mini-PC sees '$LAB_USB_STATE')"
lab_uart_start d1
lab_need "$D1_TMO" "meta-tegra's initrd-flash"
lab_lock_check
lab_log "box in RCM; meta-tegra's initrd-flash writes the QSPI and the NVMe now (at most $D1_TMO s)"
log=$LAB_DIR/logs/d1-flash-$(date -u +%Y%m%dT%H%M%SZ).log
t0=$SECONDS rc=0
lab_mpc_root -t "$D1_TMO" 'cd "$1/d1" && ./initrd-flash; rc=$?; cat "$(ls -t log.initrd-flash.* | head -n 1)"; exit $rc' "$D0_DIR" >"$log" 2>&1 || rc=$?
dur=$((SECONDS - t0))
lab_log "initrd-flash ended with exit $rc after $dur s; its log: ${log#"$LAB_HOME"/}"
grep -E '^== Step|ERR|Successfully finished|Error' "$log" >&2 || true
if ((rc != 0)); then
  lab_uart_tail 30 >&2 || true
  lab_die "initrd-flash failed (exit $rc): the box stays as it is for the person at it (the console log, then c2-restore.sh)"
fi

# The Fjarr image: root, its own host key, no USB device while it runs.
lab_use_fjarr_image
t1=$SECONDS
while ((SECONDS - t1 < D1_FIRST_BOOT_S)); do
  if lab_find_box && lab_box_bootid; then break; fi
  lab_host_key_failed && lab_die "something answered on the box's MAC with another host key (L7): $LAB_SSH_ERR"
  sleep 10
done
[[ -n ${LAB_BOOTID:-} ]] || {
  lab_uart_tail 40 >&2 || true
  lab_die "the Fjarr image did not answer ssh within $D1_FIRST_BOOT_S s of the flash (its dead-man enters RCM after 20 min)"
}
first=$LAB_BOOTID
lab_deadman_touch || true
state=$(lab_wait_ready 300) || true
lab_log "the Fjarr image answers $((SECONDS - t1)) s after the flash: boot $first, systemd $state"
lab_box_root -t 120 'rec_sha=$1 rec_size=$2
. /etc/os-release; echo "os-release: $ID $IMAGE_ID $IMAGE_VERSION"
echo "cmdline: $(cat /proc/cmdline)"
echo "dead-man: $(systemctl is-active fjarr-lab-deadman.service)"
[ -e /etc/fjarr/NOT-FOR-THE-BOX ] && echo "NOT-FOR-THE-BOX present"
echo "watchdog: $(systemctl show -p RuntimeWatchdogUSec --value)"
echo "failed units: $(systemctl --failed --no-legend | wc -l)"
echo "/data: $(findmnt -no SOURCE,FSTYPE /data 2>/dev/null || echo not mounted)"
nvbootctrl dump-slots-info 2>&1; nvbootctrl -t rootfs dump-slots-info 2>&1
r=$(head -c "$rec_size" /dev/disk/by-partlabel/recovery | sha256sum | cut -d" " -f1)
[ "$r" = "$rec_sha" ] && echo "recovery: the package rescue" || echo "recovery: DIFFERS ($r)"
efibootmgr 2>&1' "$rec_sha" "$rec_size" >"$LAB_DIR/d1/after.txt" 2>&1 || lab_die "could not read the Fjarr image's state"
cat "$LAB_DIR/d1/after.txt" >&2
grep -q '^dead-man: active$' "$LAB_DIR/d1/after.txt" || lab_die "the lab dead-man is not running on the Fjarr image (L3)"
grep -q ' panic=10\b' "$LAB_DIR/d1/after.txt" || lab_die "the Fjarr image's kernel has no panic=10 (L3)"
grep -q '^recovery: the package rescue$' "$LAB_DIR/d1/after.txt" || lab_die "recovery does not hold the package's rescue"
! grep -q 'NOT-FOR-THE-BOX' "$LAB_DIR/d1/after.txt" || lab_die "the image on the box is marked NOT-FOR-THE-BOX"

# L3: the Fjarr image's own RCM round trip, before anything relies on it.
lab_deadman_touch || true
lab_rcm_enter
lab_rcm_wait || lab_die "the Fjarr image did not reach RCM within $LAB_RCM_TMO s (the mini-PC sees '$LAB_USB_STATE'; its dead-man retries in 20 min)"
lab_log "RCM from the Fjarr image; leaving it by one power cycle"
lab_lock_check
lab_rcm_leave
t2=$SECONDS
while ((SECONDS - t2 < 2 * LAB_BACK_TMO)); do
  if lab_find_box && lab_box_bootid && [[ $LAB_BOOTID != "$first" ]]; then break; fi
  sleep 5
done
[[ -n ${LAB_BOOTID:-} && $LAB_BOOTID != "$first" ]] || lab_die "the Fjarr image did not come back after the power cycle out of RCM"
lab_deadman_touch || true
state=$(lab_wait_ready 300) || true
lab_box -t 20 'systemctl is-active fjarr-lab-deadman.service' | grep -qx active || lab_die "no dead-man after the round trip (L3)"
lab_log "the Fjarr image back $((SECONDS - t2)) s after the power cycle: systemd $state, the dead-man running"
lab_uart_stop
lab_log "D1 done: Fjarr's firmware and image on the box (flash $dur s), its RCM round trip passed (L3); .lab/d1/"
