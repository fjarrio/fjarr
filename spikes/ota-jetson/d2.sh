#!/usr/bin/env bash
# shellcheck disable=SC2016 # the remote scripts take their arguments as $1, $2 there
# d2.sh: phase D2 (and D2b), NVIDIA's A/B semantics on the Fjarr image (slice 8.0).
# spec: docs/32-fleet-ota.md#jetson-test-image (phase D; rules L1, L2, L5, L6, L7); README.md, "D2"
#
#   LAB_DESKTOP_OK=1 d2.sh state|net|count|switch|stale|wdt|seeded|recover
#
# recover: after a stop that left the box hung, its dead-man's RCM and one power cycle, then the
# verify unmasked on both slots and the boot verified.
#
# Each test starts on slot A of the Fjarr image, committed, and ends there. Before and after
# every boot it records the same state (.lab/d2/<time>-<test>/<n>-<step>.txt) and logs a summary.
# To see the firmware's own behaviour a test masks fjarr-boot-verify.service on the slot it boots
# and unmasks it before it ends, also when it stops (LAB_DIE_HOOK). L6: at most two warm resets in
# a row without a verify. Nothing here writes the QSPI.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=lib/lab.sh
. "$here/lib/lab.sh"

test=${1:?usage: d2.sh state|net|count|switch|stale|wdt|seeded|recover}
: "${D2_BOOT_TMO:=600}"                            # from a reboot to ssh
: "${D2_NET_TMO:=900}"                             # from a reboot to the network rescue on ssh
: "${D2_NET_DIR:=$LAB_HOME/out/rescue/36.5.2}"     # rescue/build.sh --l4t 36.5.2
OUT=$LAB_DIR/d2/$(date -u +%Y%m%dT%H%M%SZ)-$test
mkdir -p "$OUT"
G=781e084c-a330-417c-b678-38e696380cb9

case $test in
  state) need=600 ;;
  recover) need=$((1800 + LAB_RCM_ROUND_WORST + 600)) ;;
  net)
    # The network rescue on 36.5.2 first: net-up takes the lab lock itself.
    [[ -s $D2_NET_DIR/fjarr-net-rescue.efi && -s $D2_NET_DIR/known_hosts ]] ||
      { echo "d2.sh: no network rescue in $D2_NET_DIR (rescue/build.sh --l4t 36.5.2)" >&2; exit 2; }
    B6_NET_EFI=$D2_NET_DIR/fjarr-net-rescue.efi B6_NET_KNOWN_HOSTS=$D2_NET_DIR/known_hosts "$here/b6.sh" net-up
    need=$((600 + 2 * (2 * D2_NET_TMO + 2 * D2_BOOT_TMO)))
    ;;
  count) need=$((600 + 3 * D2_BOOT_TMO + LAB_CYCLE_WORST + D2_BOOT_TMO)) ;;
  switch | stale) need=$((600 + 4 * D2_BOOT_TMO)) ;;
  wdt) need=$((600 + 4 * D2_BOOT_TMO + 1800 + LAB_RCM_ROUND_WORST)) ;;
  seeded) need=$((600 + 5 * D2_BOOT_TMO)) ;;
  *) echo "d2.sh: no test '$test'" >&2; exit 2 ;;
esac
lab_init "d2-$test" "$need"
lab_plug_check
lab_desktop_guard
if [[ $test != state && $test != recover ]]; then # L2: every test writes UEFI variables or the slot's root
  lab_backup_check || lab_die "no verified B3 backup (L2)"
  lab_mpc -t 30 'systemctl is-active -q fjarr-net-rescue-dhcp.service' ||
    lab_die "the network rescue is not serving on the mini-PC (b6.sh net-up; L2)"
fi
cut -d' ' -f2,3 "$D2_NET_DIR/known_hosts" >"$LAB_RUN/net.pub" 2>/dev/null || true
[[ $test == state ]] || lab_uart_start "d2-$test" # the box's console for the run; _lab_exit stops it

# --- who answers ---------------------------------------------------------------------------------

