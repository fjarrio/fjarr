#!/usr/bin/env bash
# shellcheck disable=SC2016 # the remote scripts take their arguments as $1, $2 there
# d0-rcmboot.sh: phase D0, Fjarr's firmware and meta-tegra's flash initrd booted from RAM over RCM,
# writing nothing (slice 8.0, rule L4).
# spec: docs/32-fleet-ota.md#jetson-test-image (phase D; rules L1, L4, L7)
#
#   LAB_DESKTOP_OK=1 [D0_DTB=<a kernel device tree>] d0-rcmboot.sh [<the image's .tegraflash.tar.gz>]
#
# D0_DTB boots this run with another device tree in place of the package's (the file keeps the
# package's name, so UEFI and the flash kernel both take it): the plan's fallback, NVIDIA's own
# tree for the board, when the flash initrd's USB device does not appear with Fjarr's. Each run
# works on a fresh copy of the unpacked package, so a swapped tree never outlives its run.
#
# D0_SWAP="<package file>=<local file> ..." puts local files in the copy under the package's names
# for one run (a bisection step, such as NVIDIA's own UEFI in place of meta-tegra's build).
#
# D0_FLASHVARS_SED=<a sed expression> edits the copy's flashvars for one run (a bisection step,
# such as leaving one overlay out of BOOTCONTROL_OVERLAYS).
#
# D0_NVIDIA_L4T=<a tree on the mini-PC> boots NVIDIA's own way instead, for comparison:
# tools/kernel_flash/l4t_initrd_flash.sh --initrd --network usb0 p3509-a02-p3767-0000 internal in
# that tree (flash/prepare-l4t.sh, exported by flash/host-setup.sh), as NVIDIA's backup tool boots
# the box (C1). It generates no flash images, boots the flash initrd over RCM, and returns once
# ssh over the USB link answers ("Device has booted into initrd"). It writes nothing either.
#
# Every new firmware build boots from RAM before it is written to the QSPI (L4). Under the lab
# lock: the Yocto image's flash package (the newest in the deploy directory by default) goes to
# the mini-PC; the box, running the stock system, enters RCM by software; meta-tegra's own
# initrd-flash runs in the fjarr-l4t-flash container, cut off right after its "Boot Jetson via
# RCM" step: it signs the package's binaries (no keys: NVIDIA's zero key) and loads Fjarr's MB1
# and MB2 configuration, UEFI, and the flash kernel with Fjarr's device tree and initrd into RAM.
# The flash initrd shows a USB device to the mini-PC and waits for its command package, which
# nobody sends. That device must appear (MB2 got past the carrier EEPROM it no longer reads, and
# UEFI and the kernel ran); nothing mounts or writes it. One power cycle then brings back the
# stock system from the QSPI and the NVMe, and the box's fingerprint must equal the one before.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=lib/lab.sh
. "$here/lib/lab.sh"

deploy=$LAB_HOME/out/yocto/build/tmp/deploy/images/zedbox-mini-orin-nx16
pkg=${1:-$deploy/fjarr-spike-image-zedbox-mini-orin-nx16.rootfs.tegraflash.tar.gz} # the newest build's link
[[ -s $pkg ]] || { echo "d0-rcmboot.sh: no flash package (build the Yocto image first)" >&2; exit 2; }
: "${D0_DIR:=/srv/fjarr-flash/d0}"
: "${D0_GADGET_S:=300}" # from the RCM boot to the flash initrd's USB device
: "${D0_HOLD_S:=30}"    # how long the device must stay
: "${D0_DTB:=}"
: "${D0_NVIDIA_L4T:=}"
: "${D0_FLASHVARS_SED:=}"
: "${D0_SWAP:=}"
[[ -z $D0_DTB || -s $D0_DTB ]] || { echo "d0-rcmboot.sh: no $D0_DTB" >&2; exit 2; }

mkdir -p "$LAB_DIR/d0"
lab_init d0-rcmboot $((600 + LAB_RCM_ROUND_WORST + D0_GADGET_S + D0_HOLD_S + 2 * LAB_BACK_TMO + 300))
lab_plug_check
lab_desktop_guard

fingerprint() {
  lab_box_root -t 60 'cat /etc/nv_boot_control.conf
for k in /etc/ssh/ssh_host_*_key.pub; do ssh-keygen -l -f "$k"; done
echo "root PARTUUID $(findmnt -no PARTUUID /)"
nvbootctrl dump-slots-info 2>&1
nvbootctrl -t rootfs dump-slots-info 2>&1
efibootmgr 2>&1 | grep -v "^BootCurrent"' 2>&1
}

