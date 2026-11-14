#!/usr/bin/env bash
# shellcheck disable=SC2016 # the remote scripts take their arguments as $1, $2 there
# d4.sh: phase D4, a trial kernel that hangs before userspace (slice 8.0).
# spec: docs/32-fleet-ota.md#jetson-test-image (phase D; rules L2, L5, L7; Q4); docs/18 #48;
# README.md, "D4"
#
#   LAB_DESKTOP_OK=1 D4_PERSON_AT_BOX=1 d4.sh setup|panic|panic-nv|live
#
# Each case starts on D4_BASE in slot A, committed, installs D4_NEW into slot B from a stage of
# its own that carries no capsule (nothing reaches the QSPI), and rewrites slot B's
# extlinux.conf into a menu: entry 0, the default, the hang; entry 1, the slot's own boot. Then
# set-next and a reboot. The hang has no initrd and a root that does not exist, so it stops
# before anything opens a watchdog from userspace; both kernels arm WDT0 at probe (120 s) and
# pet it from its interrupt.
#   setup     copy NVIDIA's unpatched kernel (yocto/build.sh --unpatched-kernel) to the box
#   panic     the release's kernel, panic=0: it panics with interrupts off. Expected: the
#             watchdog resets, each reset counts down like a warm one (D2b), and L4TLauncher falls
#             back to slot A by itself
#   panic-nv  the same with NVIDIA's unpatched kernel
#   live      the release's kernel waiting for that root (rootwait): the kernel stays alive and
#             its interrupt keeps the watchdog fed. Expected: no reset (the gap, docs/18 #48);
#             after a power cut slot B again, a cold boot giving it its count back, and the same
#             hang
# A hang that does not end by itself is left by a power cut and entry 1, which the mini-PC types
# on the box's console at L4TLauncher's menu: the slot's own boot, a trial that confirms, then
# rollback-now. The button is needed only if neither comes up: D4_PERSON_AT_BOX=1 (Q4).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=lib/lab.sh
. "$here/lib/lab.sh"

case=${1:?usage: d4.sh setup|panic|panic-nv|live}
: "${D4_BASE:=1.0}"                        # the release in slot A
: "${D4_NEW:=1.2}"                         # the release the hang is made from
: "${D4_REL:=$LAB_HOME/out/swu/$D4_NEW}"   # its release directory (mkswu.sh)
: "${D4_NV_IMAGE:=$LAB_HOME/out/yocto/build-nvwdt/tmp/deploy/images/zedbox-mini-orin-nx16/Image}"
: "${D4_BOOT_TMO:=600}"                    # a reboot to ssh
: "${D4_TRIAL_TMO:=1200}"                  # a trial's confirmation
: "${D4_RESET_TMO:=1800}"                  # the panic cases: until slot A answers, all resets included
: "${D4_LIVE_S:=600}"                      # live: how long the hang is watched for a reset
: "${D4_LIVE2_S:=300}"                     # live: the same after the power cut
STAGE=/data/fjarr-update/stage/d4          # the release's root and firmware, no capsule
NVK=/data/fjarr-test/Image-nvwdt
APPLIER=/usr/lib/fjarr/applier
M=/data/fjarr-update/applier
OUT=$LAB_DIR/d4/$(date -u +%Y%m%dT%H%M%SZ)-$case
mkdir -p "$OUT"
[[ -s $D4_REL/fjarr_$D4_NEW.swu && -s $D4_REL/fjarr_$D4_NEW.fw ]] ||
  { echo "d4.sh: no release $D4_NEW in $D4_REL (mkswu.sh $D4_NEW)" >&2; exit 2; }

case $case in
  setup) need=900 ;;
  panic | panic-nv) need=$((900 + D4_RESET_TMO + D4_TRIAL_TMO + 3 * D4_BOOT_TMO + LAB_CYCLE_WORST)) ;;
  live) need=$((900 + D4_LIVE_S + D4_LIVE2_S + D4_TRIAL_TMO + 3 * D4_BOOT_TMO + 2 * LAB_CYCLE_WORST)) ;;
  *) echo "d4.sh: no case '$case'" >&2; exit 2 ;;
esac
if [[ $case != setup && ${D4_PERSON_AT_BOX:-} != 1 ]]; then
  echo "d4.sh: a hung kernel may need the button: a person at the box (Q4), D4_PERSON_AT_BOX=1" >&2
  exit 2