UP=none N=0 LAST=
use() { # slot | recovery | net: the ssh profile of that system
  case $1 in
    slot) lab_use_fjarr_image ;;
    recovery) lab_use_fjarr_rescue ;;
    net) _lab_use_root_profile fjarr-net-rescue "$LAB_RUN/net.pub" && LAB_RCM_CMD=reboot-recovery ;;
  esac
}
who_up() { # sets UP and LAB_BOOTID: the system that answers on the box's MAC, by its host key
  local p
  for p in slot recovery net; do
    use "$p"
    if lab_find_box && lab_box_bootid; then
      UP=$p
      return 0
    fi
  done
  UP=none
  return 1
}
wait_boot() { # <old boot id> <timeout s>: until a system answers with another boot id
  local t0=$SECONDS
  while ((SECONDS - t0 < $2)); do
    if who_up && [[ $LAB_BOOTID != "$1" ]]; then
      lab_deadman_touch || true
      lab_log "up after $((SECONDS - t0)) s: $UP, boot ${LAB_BOOTID:0:8}"
      return 0
    fi
    sleep 5
  done
  return 1
}

# --- the state -----------------------------------------------------------------------------------

state() { # <step>: record the state into OUT/<n>-<step>.txt (LAST) and log its summary
  local f
  N=$((N + 1))
  f=$OUT/$(printf %02d $N)-$1.txt
  lab_box -t 90 'g=781e084c-a330-417c-b678-38e696380cb9 u=8be4df61-93ca-11d2-aa0d-00e098032b8c t=ed3374ef-767b-42fa-af70-db520a392822
v() { f=/sys/firmware/efi/efivars/$1-$2; if [ -e "$f" ]; then printf "%s=%s\n" "$1" "$(hexdump -v -e "1/1 \"%02x\"" "$f")"; else echo "$1=absent"; fi; }
for n in BootChainFwCurrent BootChainFwNext BootChainFwStatus BootChainOsCurrent RootfsStatusSlotA RootfsStatusSlotB RootfsRetryCountMax RootfsRedundancyLevel SystemFwVersions L4TDefaultBootMode; do v $n $g; done
for n in BootChainFwPrevious BootChainFwResetCount; do v $n $t; done
for n in OsIndications BootNext BootCurrent BootOrder; do v $n $u; done
p=/sys/devices/platform/bus@0/c360000.pmc
for n in rootfs_retry_count_a rootfs_retry_count_b boot_chain_current boot_chain_status_a boot_chain_status_b reset_reason reset_level; do printf "pmc_%s=%s\n" $n "$(cat $p/$n 2>/dev/null || echo -)"; done
echo "bios=$(cat /sys/class/dmi/id/bios_version 2>/dev/null)"
echo "kernel=$(uname -r)"
echo "uptime=$(cut -d" " -f1 /proc/uptime)"
if command -v nvbootctrl >/dev/null; then nvbootctrl dump-slots-info 2>&1 | sed "s/^/bl: /"; nvbootctrl -t rootfs dump-slots-info 2>&1 | sed "s/^/rootfs: /"; fi
if command -v systemctl >/dev/null; then echo "verify_unit=$(systemctl is-enabled fjarr-boot-verify.service 2>&1)"; fi' >"$f" 2>&1 ||
    { lab_log "state $1: could not read the state ($UP)"; return 1; }
  printf 'system=%s boot=%s\n' "$UP" "$LAB_BOOTID" >>"$f"
  LAST=$f
  lab_log "[$N $1] $UP $(summary "$f")"
}
val() { sed -n "s/^$1=//p" "$2" | head -n 1; } # <key> <state file>
le32() { # <efivarfs hex>: the UINT32 after the 4 attribute bytes, or the word as is
  local h=$1
  [[ $h =~ ^[0-9a-f]{16}$ ]] || { printf '%s' "$h"; return; }
  printf '%d' "0x${h:14:2}${h:12:2}${h:10:2}${h:8:2}"
}
summary() { # <state file>: one line of what the tests compare
  local f=$1 r
  r=$(grep -E '^rootfs: slot: [01],' "$f" | sed -E 's/.*slot: ([01]),.*retry_count: ([0-9]+),.*status: ([a-z]+).*/\1:\2:\3/' | tr '\n' ' ')
  printf 'fw=%s os=%s next=%s status=%s resetcount=%s slotA=%s slotB=%s pmc_a=%s pmc_b=%s reason=%s nvbootctrl[%s] bootnext=%s verify=%s' \
    "$(le32 "$(val BootChainFwCurrent "$f")")" "$(le32 "$(val BootChainOsCurrent "$f")")" \
    "$(le32 "$(val BootChainFwNext "$f")")" "$(le32 "$(val BootChainFwStatus "$f")")" \
    "$(le32 "$(val BootChainFwResetCount "$f")")" "$(le32 "$(val RootfsStatusSlotA "$f")")" \
    "$(le32 "$(val RootfsStatusSlotB "$f")")" "$(val pmc_rootfs_retry_count_a "$f")" \
    "$(val pmc_rootfs_retry_count_b "$f")" "$(val pmc_reset_reason "$f")" "${r% }" \
    "$(val BootNext "$f")" "$(val verify_unit "$f")"
}
os_slot() { # the slot L4TLauncher booted, from the last state file
  le32 "$(val BootChainOsCurrent "$LAST")"
}