if [[ -n $D0_NVIDIA_L4T ]]; then
  out=$(lab_mpc_root -t 60 'L4T=$1
[ -x "$L4T/tools/kernel_flash/l4t_initrd_flash.sh" ] || { echo "no $L4T"; exit 1; }
showmount -e localhost | awk -v p="$L4T/rootfs" "\$1 == p {f = 1} END {exit !f}" || { echo "$L4T is not exported (flash/host-setup.sh)"; exit 1; }
head -n 2 "$L4T/../fjarr-prepare.txt"' "$D0_NVIDIA_L4T" 2>&1) || lab_die "the mini-PC is not ready: $out"
  lab_log "NVIDIA's way, in $D0_NVIDIA_L4T: ${out//$'\n'/; }"
else
sha=$(sha256sum <"$pkg" | cut -d' ' -f1)
lab_log "flash package ${pkg##*/}, sha256 ${sha:0:16}"
# The package, unpacked on the mini-PC once per build; initrd-flash-d0 is meta-tegra's script up to
# its RCM boot, and stops there.
lab_mpc_root -t 60 'install -d -m 0755 "$1"; [ "$(cat "$1/pkg.sha256" 2>/dev/null)" = "$2" ]' "$D0_DIR" "$sha" ||
  {
    lab_log "copying the flash package to the mini-PC"
    # shellcheck disable=SC2029
    ssh "${LAB_SSH_MPC[@]}" "$LAB_MINIPC" "sudo -n rm -rf '$D0_DIR/pkg' && sudo -n install -d '$D0_DIR/pkg' && sudo -n tar -C '$D0_DIR/pkg' -xzf - && echo '$sha' | sudo -n tee '$D0_DIR/pkg.sha256' >/dev/null" <"$pkg"
  }
if [[ -n $D0_DTB ]]; then
  # shellcheck disable=SC2029
  ssh "${LAB_SSH_MPC[@]}" "$LAB_MINIPC" "sudo -n tee '$D0_DIR/swap.dtb' >/dev/null" <"$D0_DTB"
  lab_log "this run boots ${D0_DTB##*/} (sha256 $(sha256sum <"$D0_DTB" | cut -c1-16)) in place of the package's device tree"
fi
lab_mpc_root -t 60 'rm -rf "$1/swap" && install -d "$1/swap"' "$D0_DIR" || lab_die "the mini-PC's swap directory"
for pair in $D0_SWAP; do
  name=${pair%%=*} file=${pair#*=}
  [[ $name != */* && -s $file ]] || lab_die "D0_SWAP: bad pair $pair"
  # shellcheck disable=SC2029
  ssh "${LAB_SSH_MPC[@]}" "$LAB_MINIPC" "sudo -n tee '$D0_DIR/swap/$name' >/dev/null" <"$file"
  lab_log "this run puts ${file##*/} (sha256 $(sha256sum <"$file" | cut -c1-16)) in place of the package's $name"
done
out=$(lab_mpc_root -t 120 'rm -rf "$1/work" && cp -a "$1/pkg" "$1/work" && cd "$1/work" || exit 1
for f in "$1"/swap/*; do [ -e "$f" ] || continue; [ -e "${f##*/}" ] || { echo "no ${f##*/} in the package"; exit 1; }; cp "$f" "${f##*/}"; done
if [ "$2" = 1 ]; then
  dtb=$(sed -n "s/^DTBFILE=\"\{0,1\}\([^\"]*\)\"\{0,1\}\$/\1/p" .env.initrd-flash 2>/dev/null | tail -n 1)
  [ -n "$dtb" ] || dtb=$(ls ./*-zedbox-mini.dtb | head -n 1)
  [ -f "$dtb" ] || { echo "no device tree to swap"; exit 1; }
  cp "$1/swap.dtb" "$dtb"
fi
if [ -n "$3" ]; then
  sed -i -e "$3" flashvars
  echo "flashvars edited:" >&2; grep -n "" flashvars >&2
fi
n=$(grep -n "^# Boot device flashing\$" initrd-flash | cut -d: -f1)
[ -n "$n" ] || { echo "initrd-flash has no \"# Boot device flashing\" line: meta-tegra changed it"; exit 1; }
head -n $((n - 1)) initrd-flash > initrd-flash-d0
echo "echo \"D0: stopping after the RCM boot, before any command or write\"; exit 0" >> initrd-flash-d0
chmod +x initrd-flash-d0
grep -c "write_to_device \\$" initrd-flash-d0 || true
# The MB2 configuration of Fjarr, without the carrier EEPROM read (NVIDIA default stops before UEFI).
grep -lE "cvb_eeprom_read_size = <0x0+>" ./*mb2-bct-misc*zedbox-mini*.dts* >/dev/null 2>&1 || { echo "the package lacks the MB2 configuration of Fjarr"; exit 1; }
docker image inspect fjarr-l4t-flash >/dev/null' "$D0_DIR" "$([[ -n $D0_DTB ]] && echo 1 || echo 0)" "$D0_FLASHVARS_SED" 2>"$LAB_DIR/d0/flashvars.txt") || lab_die "the mini-PC is not ready: $out"
[[ $out == 0 ]] || lab_die "initrd-flash-d0 still calls write_to_device: $out"
lab_log "initrd-flash-d0 ready: meta-tegra's script up to the RCM boot"
[[ -z $D0_FLASHVARS_SED ]] || lab_log "flashvars for this run: $(grep -E 'OVERLAY' "$LAB_DIR/d0/flashvars.txt" | tr '\n' ' ')"
fi

lab_usb_poll
[[ $LAB_USB_STATE == l4t ]] || lab_die "the mini-PC sees the box as '$LAB_USB_STATE', not L4T"
lab_find_box || lab_die "the box does not answer on its MAC"
lab_box_bootid || lab_die "no boot id: $LAB_SSH_ERR"
before=$LAB_BOOTID
mkdir -p "$LAB_DIR/logs"
fingerprint >"$LAB_DIR/d0/before.txt" || lab_die "could not read the box's fingerprint"

lab_rcm_enter
lab_rcm_wait || lab_die "no RCM within $LAB_RCM_TMO s (the mini-PC sees '$LAB_USB_STATE')"
lab_log "box in RCM; signing and the RCM boot"
lab_lock_check
log=$LAB_DIR/logs/d0-rcmboot-$(date -u +%Y%m%dT%H%M%SZ).log
rc=0
if [[ -n $D0_NVIDIA_L4T ]]; then
  lab_mpc_root -t 1200 'L4T=$1
docker run --rm --privileged --net=host -v /dev:/dev -v /run/nvidia_initrd_flash:/run/nvidia_initrd_flash:ro \
  -v "$L4T:$L4T" -w "$L4T" fjarr-l4t-flash \
  ./tools/kernel_flash/l4t_initrd_flash.sh --initrd --showlogs --network usb0 p3509-a02-p3767-0000 internal' "$D0_NVIDIA_L4T" >"$log" 2>&1 || rc=$?
  grep -q 'Device has booted into initrd' "$log" || rc=${rc/#0/1}
else
lab_mpc_root -t 900 'cd "$1/work" && docker run --rm --privileged --net=host -v /dev:/dev -v "$1/work:$1/work" -w "$1/work" \
  fjarr-l4t-flash ./initrd-flash-d0; cat "$(ls -t log.initrd-flash.* | head -n 1)"' "$D0_DIR" >"$log" 2>&1 || rc=$?
fi
lab_log "initrd-flash-d0 ended with exit $rc; its log: ${log#"$LAB_HOME"/}"
grep -E '^== Step|ERR|D0:|Device has booted into initrd|Error|Step [0-9]' "$log" >&2 || true

seen='' t0=$SECONDS last=''
if ((rc == 0)); then
  while ((SECONDS - t0 < D0_GADGET_S)); do
    lab_usb_poll
    [[ $LAB_USB_STATE == "$last" ]] || { lab_log "the mini-PC sees the box as: $LAB_USB_STATE ($((SECONDS - t0)) s)"; last=$LAB_USB_STATE; }
    case $LAB_USB_STATE in other:*) seen=${LAB_USB_STATE#other:} && break ;; esac
    sleep 2
  done
fi
if [[ -n $seen ]]; then
  lab_log "the flash initrd's USB device appeared $((SECONDS - t0)) s after the RCM boot: $seen"
  sleep "$D0_HOLD_S"
  lab_usb_poll
  lab_log "after $D0_HOLD_S s the mini-PC sees: $LAB_USB_STATE"
  lab_mpc 'for d in /sys/bus/usb/devices/*; do [ "$(cat $d/idVendor 2>/dev/null)" = 0955 ] && printf "%s %s:%s %s %s\n" "${d##*/}" "$(cat $d/idVendor)" "$(cat $d/idProduct)" "$(cat $d/product 2>/dev/null)" "$(cat $d/speed 2>/dev/null)"; done; lsblk -o NAME,SIZE,TRAN,VENDOR,MODEL,MOUNTPOINTS 2>/dev/null | grep -i usb || true' >>"$log" 2>&1 || true
fi

lab_lock_check
lab_power_cycle
t1=$SECONDS
while ((SECONDS - t1 < 2 * LAB_BACK_TMO)); do
  if lab_find_box && lab_box_bootid && [[ $LAB_BOOTID != "$before" ]]; then break; fi
  sleep 5
done
[[ -n ${LAB_BOOTID:-} && $LAB_BOOTID != "$before" ]] || lab_die "the stock system did not come back after the power cycle"
state=$(lab_wait_ready 300) || true
lab_log "stock system back $((SECONDS - t1)) s after the power cycle: systemd $state"
fingerprint >"$LAB_DIR/d0/after.txt" || lab_die "could not read the fingerprint after"
diff "$LAB_DIR/d0/before.txt" "$LAB_DIR/d0/after.txt" >"$LAB_DIR/d0/diff.txt" ||
  lab_die "the fingerprint changed (L7): .lab/d0/diff.txt"
((rc == 0)) || lab_die "initrd-flash-d0 failed (exit $rc): ${log#"$LAB_HOME"/}; the box is back and unchanged"
[[ -n $seen ]] || lab_die "no USB device from the flash initrd within $D0_GADGET_S s; the box is back and unchanged"
if [[ -n $D0_NVIDIA_L4T ]]; then
  lab_log "D0 comparison done: NVIDIA's own firmware and flash initrd booted from RAM; the box is unchanged"
else
  lab_log "D0 done: Fjarr's firmware and the flash initrd booted from RAM; the box is unchanged"
fi