fi
lab_init "d4-$case" "$need"
lab_plug_check
lab_desktop_guard
lab_backup_check || lab_die "no verified B3 backup (L2)"
lab_mpc -t 30 'systemctl is-active -q fjarr-net-rescue-dhcp.service' ||
  lab_die "the network rescue is not serving on the mini-PC (b6.sh net-up; L2)"
lab_uart_start "d4-$case"
[[ -n $LAB_UART_UNIT ]] || lab_die "no console capture: D4 reads the hang, and types the way out, on the console"
lab_use_fjarr_image

N=0 LAST=''
st() { # <step>: the applier's state into OUT/<n>-<step>.json (LAST), and its summary in the log
  N=$((N + 1))
  LAST=$OUT/$(printf %02d $N)-$1.json
  lab_box -t 60 "$APPLIER state" >"$LAST" 2>"$LAST.err" || { lab_log "[$N $1] no state: $(tail -n 2 "$LAST.err")"; return 1; }
  lab_log "[$N $1] $(jq -rc '{booted, slot: .booted_slot, trial: .is_trial, t: (.trial // null | if . then {version, slot, boots} else null end), last: (.last_trial // null | if . then {version, result} else null end), rs: [.slots[] | {(.slot): [.label, (to_entries[] | select(.key | startswith("RootfsStatusSlot")) | .value.value)]}] | add, pmc: (.pmc | [.rootfs_current, .rootfs_retry_count_a, .rootfs_retry_count_b])}' "$LAST" 2>/dev/null || head -c 300 "$LAST")"
}
j() { jq -r "$1" "$LAST"; }
on_box() { # <command>
  lab_lock_check
  lab_log "box: $1"
  lab_box -t 600 "$1" 2>&1 | sed 's/^/  | /' >&2
  return "${PIPESTATUS[0]}"
}
box_script() { # <bash script> [args...]: on the box, logged by its arguments only
  local cmd a
  cmd="bash -c $(_lab_sq "$1") d4"
  shift
  for a in "$@"; do cmd+=" $(_lab_sq "$a")"; done
  lab_lock_check
  lab_box -t 600 "$cmd" 2>&1 | sed 's/^/  | /' >&2
  return "${PIPESTATUS[0]}"
}
up_wait() { # <old boot id> <timeout s>: until the image answers with another boot id
  local t0=$SECONDS
  while ((SECONDS - t0 < $2)); do
    if lab_find_box && lab_box_bootid && [[ $LAB_BOOTID != "$1" ]]; then
      lab_deadman_touch || true
      lab_wait_ready 300 >/dev/null || true
      lab_log "up after $((SECONDS - t0)) s: boot ${LAB_BOOTID:0:8}"
      return 0
    fi
    lab_host_key_failed && lab_die "something answered on the box's MAC with another host key (L7)"
    sleep 5
  done
  return 1
}
reboot_wait() { # <command> <step>
  local before=$LAB_BOOTID rc=0
  lab_lock_check
  lab_log "box: $1"
  _lab_tmo 20 ssh -n "${LAB_SSH_BOX[@]}" "$LAB_BOX_USER@$LAB_BOX_IP" "$1" >/dev/null 2>&1 || rc=$?
  case $rc in 0 | 124 | 255) ;; *) lab_die "the box refused '$1' (exit $rc)" ;; esac
  up_wait "$before" "$D4_BOOT_TMO" || lab_die "nothing came back within $D4_BOOT_TMO s of '$1'"
  st "$2" || true
}
reboot_only() { # <command>: one that ends the session, without waiting for the box
  local rc=0
  lab_lock_check
  lab_log "box: $1"
  _lab_tmo 20 ssh -n "${LAB_SSH_BOX[@]}" "$LAB_BOX_USER@$LAB_BOX_IP" "$1" >/dev/null 2>&1 || rc=$?
  case $rc in 0 | 124 | 255) ;; *) lab_die "the box refused '$1' (exit $rc)" ;; esac
}
trial_end() { # <step>: until no trial is in progress
  local t0=$SECONDS
  while ((SECONDS - t0 < D4_TRIAL_TMO)); do
    if lab_find_box && lab_box_bootid; then
      lab_deadman_touch || true
      if lab_box -t 30 "test ! -e $M/trial" 2>/dev/null; then
        lab_wait_ready 300 >/dev/null || true
        st "$1"
        return 0
      fi
    fi
    sleep 10
  done
  lab_die "the trial did not end within $D4_TRIAL_TMO s"
}
expect() { # <what> <version> <slot>
  [[ $(j .booted) == "$2" && $(j .booted_slot) == "$3" ]] ||
    lab_die "$1: the box runs $(j .booted) from slot $(j .booted_slot), not $2 from slot $3"
  lab_log "PASS $1: $2 from slot $3"
}

