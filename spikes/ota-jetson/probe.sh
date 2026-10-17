#!/usr/bin/env bash
# probe.sh: a read-only state dump of the ZED Box Mini (slice 8.0, phase B1).
# spec: docs/32-fleet-ota.md#jetson-test-image
#
#   probe.sh [label]        label: stock (default), or what is on the box, e.g. fjarr-d1
#
# One ssh session runs a script as root on the box (sudo -n). It only reads: no write to the disk,
# the ESPs, the UEFI variables, the PMC registers or the QSPI, no mount, no file read through the
# mounted ESP, no probing of a bus (i2cdetect lists the buses only), and it never opens
# /dev/watchdog (opening it would arm the watchdog). The raw dump and the box's identifiers stay
# in .lab/probe/<time>/; results/<date>-probe-<label>.txt gets a digest and the dump with every
# identifier replaced (lab_redact in lib/lab.sh), and is refused if one survived.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=lib/lab.sh
. "$here/lib/lab.sh"

label=${1:-stock}
[[ $label =~ ^[a-z0-9][a-z0-9-]*$ ]] || {
  echo "usage: probe.sh [label]" >&2
  exit 2
}
lab_init "probe-$label" 900

# The box side. bash as root; every command shows its exit status when it is not 0.
probe_script() {
  cat <<'EOF'
nv=781e084c-a330-417c-b678-38e696380cb9   # NVIDIA's UEFI variable GUID
tok=ed3374ef-767b-42fa-af70-db520a392822  # gNVIDIATokenSpaceGuid: BootChainFwPrevious, ...ResetCount
efi=8be4df61-93ca-11d2-aa0d-00e098032b8c  # the UEFI global variables
sec() { printf '\n===== %s\n' "$*"; }
run() { printf '\n$ %s\n' "$*"; timeout 60 "$@" 2>&1; rc=$?; [ "$rc" -eq 0 ] || printf '[exit %s]\n' "$rc"; }
var() { # <name> <guid>: the 4 attribute bytes, then the data, in hex
  f=/sys/firmware/efi/efivars/$1-$2
  if [ -e "$f" ]; then printf '%-24s %s\n' "$1" "$(od -An -tx1 -v "$f" | tr -s ' \n' '  ' | sed 's/^ //; s/ $//')"
  else printf '%-24s absent\n' "$1"; fi; }

sec "identity and versions"
run date -u
run uptime
run cat /etc/nv_tegra_release
run sh -c 'tr -d "\000" </proc/device-tree/model; echo'
run sh -c 'tr "\000" " " </proc/device-tree/compatible; echo'
run uname -r -v -m
run dpkg-query -W -f '${Package} ${Version}\n' nvidia-l4t-core nvidia-l4t-bootloader nvidia-l4t-kernel \
  nvidia-l4t-init nvidia-l4t-tools stereolabs-zedbox-mini
run cat /etc/nv_boot_control.conf

sec "slots (nvbootctrl, read only)"
run nvbootctrl dump-slots-info
run nvbootctrl -t rootfs dump-slots-info
nvbootctrl -t rootfs is-rootfs-ab-enabled >/dev/null 2>&1; ab=$?
printf '\nis-rootfs-ab-enabled exit status: %s (0 disabled, 1 enabled on A, 2 enabled on B)\n' "$ab"
run nvbootctrl get-number-slots
run nvbootctrl get-current-slot
run nvbootctrl -t rootfs get-number-slots
run nvbootctrl -t rootfs get-current-slot

sec "NVIDIA UEFI variables ($nv): attributes, then data, hex"
for v in L4TDefaultBootMode RootfsRedundancyLevel RootfsRetryCountMax RootfsStatusSlotA \
  RootfsStatusSlotB BootChainFwCurrent BootChainFwNext BootChainFwStatus \
  BootChainOsCurrent TegraPlatformSpec TegraPlatformCompatSpec; do var "$v" "$nv"; done
# Boot-services-only under gNVIDIATokenSpaceGuid by edk2-nvidia's source (applier/README.md, "From
# the source"): absent here is what the source says, not a measurement; present would be news.
echo "under $tok, not runtime-visible by edk2-nvidia's source:"
for v in BootChainFwPrevious BootChainFwResetCount; do var "$v" "$tok"; done
for v in TegraPlatformSpec TegraPlatformCompatSpec; do
  f=/sys/firmware/efi/efivars/$v-$nv
  [ -e "$f" ] && printf '%s as text: %s\n' "$v" "$(cat "$f" | tail -c +5 | tr -d '\000')" # efivarfs cannot seek
done
echo; echo "every variable under NVIDIA's GUID:"
for f in /sys/firmware/efi/efivars/*-"$nv"; do n=${f##*/}; printf '%s ' "${n%-"$nv"}"; done; echo