# --- actions on the box --------------------------------------------------------------------------

on_box() { # <command>: as root on what answers; the output to the run's log
  lab_lock_check
  lab_log "box ($UP): $1"
  lab_box -t 60 "$1" 2>&1 | sed 's/^/  | /' >&2
  return "${PIPESTATUS[0]}"
}
reboot_with() { # <command>: one that ends the session (reboot, a hang, a panic); 255 and 124 expected
  local rc=0
  lab_lock_check
  lab_log "box ($UP): $1"
  _lab_tmo 20 ssh -n "${LAB_SSH_BOX[@]}" "$LAB_BOX_USER@$LAB_BOX_IP" "$1" >/dev/null 2>&1 || rc=$?
  case $rc in 0 | 124 | 255) ;; *) lab_die "the box refused '$1' (exit $rc)" ;; esac
}
MASKED=() # the slots whose fjarr-boot-verify this run masked: a, b
mask_verify() { # <a|b>: on the booted slot, or on the other slot's root mounted from it
  local s=$1
  if [[ $s == "$(slot_letter)" ]]; then
    on_box 'systemctl mask fjarr-boot-verify.service' || lab_die "could not mask fjarr-boot-verify on slot $s"
  else
    other_root "$s" 'ln -sfn /dev/null "$r/etc/systemd/system/fjarr-boot-verify.service"' ||
      lab_die "could not mask fjarr-boot-verify on slot $s"
  fi
  MASKED+=("$s")
}
unmask_verify() { # <a|b>
  local s=$1
  if [[ $s == "$(slot_letter)" ]]; then
    on_box 'systemctl unmask fjarr-boot-verify.service' || return 1
  else
    other_root "$s" 'rm -f "$r/etc/systemd/system/fjarr-boot-verify.service"' || return 1
  fi
  MASKED=("${MASKED[@]/$s/}")
}
slot_letter() { case $(os_slot) in 0) echo a ;; 1) echo b ;; *) echo "?" ;; esac; }
other_root() { # <a|b> <script with $r>: on the other slot's root, mounted read-write from this one
  local part=APP
  [[ $1 == b ]] && part=APP_b
  lab_lock_check
  lab_box_root -t 60 'set -e; r=/run/fjarr-d2-root; mkdir -p $r; mount /dev/disk/by-partlabel/$1 $r
trap "umount $r" EXIT
'"$2"'
sync' "$part"
}
nv() { on_box "nvbootctrl $1"; } # <nvbootctrl arguments>

d2_die_hook() { # L7: record what answers, then give every masked slot its verify back
  local s
  who_up || return 0
  state stopped || true
  [[ $UP == slot ]] || return 0
  for s in "${MASKED[@]}"; do [[ -n $s ]] && unmask_verify "$s" && lab_log "unmasked fjarr-boot-verify on slot $s"; done
}
LAB_DIE_HOOK=d2_die_hook