# --- The console, read and typed on the mini-PC -------------------------------------------------
# The capture's log has one line per console line, prefixed with seconds since the capture began.
uart_lines() { lab_mpc_root -t 30 'wc -l < "/var/log/fjarr-lab-uart/$1.log"' "$LAB_UART_UNIT"; }
uart_wait() { # <ERE> <after line> <timeout s>: 0 when a console line after that line matches
  lab_lock_check
  lab_mpc_root -t $(($3 + 30)) 'f=/var/log/fjarr-lab-uart/$1.log; end=$((SECONDS + $4))
while ((SECONDS < end)); do tail -n +"$(($3 + 1))" "$f" | grep -aqE -- "$2" && exit 0; sleep 2; done; exit 1' \
    "$LAB_UART_UNIT" "$1" "$2" "$3"
}
uart_grep() { # <ERE> <after line>: the matching console lines after that line
  lab_mpc_root -t 30 'tail -n +"$(($3 + 1))" "/var/log/fjarr-lab-uart/$1.log" | grep -aE -- "$2" || true' \
    "$LAB_UART_UNIT" "$1" "$2"
}
_D4_TYPE_PY='import os, sys
fd = os.open(sys.argv[1], os.O_WRONLY | os.O_NOCTTY)
os.write(fd, sys.argv[2].encode())
os.close(fd)'
menu_type() { # <key> <after line> <timeout s>: at the next "fjarr d4" menu after that line, type the key
  lab_lock_check
  lab_mpc_root -t $(($3 + 30)) 'f=/var/log/fjarr-lab-uart/$1.log; end=$((SECONDS + $4))
while ((SECONDS < end)); do
  if tail -n +"$(($2 + 1))" "$f" | grep -aq "Press 0-1 to boot selection"; then python3 -Ic "$6" "$5" "$3"; exit 0; fi
  sleep 0.5
done; exit 1' "$LAB_UART_UNIT" "$2" "$1" "$3" "$LAB_UART" "$_D4_TYPE_PY"
}

# --- The hang ----------------------------------------------------------------------------------
start_case() {
  { lab_find_box && lab_box_bootid; } || lab_die "the Fjarr image does not answer on the box's MAC"
  lab_deadman_touch
  st start
  expect "$case: start" "$D4_BASE" a
  [[ $(j .is_trial) == false ]] || lab_die "$case: a trial is in progress"
  if [[ $(j .verified_this_boot) != true ]]; then
    on_box "$APPLIER boot-verify" || lab_die "boot-verify failed"
    st verified
  fi
  on_box 'rm -f /data/fjarr-test/fail-health /data/fjarr-test/panic /data/fjarr-test/hold-health'
  on_box "mkdir -p $STAGE && ln -sf /data/fjarr-update/stage/$D4_NEW/fjarr_$D4_NEW.swu $STAGE/fjarr_$D4_NEW.swu && cp /data/fjarr-update/stage/$D4_NEW/fjarr_$D4_NEW.fw $STAGE/ && rm -f $STAGE/fjarr_$D4_NEW.cap && ls -l $STAGE" ||
    lab_die "no stage for $D4_NEW (e-capsule.sh setup stages it)"
  lab_box -t 120 "sha256sum $STAGE/fjarr_$D4_NEW.swu" | grep -q "^$(head -n 1 "$D4_REL/SHA256SUMS" | cut -d' ' -f1) " ||
    lab_die "the staged package is not out/swu/$D4_NEW"
  lab_box -t 30 "test -e $M/chain-b && grep -qx state=complete $M/chain-b && grep -qx fw=$(cat "$D4_REL/fjarr_$D4_NEW.fw") $M/chain-b" ||
    lab_die "chain B is not complete on the release's firmware: set-next would want a capsule"
}
# The menu: entry 0 (default) the hang, entry 1 the slot's own entry as installed. The hang keeps
# the slot's arguments but its root ones and panic=10; L4TLauncher appends only bl_prof_* after.
_D4_HANG_SH='set -eu
kernel=$1 extra=$2 nvk=$3
m=$(mktemp -d); mount /dev/disk/by-partlabel/APP_b "$m"; trap '\''umount "$m"; rmdir "$m"'\'' EXIT
c=$m/boot/extlinux/extlinux.conf
cp "$c" "$c.installed"
a=$(sed -n "s/^[[:space:]]*APPEND //p" "$c.installed" | head -n 1)
fdt=$(sed -n "s/^[[:space:]]*FDT //p" "$c.installed" | head -n 1)
[ -n "$a" ] && [ -n "$fdt" ] || { echo "no APPEND or FDT in the installed extlinux.conf"; exit 1; }
h=""; for w in $a; do case $w in root=* | rootfstype=* | rootflags=* | panic=*) ;; *) h="$h$w " ;; esac; done
[ "$kernel" = /boot/Image ] || { cp "$nvk" "$m$kernel"; sync -f "$m"; }
{ printf "TIMEOUT 100\nMENU TITLE fjarr d4\nDEFAULT hang\nLABEL hang\n\tMENU LABEL d4 hang\n\tLINUX %s\n\tFDT %s\n\tAPPEND %sroot=/dev/fjarr-hang %spanic=0\n" "$kernel" "$fdt" "$h" "$extra"
  sed -n "/^LABEL /,\$p" "$c.installed"; } > "$c.new"