sec "UEFI global variables ($efi)"
for v in BootCurrent BootNext BootOrder Timeout OsIndications OsIndicationsSupported SecureBoot SetupMode; do var "$v" "$efi"; done

sec "boot entries"
run efibootmgr -v

# The ESP is mounted rw,relatime: reading its files through the mount could update their FAT
# access dates. Both ESPs are read from the raw partition instead: fsck.vfat -n opens it read-only.
sec "partitions, and both ESPs (from the raw partitions, read only)"
run lsblk -o NAME,SIZE,TYPE,PARTLABEL,PARTTYPE,FSTYPE,MOUNTPOINTS /dev/nvme0n1
for p in esp esp_alt; do
  d=/dev/disk/by-partlabel/$p
  printf '\n--- %s\n' "$p"
  [ -e "$d" ] || { echo "no partition labelled $p"; continue; }
  run blkid -p -o export "$d"
  echo "mounted at: $(findmnt -n -o TARGET -S "$(readlink -f "$d")" | head -n 1)"
  t=$(blkid -p -s TYPE -o value "$d" 2>/dev/null)
  if [ "$t" = vfat ]; then
    run fsck.vfat -n -l -v "$d"
    run sha256sum "$d"
  elif [ -z "$t" ]; then
    n=$(blockdev --getsize64 "$d")
    if cmp -s -n "$n" "$d" /dev/zero; then echo "no file system: all $n bytes are zero"
    else echo "no file system, and not all zero"; fi
  else
    echo "a $t file system: not listed"
  fi
done

sec "ESRT and firmware version"
for e in /sys/firmware/efi/esrt/entries/*; do
  for a in fw_class fw_type fw_version lowest_supported_fw_version capsule_flags last_attempt_version last_attempt_status; do
    printf '%s %s: %s\n' "${e##*/}" "$a" "$(cat "$e/$a" 2>&1)"
  done
done
for a in bios_vendor bios_version bios_release bios_date; do printf '%s: %s\n' "$a" "$(cat /sys/class/dmi/id/$a 2>&1)"; done