start_on_a() { # every test starts on slot A, committed
  who_up || lab_die "nothing answers on the box's MAC"
  [[ $UP == slot ]] || lab_die "the box runs the $UP system, not the Fjarr slot"
  lab_deadman_touch
  state start
  [[ $(os_slot) == 0 ]] || lab_die "the box booted slot $(slot_letter), not A"
  grep -q '^verify_unit=enabled' "$LAST" ||
    lab_die "fjarr-boot-verify is not enabled on slot A: a test left it masked?"
}
warm() { # <step>: a warm reboot and the state after it
  local before=$LAB_BOOTID
  reboot_with 'systemctl reboot'
  wait_boot "$before" "$D2_BOOT_TMO" || lab_die "nothing came back within $D2_BOOT_TMO s of the reboot"
  state "$1"
}

# --- the tests -----------------------------------------------------------------------------------

t_state() {
  who_up || lab_die "nothing answers on the box's MAC"
  lab_deadman_touch || true
  state now
  cat "$LAST" >&2
}

t_net() {
  local way entry before bo
  start_on_a
  bo=$(val BootOrder "$LAST")
  for way in PXEv4 HTTPv4; do
    entry=$(lab_box -t 30 "efibootmgr | sed -n 's/^Boot\([0-9A-F]\{4\}\)\* UEFI $way (MAC:.*/\1/p' | head -n 1")
    [[ $entry =~ ^[0-9A-F]{4}$ ]] || lab_die "no 'UEFI $way' boot entry"
    on_box "efibootmgr -q -n $entry" || lab_die "could not set BootNext to $entry"
    state "bootnext-$way"
    before=$LAB_BOOTID
    reboot_with 'systemctl reboot'
    wait_boot "$before" "$D2_NET_TMO" || lab_die "nothing answered within $D2_NET_TMO s of the reboot to $way"
    state "net-$way"
    [[ $UP == net ]] || lab_die "the reboot by $way came up as the $UP system, not the network rescue"
    on_box 'lsblk -dno NAME,SIZE /dev/nvme0n1; cat /proc/cmdline' || true
    before=$LAB_BOOTID
    reboot_with 'reboot -f'
    wait_boot "$before" "$D2_BOOT_TMO" || lab_die "nothing came back after the network rescue's reboot"
    state "back-$way"
    [[ $UP == slot && $(os_slot) == 0 ]] || lab_die "after the network rescue the box runs $UP, slot $(slot_letter)"
    [[ $(val BootNext "$LAST") == absent ]] || lab_die "BootNext is still set (L5)"
    [[ $(val BootOrder "$LAST") == "$bo" ]] || lab_die "BootOrder changed"
    lab_log "the network rescue on 36.5.2 by $way: passed"
  done
}

t_count() {
  local before
  start_on_a
  mask_verify a
  state masked
  warm warm-1
  warm warm-2 # L6: no third warm reset without a verify
  before=$LAB_BOOTID
  lab_lock_check
  lab_power_cycle
  wait_boot "$before" "$D2_BOOT_TMO" || lab_die "nothing came back after the power cycle"
  state cold
  nv verify || lab_die "nvbootctrl verify failed"
  state verified
  unmask_verify a || lab_die "could not unmask fjarr-boot-verify"
  state end
}