mv "$c.new" "$c"; sync -f "$m"
grep -v "^[[:space:]]*$" "$c"; ls -l "$m/boot"'
hang_slot_b() { # <kernel in the slot's /boot> <extra arguments, each followed by a space>
  lab_log "slot B's extlinux.conf: entry 0 the hang ($1, ${2}panic=0, no initrd), entry 1 the slot's own"
  box_script "$_D4_HANG_SH" "$1" "$2" "$NVK" || lab_die "could not write slot B's menu"
}
install_hang() { # <kernel> <extra>
  on_box "$APPLIER install $D4_NEW $STAGE" >/dev/null 2>&1 || lab_die "install $D4_NEW failed"
  st installed
  hang_slot_b "$1" "$2"
  on_box "$APPLIER set-next $D4_NEW" || lab_die "set-next $D4_NEW failed"
  st set-next
  [[ $(j .capsule_staged) == false ]] || lab_die "set-next staged a capsule"
}
hang_boot() { # [<after line>]: the reboot into the hang; HANG_AT: the console line it started at
  local from=${1:-}
  [[ -n $from ]] || { from=$(uart_lines); reboot_only 'systemctl reboot'; }
  uart_wait 'Kernel command line:.*root=/dev/fjarr-hang' "$from" 300 ||
    lab_die "the hang entry did not start within 300 s (console: .lab/logs, after line $from)"
  HANG_AT=$from
  lab_log "the hang entry started: $(uart_grep 'Kernel command line:' "$from" | tail -n 1 | cut -c1-60)…"
}
escape() { # <why>: power cut, entry 1 on the console, the slot's own boot; its trial confirms; back to A
  local from
  lab_log "the way out ($1): a power cut, then entry 1 at L4TLauncher's menu"
  from=$(uart_lines)
  lab_power_cycle
  if ! menu_type 1 "$from" 300; then
    lab_die "no fjarr d4 menu within 300 s of the power cut: L7, look at the console (the button: Q4)"
  fi
  uart_wait 'Kernel command line:.*root=/dev/fjarr/root' "$from" 120 || lab_die "entry 1 did not start"
  lab_log "entry 1 started: the slot's own boot"
  up_wait "$LAB_BOOTID" "$D4_BOOT_TMO" || lab_die "slot B's own boot did not answer within $D4_BOOT_TMO s"
  trial_end escaped
  [[ $(j .last_trial.result) == good ]] || lab_die "the trial after the way out ended '$(j .last_trial.result)'"
  expect "the way out" "$D4_NEW" b
  reboot_wait "$APPLIER rollback-now" rollback
  expect "rollback-now" "$D4_BASE" a
}
summary() { # <after line>: the hang's boots, panics and kernel watchdog lines on the console
  lab_log "console since the reboot: $(uart_grep 'fjarr d4' "$1" | grep -c 'MENU\|fjarr d4' || true) menus, $(uart_grep 'Kernel panic' "$1" | grep -c . || true) panics"
  uart_grep 'fjarr d4|Kernel command line|Kernel panic|Waiting for root|tegra186|watchdog|L4TLauncher|reset|Rebooting' "$1" | cut -c1-200 >"$OUT/console-summary.txt" || true
  sed 's/^/  ~ /' "$OUT/console-summary.txt" | tail -n 60 >&2
}