sec "PMC: boot chain and root slot retry state (read only)"
pmc=''
for p in /sys/devices/platform/*/*.pmc /sys/devices/platform/*.pmc; do [ -d "$p" ] && { pmc=$p; break; }; done
echo "PMC: ${pmc:-not found}"
for a in boot_chain_current boot_chain_status_a boot_chain_status_b rootfs_current rootfs_retry_count_a \
  rootfs_retry_count_b rootfs_sr_magic reset_reason reset_level; do
  [ -n "$pmc" ] && [ -e "$pmc/$a" ] && printf '%s: %s\n' "$a" "$(cat "$pmc/$a" 2>&1)"
done

sec "/proc/mtd (is the QSPI visible to Linux?)"
run cat /proc/mtd

sec "watchdog (sysfs only: /dev/watchdog is never opened)"
run sh -c 'ls -la /dev/watchdog*'
for w in /sys/class/watchdog/*; do
  [ -e "$w" ] || continue
  printf '%s attributes:' "${w##*/}"; for a in "$w"/*; do printf ' %s' "${a##*/}"; done; echo
  for a in identity state status timeout timeleft pretimeout bootstatus nowayout; do
    [ -r "$w/$a" ] && printf '%s %s: %s\n' "${w##*/}" "$a" "$(cat "$w/$a" 2>&1)"
  done
  printf '%s driver: %s\n' "${w##*/}" "$(basename "$(readlink -f "$w/device/driver")")"
done
echo; echo "who holds a watchdog device open (fuser exits 1 when nobody does):"
run fuser -v /dev/watchdog /dev/watchdog0
run systemctl show -p RuntimeWatchdogUSec -p RuntimeWatchdogPreUSec -p RebootWatchdogUSec -p KExecWatchdogUSec -p WatchdogDevice
run sh -c 'dmesg | grep -iE "watchdog|wdt|tegra186-timer|tegra-wdt"'
run sh -c 'lsmod | grep -iE "wdt|watchdog|timer"'

sec "kernel configuration (watchdog, panic, lockups, QSPI, EFI)"
run sh -c "zcat /proc/config.gz | grep -E '^(# )?CONFIG_[A-Z0-9_]*(WATCHDOG|WDT|PANIC|LOCKUP|HUNG_TASK|TEGRA186_TIMER|MAGIC_SYSRQ|PSTORE|RAMOOPS|MTD_SPI_NOR|SPI_TEGRA210_QUAD|EFI_VARS|EFIVAR|EFI_ESRT|EFI_CAPSULE)'"

sec "kernel command line and panic sysctls"
run cat /proc/cmdline
run sysctl kernel.panic kernel.panic_on_oops kernel.panic_on_warn kernel.softlockup_panic \
  kernel.hardlockup_panic kernel.hung_task_panic kernel.hung_task_timeout_secs kernel.panic_on_rcu_stall \
  kernel.sysrq kernel.watchdog kernel.nmi_watchdog kernel.watchdog_thresh

sec "boot verification services"
for u in nv_update_verifier nv-l4t-bootloader-config l4t-rootfs-validation-config; do
  printf 'unit %s: load=%s enabled=%s active=%s/%s result=%s started=%s\n' "$u" \
    "$(systemctl show -p LoadState --value "$u.service")" "$(systemctl show -p UnitFileState --value "$u.service")" \
    "$(systemctl show -p ActiveState --value "$u.service")" "$(systemctl show -p SubState --value "$u.service")" \
    "$(systemctl show -p Result --value "$u.service")" "$(systemctl show -p ExecMainStartTimestamp --value "$u.service")"
done
for u in nv_update_verifier nv-l4t-bootloader-config l4t-rootfs-validation-config; do
  run sh -c "systemctl cat $u.service 2>&1 | grep -vE '^#'"
done
run sh -c 'journalctl -b -o cat --no-pager -u nv-l4t-bootloader-config.service -u l4t-rootfs-validation-config.service | tail -n 40'
run grep -n nvbootctrl /opt/nvidia/l4t-bootloader-config/nv-l4t-bootloader-config.sh

sec "board"
if command -v tegra-boardspec >/dev/null; then run tegra-boardspec
else echo "tegra-boardspec: not installed; the board spec is TNSPEC in /etc/nv_boot_control.conf and TegraPlatformSpec, above"; fi
run lspci -nnk
nic=$(ip route show default | awk '{ for (i = 1; i < NF; i++) if ($i == "dev") { print $(i + 1); exit } }')
echo; echo "the default route's interface: ${nic:-none}"
if [ -n "$nic" ]; then
  run ethtool -i "$nic"
  run ethtool "$nic"
  run nmcli -f GENERAL.DEVICE,GENERAL.TYPE,GENERAL.STATE,GENERAL.CONNECTION,IP4.ADDRESS,IP4.GATEWAY,DHCP4 device show "$nic"
fi
run gpioinfo
run i2cdetect -l
# The devices the kernel bound on each I2C bus, from sysfs: i2cdetect -y would probe the bus.
echo; echo "I2C devices bound by the kernel:"
for d in /sys/bus/i2c/devices/*-*; do
  [ -r "$d/name" ] || continue
  drv=none
  [ -e "$d/driver" ] && drv=$(basename "$(readlink -f "$d/driver")")
  printf '%s %s driver=%s\n' "${d##*/}" "$(cat "$d/name")" "$drv"
done

sec "secure boot"
if command -v mokutil >/dev/null; then run mokutil --sb-state
else echo "mokutil: not installed; SecureBoot and SetupMode are under the UEFI global variables, above"; fi

sec "storage"
run df -h /
run df -B1 /
run lsblk -b -o NAME,SIZE,TYPE,PARTLABEL,FSTYPE,MOUNTPOINTS
run sh -c 'cat /sys/block/nvme0n1/queue/logical_block_size /sys/block/nvme0n1/queue/physical_block_size'
run swapon --show
run systemctl is-system-running
exit 0
EOF
}