t_switch() {
  local status
  start_on_a
  mask_verify a
  mask_verify b
  nv 'set-active-boot-slot 1' || lab_die "nvbootctrl set-active-boot-slot 1 failed"
  state set-1
  warm on-b
  [[ $(os_slot) == 1 ]] || lab_die "set-active-boot-slot 1 and a reboot left the box on slot $(slot_letter)"
  nv verify || lab_die "nvbootctrl verify failed on slot B"
  state b-verified
  nv 'set-active-boot-slot 0' || lab_die "nvbootctrl set-active-boot-slot 0 failed"
  state set-0-status-left
  warm after-set-0-status-left
  if [[ $(os_slot) == 1 ]]; then
    lab_log "with BootChainFwStatus left set, the switch to slot A did not happen (as the source says)"
    nv verify || lab_die "nvbootctrl verify failed on slot B"
    status=$(val BootChainFwStatus "$LAST")
    if [[ $status != absent ]]; then
      on_box "f=/sys/firmware/efi/efivars/BootChainFwStatus-$G; chattr -i \$f; rm \$f" || lab_die "could not delete BootChainFwStatus"
    fi
    nv 'set-active-boot-slot 0' || lab_die "nvbootctrl set-active-boot-slot 0 failed"
    state set-0-status-cleared
    warm back-on-a
  else
    lab_log "the switch to slot A happened with BootChainFwStatus left set"
  fi
  [[ $(os_slot) == 0 ]] || lab_die "the box did not get back to slot A"
  nv verify || lab_die "nvbootctrl verify failed on slot A"
  unmask_verify a || lab_die "could not unmask fjarr-boot-verify on slot A"
  unmask_verify b || lab_die "could not unmask fjarr-boot-verify on slot B"
  state end
}

t_stale() { # a switch while the last one's BootChainFwStatus is still set (no verify between)
  start_on_a
  mask_verify a
  mask_verify b
  nv 'set-active-boot-slot 1' || lab_die "nvbootctrl set-active-boot-slot 1 failed"
  warm on-b
  [[ $(os_slot) == 1 ]] || lab_die "set-active-boot-slot 1 and a reboot left the box on slot $(slot_letter)"
  [[ $(le32 "$(val BootChainFwStatus "$LAST")") == 0 ]] || lab_die "the switch left no BootChainFwStatus 0 to test with"
  nv 'set-active-boot-slot 0' || lab_die "nvbootctrl set-active-boot-slot 0 failed"
  state set-0-status-set
  warm after-set-0 # L6: the second warm reset on slot B without a verify, and the last
  if [[ $(os_slot) == 1 ]]; then
    lab_log "with BootChainFwStatus set, the switch to slot A did not happen"
    nv verify || lab_die "nvbootctrl verify failed on slot B"
    nv 'set-active-boot-slot 0' || lab_die "nvbootctrl set-active-boot-slot 0 failed"
    state set-0-after-verify
    warm back-on-a
  else
    lab_log "with BootChainFwStatus set, the switch to slot A happened all the same"
  fi
  [[ $(os_slot) == 0 ]] || lab_die "the box did not get back to slot A"
  nv verify || lab_die "nvbootctrl verify failed on slot A"
  unmask_verify a || lab_die "could not unmask fjarr-boot-verify on slot A"
  unmask_verify b || lab_die "could not unmask fjarr-boot-verify on slot B"
  other_root b 'test ! -e "$r/etc/systemd/system/fjarr-boot-verify.service" && echo "slot B: fjarr-boot-verify not masked"' || lab_die "slot B is still masked"
  state end
}