c_setup() {
  [[ -s $D4_NV_IMAGE ]] || lab_die "no unpatched kernel at $D4_NV_IMAGE (yocto/build.sh --unpatched-kernel)"
  { lab_find_box && lab_box_bootid; } || lab_die "the Fjarr image does not answer on the box's MAC"
  lab_deadman_touch
  lab_lock_check
  _lab_tmo 600 ssh "${LAB_SSH_BOX[@]}" "$LAB_BOX_USER@$LAB_BOX_IP" "mkdir -p /data/fjarr-test && cat > $NVK.part && sync -f /data/fjarr-test && mv $NVK.part $NVK" <"$D4_NV_IMAGE" ||
    lab_die "could not copy the unpatched kernel"
  lab_box -t 60 "sha256sum $NVK" | grep -q "^$(sha256sum <"$D4_NV_IMAGE" | cut -d' ' -f1) " || lab_die "the kernel on the box does not match"
  strings "$D4_NV_IMAGE" | grep -m1 -aE '^Linux version' | sed 's/^/  | /' >&2 || true
  lab_log "the unpatched kernel is at $NVK"
}

c_panic() { # <kernel>
  local before
  start_case
  install_hang "$1" ""
  before=$LAB_BOOTID
  hang_boot
  uart_wait 'Kernel panic' "$HANG_AT" 300 || lab_die "no panic within 300 s of the hang entry"
  lab_log "panicked: $(uart_grep 'Kernel panic' "$HANG_AT" | head -n 1 | cut -c1-140)"
  if up_wait "$before" "$D4_RESET_TMO"; then
    trial_end fell-back
    summary "$HANG_AT"
    [[ $(j .last_trial.result) == fell-back ]] || lab_die "the trial ended '$(j .last_trial.result)', not fell-back"
    expect "a trial kernel that panics before userspace falls back by itself" "$D4_BASE" a
  else
    summary "$HANG_AT"
    lab_log "RESULT $case: no recovery within $D4_RESET_TMO s of the panic: hung"
    escape "hung after a panic"
  fi
}

c_live() {
  local before from t0
  start_case
  install_hang /boot/Image "rootwait "
  before=$LAB_BOOTID
  hang_boot
  uart_wait 'Waiting for root device' "$HANG_AT" 300 || lab_die "the kernel did not wait for the root"
  lab_log "waiting for the root; watching $D4_LIVE_S s for a reset"
  t0=$SECONDS
  while ((SECONDS - t0 < D4_LIVE_S)); do
    lab_lock_check
    sleep 30
    if [[ $(uart_grep 'Kernel command line:' "$HANG_AT" | grep -c . || true) -gt 1 ]]; then
      summary "$HANG_AT"
      lab_log "RESULT live: the box restarted $((SECONDS - t0)) s into the hang"
      if up_wait "$before" "$D4_RESET_TMO"; then
        trial_end fell-back
        expect "a live hang ended by a reset, then fell back" "$D4_BASE" a
        return 0
      fi
      escape "restarted, then hung"
      return 0
    fi
    ! { lab_find_box && lab_box_bootid && [[ $LAB_BOOTID != "$before" ]]; } || lab_die "the box answers during the hang"
  done
  summary "$HANG_AT"
  lab_log "RESULT live: no reset in $D4_LIVE_S s with the kernel alive (the gap, docs/18 #48)"
  from=$(uart_lines)
  lab_power_cycle
  if uart_wait 'Kernel command line:.*root=/dev/fjarr-hang' "$from" 300; then
    lab_log "RESULT live: after a power cut, slot B's hang again (the count back by the cold boot)"
    uart_wait 'Waiting for root device' "$from" 300 || lab_die "after the cut: the kernel did not wait for the root"
    sleep "$D4_LIVE2_S"
    [[ $(uart_grep 'Kernel command line:' "$from" | grep -c . || true) -eq 1 ]] ||
      lab_log "RESULT live: after the cut, a restart within $D4_LIVE2_S s"
    summary "$from"
    escape "the hang after the power cut"
  elif up_wait "$before" "$D4_BOOT_TMO"; then
    st after-cut
    lab_log "RESULT live: after a power cut, the box came up as $(j .booted) from slot $(j .booted_slot)"
    trial_end after-cut-end
  else
    lab_die "after the power cut: neither the hang nor the image (L7, the console)"
  fi
}

case $case in
  setup) c_setup ;;
  panic) c_panic /boot/Image ;;
  panic-nv) c_panic /boot/Image-nvwdt ;;
  live) c_live ;;
esac
st end || true
lab_log "d4 $case done: ${OUT#"$LAB_HOME"/}"