# The lines of the redacted dump that answer B1's questions at a glance.
digest() {
  grep -aE \
    -e '^# R[0-9]+ \(release\)' \
    -e '^(nvidia-l4t-core|nvidia-l4t-bootloader|stereolabs-zedbox-mini) ' \
    -e '^\$ nvbootctrl.*dump-slots-info' -e '^RootFS A/B' -e '^is-rootfs-ab-enabled exit status' \
    -e '^(Current version|Capsule update status|Current bootloader slot|Active bootloader slot|Current rootfs slot|Active rootfs slot|num_slots|slot: )' \
    -e '^(L4TDefaultBootMode|RootfsRedundancyLevel|RootfsRetryCountMax|RootfsStatusSlot[AB]|BootChainFw[A-Za-z]*|BootChainOsCurrent|BootCurrent|BootNext|OsIndications|SecureBoot) ' \
    -e '^entry[0-9]+ (fw_version|lowest_supported_fw_version|last_attempt_version|last_attempt_status):' \
    -e '^bios_version:' \
    -e '^(boot_chain_current|boot_chain_status_[ab]|rootfs_current|rootfs_retry_count_[ab]|reset_reason|reset_level):' \
    -e '^mtd[0-9]+:' -e '^watchdog[0-9]+ driver:' \
    -e '^(RuntimeWatchdogUSec|RebootWatchdogUSec|WatchdogDevice)=' \
    -e '^kernel\.(panic|panic_on_oops|softlockup_panic|hung_task_panic) =' \
    -e '^unit [^ ]+: ' \
    -e '^(running|degraded|starting|maintenance)$' \
    "$1" || true
}

lab_plug_check || lab_log "the plug does not answer (the probe goes on: it changes nothing)"
lab_find_box || lab_die "the box does not answer on its MAC (L2)"
lab_usb_poll
usb=$LAB_USB_STATE
lab_log "box at $LAB_BOX_IP; the mini-PC's USB sees: $usb"

stamp=$(TZ=$LAB_TZ date +%Y%m%dT%H%M%S)
raw=$LAB_DIR/probe/$stamp-$label
mkdir -p "$raw"
lab_box_root "$LAB_SECRETS_SCRIPT" >"$raw/secrets.txt" || lab_die "could not collect the box's identifiers"
grep -q '^hostname ' "$raw/secrets.txt" || lab_die "no host name among the box's identifiers: not redacting blind"
lab_log "probing (read only) ..."
lab_box_root "$(probe_script)" >"$raw/box.txt" 2>&1 || lab_die "the probe failed on the box (see $raw/box.txt)"
lab_log "raw dump: $raw/box.txt ($(wc -l <"$raw/box.txt") lines)"

lab_redact "$raw/secrets.txt" <"$raw/box.txt" >"$raw/box.redacted.txt"
out=$LAB_HOME/results/$(lab_today)-probe-$label.txt
[[ -e $out ]] && out=${out%.txt}-$(TZ=$LAB_TZ date +%H%M).txt
{
  printf '# ZED Box Mini, read-only probe "%s": %s %s\n' "$label" "$(TZ=$LAB_TZ date '+%Y-%m-%d %H:%M')" "$LAB_TZ"
  printf '# spec: docs/32-fleet-ota.md#jetson-test-image (phase B1). Written by probe.sh. Host names,\n'
  printf '# MACs, serial numbers, disk and partition GUIDs and machine IDs are replaced by <kind>.\n'
  printf '\n## Seen from the lab\n\n'
  printf 'plug: output=%s, %s W\n' "$LAB_PLUG_OUTPUT" "${LAB_PLUG_APOWER:-?}"
  printf 'the mini-PC USB sees the box as: %s (%s = L4T running, %s = RCM)\n' "$usb" "$LAB_L4T_USB" "$LAB_RCM_USB"
  printf '\n## Digest\n\n'
  digest "$raw/box.redacted.txt"
  printf '\n## The dump\n'
  cat "$raw/box.redacted.txt"
} >"$raw/result.txt"
lab_leak_check "$raw/result.txt" "$raw/secrets.txt" || lab_die "an identifier survived the redaction: nothing written to results/ (see $raw/result.txt)"
cp "$raw/result.txt" "$out"
lab_log "written: ${out#"$LAB_HOME"/}"