t_wdt() {
  local before t0
  start_on_a
  mask_verify a
  # 1. The driver alone: systemd lets go of the watchdog (a magic close: stopped, its 30 s timeout
  # kept), a shell opens it, feeds it once and holds it unfed.
  before=$LAB_BOOTID
  on_box 'busctl set-property org.freedesktop.systemd1 /org/freedesktop/systemd1 org.freedesktop.systemd1.Manager RuntimeWatchdogUSec t 0' ||
    lab_die "could not stop systemd's watchdog"
  reboot_with 'setsid sh -c "exec 3>/dev/watchdog0; printf x >&3; sleep 900" </dev/null >/dev/null 2>&1 & echo held'
  t0=$SECONDS
  wait_boot "$before" "$D2_BOOT_TMO" || lab_die "no reset within $D2_BOOT_TMO s of the unfed hold"
  lab_log "reset about $((SECONDS - t0)) s after the last feed (with the boot)"
  state hold
  nv verify || lab_die "nvbootctrl verify failed"
  state hold-verified
  # 2. A panic (panic=10).
  before=$LAB_BOOTID
  reboot_with 'echo c > /proc/sysrq-trigger'
  wait_boot "$before" "$D2_BOOT_TMO" || lab_die "no reset within $D2_BOOT_TMO s of the panic"
  state panic
  nv verify || lab_die "nvbootctrl verify failed"
  state panic-verified
  # 3. systemd crashed: PID 1 takes only the signals it handles, so SIGSTOP is dropped (D2,
  # 2026-11-14); SIGSEGV reaches its crash handler, which freezes it. If nothing resets the box,
  # its dead-man sends it to RCM (L3) and one power cycle brings it back.
  before=$LAB_BOOTID
  reboot_with 'kill -SEGV 1'
  t0=$SECONDS
  if wait_boot "$before" "$D2_BOOT_TMO"; then
    lab_log "reset about $((SECONDS - t0)) s after systemd crashed"
  else
    lab_log "no reset within $D2_BOOT_TMO s of systemd's crash; waiting for the dead-man's RCM"
    lab_rcm_wait 1800 || lab_die "no RCM from the dead-man within 30 min of the crash"
    lab_log "RCM $((SECONDS - t0)) s after systemd crashed: the dead-man; one power cycle"
    lab_rcm_leave
    wait_boot "$before" "$D2_BOOT_TMO" || lab_die "nothing came back after the power cycle out of RCM"
  fi
  state crash
  nv verify || lab_die "nvbootctrl verify failed"
  unmask_verify a || lab_die "could not unmask fjarr-boot-verify"
  state end
}

hold_unfed() { # systemd lets go of the watchdog (a magic close), a shell feeds it once and holds it
  on_box 'busctl set-property org.freedesktop.systemd1 /org/freedesktop/systemd1 org.freedesktop.systemd1.Manager RuntimeWatchdogUSec t 0' ||
    lab_die "could not stop systemd's watchdog"
  reboot_with 'setsid sh -c "exec 3>/dev/watchdog0; printf x >&3; sleep 900" </dev/null >/dev/null 2>&1 & echo held'
}

t_seeded() { # a watchdog reset and a panic each after a warm reboot: count down, or seed afresh?
  local before
  start_on_a
  mask_verify a
  warm warm-before-hold # the register seeded: slot A at 2
  before=$LAB_BOOTID
  hold_unfed
  wait_boot "$before" "$D2_BOOT_TMO" || lab_die "no reset within $D2_BOOT_TMO s of the unfed hold"
  state hold # L6: the second reset without a verify; 1 counts like a warm reset, 2 like a cold one
  nv verify || lab_die "nvbootctrl verify failed"
  warm warm-before-panic
  before=$LAB_BOOTID
  reboot_with 'echo c > /proc/sysrq-trigger'
  wait_boot "$before" "$D2_BOOT_TMO" || lab_die "no reset within $D2_BOOT_TMO s of the panic"
  state panic
  nv verify || lab_die "nvbootctrl verify failed"
  unmask_verify a || lab_die "could not unmask fjarr-boot-verify"
  state end
}

t_recover() {
  if ! who_up; then
    lab_log "nothing answers: waiting for the dead-man's RCM (up to 30 min)"
    lab_rcm_wait 1800 || lab_die "no RCM within 30 min (the mini-PC sees '$LAB_USB_STATE')"
    lab_rcm_leave
    wait_boot "" "$D2_BOOT_TMO" || lab_die "nothing came back after the power cycle out of RCM"
  fi
  [[ $UP == slot ]] || lab_die "the box runs the $UP system, not the Fjarr slot"
  state found
  grep -q '^verify_unit=masked' "$LAST" && { unmask_verify "$(slot_letter)" || lab_die "could not unmask the booted slot"; }
  local other=b
  [[ $(slot_letter) == b ]] && other=a
  other_root "$other" 'rm -f "$r/etc/systemd/system/fjarr-boot-verify.service"' || lab_die "could not unmask slot $other"
  nv verify || lab_die "nvbootctrl verify failed"
  state end
}

"t_$test"
lab_log "d2 $test done: ${OUT#"$LAB_HOME"/}"
