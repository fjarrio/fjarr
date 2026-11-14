#!/usr/bin/env bash
# shellcheck disable=SC2016 # the remote scripts take their arguments as $1, $2 there
# d3.sh: phase D3, the applier's rehearsal on the box (slice 8.0).
# spec: docs/32-fleet-ota.md#jetson-test-image (phase D; rules L1, L2, L6, L7); README.md, "D3";
# applier/README.md, "End to end"
#
#   LAB_DESKTOP_OK=1 d3.sh setup|ab|health|panic|badroot|powercut|all
#
# Each case starts on release 1.0 in slot A, committed, and ends there; the applier's state is
# recorded before and after every boot (.lab/d3/<time>-<case>/<n>-<step>.json). The release
# directory comes from mkswu.sh (out/swu/<version>/). Nothing here writes the QSPI.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=lib/lab.sh
. "$here/lib/lab.sh"

case=${1:?usage: d3.sh setup|ab|health|panic|badroot|powercut|all}
: "${D3_BASE:=1.0}"                       # the release flashed in both slots
: "${D3_NEW:=1.2}"                        # the release the cases install
: "${D3_REL:=$LAB_HOME/out/swu/$D3_NEW}"  # its release directory (mkswu.sh)
: "${D3_BOOT_TMO:=600}"                   # a reboot to ssh
: "${D3_TRIAL_TMO:=1200}"                 # a trial's confirmation or fall-back, reboots included
STAGE=/data/fjarr-update/stage/$D3_NEW
APPLIER=/usr/lib/fjarr/applier
OUT=$LAB_DIR/d3/$(date -u +%Y%m%dT%H%M%SZ)-$case
mkdir -p "$OUT"
[[ -s $D3_REL/fjarr_$D3_NEW.swu && -s $D3_REL/fjarr_$D3_NEW.fw ]] ||
  { echo "d3.sh: no release $D3_NEW in $D3_REL (mkswu.sh $D3_NEW)" >&2; exit 2; }

case $case in
  setup) need=900 ;;
  ab | health) need=$((600 + 4 * D3_BOOT_TMO + 2 * D3_TRIAL_TMO)) ;;
  panic | badroot | powercut) need=$((600 + 2 * D3_BOOT_TMO + D3_TRIAL_TMO + LAB_CYCLE_WORST)) ;;
  all) need=$((1800 + 12 * D3_BOOT_TMO + 7 * D3_TRIAL_TMO + LAB_CYCLE_WORST)) ;;
  *) echo "d3.sh: no case '$case'" >&2; exit 2 ;;
esac
lab_init "d3-$case" "$need"
lab_plug_check
lab_desktop_guard
lab_backup_check || lab_die "no verified B3 backup (L2)"
lab_mpc -t 30 'systemctl is-active -q fjarr-net-rescue-dhcp.service' ||
  lab_die "the network rescue is not serving on the mini-PC (b6.sh net-up; L2)"
lab_uart_start "d3-$case"
lab_use_fjarr_image

N=0 LAST='' DIR=$OUT
st() { # <step>: the applier's state into DIR/<n>-<step>.json (LAST), and its summary in the log
  N=$((N + 1))
  LAST=$DIR/$(printf %02d $N)-$1.json
  lab_box -t 60 "$APPLIER state" >"$LAST" 2>"$LAST.err" || { lab_log "[$N $1] no state: $(tail -n 2 "$LAST.err")"; return 1; }
  lab_log "[$N $1] $(jq -rc '{booted, slot: .booted_slot, trial: .is_trial, t: (.trial // null | if . then {version, slot, boots} else null end), last: (.last_trial // null | if . then {version, result} else null end), rs: [.slots[] | {(.slot): [.label, .retry_count, (to_entries[] | select(.key | startswith("RootfsStatusSlot")) | .value.value)]}] | add}' "$LAST" 2>/dev/null || head -c 300 "$LAST")"
}
j() { jq -r "$1" "$LAST"; } # <jq filter> on the last state
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
on_box() { # <command>
  lab_lock_check
  lab_log "box: $1"
  lab_box -t 600 "$1" 2>&1 | sed 's/^/  | /' >&2
  return "${PIPESTATUS[0]}"
}
reboot_wait() { # <command> <step>: a command that ends the session, the next boot, its state
  local before=$LAB_BOOTID rc=0
  lab_lock_check
  lab_log "box: $1"
  _lab_tmo 20 ssh -n "${LAB_SSH_BOX[@]}" "$LAB_BOX_USER@$LAB_BOX_IP" "$1" >/dev/null 2>&1 || rc=$?
  case $rc in 0 | 124 | 255) ;; *) lab_die "the box refused '$1' (exit $rc)" ;; esac
  up_wait "$before" "$D3_BOOT_TMO" || lab_die "nothing came back within $D3_BOOT_TMO s of '$1'"
  # A failing trial reboots on its own within seconds (health, panic, a root that never mounts):
  # its state may be gone before it can be read, and trial_end follows it.
  st "$2" || lab_log "($2: the box went down again before its state could be read)"
}
trial_end() { # <step>: until no trial is in progress (its last_trial written), reboots included
  local t0=$SECONDS
  while ((SECONDS - t0 < D3_TRIAL_TMO)); do
    if lab_find_box && lab_box_bootid; then
      lab_deadman_touch || true
      if lab_box -t 30 "test ! -e /data/fjarr-update/applier/trial" 2>/dev/null; then
        lab_wait_ready 300 >/dev/null || true
        st "$1"
        return 0
      fi
    fi
    sleep 10
  done
  lab_die "the trial did not end within $D3_TRIAL_TMO s"
}
expect() { # <what> <version> <slot>: the box runs that release from that slot
  [[ $(j .booted) == "$2" && $(j .booted_slot) == "$3" ]] ||
    lab_die "$1: the box runs $(j .booted) from slot $(j .booted_slot), not $2 from slot $3"
  lab_log "PASS $1: $2 from slot $3"
}
start_case() { # <case>: on 1.0 in slot A, committed
  DIR=$OUT/$1 N=0
  mkdir -p "$DIR"
  { lab_find_box && lab_box_bootid; } || lab_die "the Fjarr image does not answer on the box's MAC"
  lab_deadman_touch
  st start
  expect "$1: start" "$D3_BASE" a
  [[ $(j .is_trial) == false ]] || lab_die "$1: a trial is in progress"
  if [[ $(j .verified_this_boot) != true ]]; then
    # A boot that came up while a test had fjarr-boot-verify masked (d2.sh): the unit's own verb.
    on_box "$APPLIER boot-verify" || lab_die "$1: boot-verify failed"
    st verified
    [[ $(j .verified_this_boot) == true ]] || lab_die "$1: this boot is still not verified"
  fi
  on_box 'rm -f /data/fjarr-test/fail-health /data/fjarr-test/panic /data/fjarr-test/hold-health'
}
install_new() { on_box "$APPLIER install $D3_NEW $STAGE" || lab_die "install $D3_NEW failed"; st installed; }
set_next() { on_box "$APPLIER set-next $D3_NEW" || lab_die "set-next $D3_NEW failed"; st set-next; }

c_setup() {
  local f
  { lab_find_box && lab_box_bootid; } || lab_die "the Fjarr image does not answer on the box's MAC"
  lab_deadman_touch
  on_box "mkdir -p $STAGE /data/fjarr-test"
  for f in "fjarr_$D3_NEW.swu" "fjarr_$D3_NEW.fw"; do
    lab_lock_check
    _lab_tmo 600 ssh "${LAB_SSH_BOX[@]}" "$LAB_BOX_USER@$LAB_BOX_IP" "cat > $STAGE/$f.part && sync -f $STAGE && mv $STAGE/$f.part $STAGE/$f" <"$D3_REL/$f" ||
      lab_die "could not copy $f to the box"
  done
  on_box "cd $STAGE && sha256sum fjarr_$D3_NEW.swu" || true
  grep -q "$(cut -d' ' -f1 "$D3_REL/SHA256SUMS")" <(lab_box -t 120 "sha256sum $STAGE/fjarr_$D3_NEW.swu") ||
    lab_die "the package on the box does not match"
  # Chain B's marker: D1 wrote both chains with 36.5.2 and D2 booted chain B on it, but only a
  # verified boot on a chain writes its marker, and D2 verified chain B by hand.
  on_box "m=/data/fjarr-update/applier; mkdir -p \$m; [ -e \$m/chain-b ] || { printf 'fw=$(cat "$D3_REL/fjarr_$D3_NEW.fw")\nstate=complete\n' > \$m/chain-b.tmp && sync -f \$m && mv \$m/chain-b.tmp \$m/chain-b && sync -f \$m; }; cat \$m/chain-a \$m/chain-b"
  st setup
}

c_ab() {
  start_case ab
  install_new
  reboot_wait 'systemctl reboot' installed-reboot
  expect "installed but not activated" "$D3_BASE" a
  set_next
  reboot_wait 'systemctl reboot' trial
  expect "set-next" "$D3_NEW" b
  trial_end confirmed
  [[ $(j .last_trial.result) == good ]] || lab_die "the trial ended '$(j .last_trial.result)', not good"
  expect "the trial confirmed" "$D3_NEW" b
  reboot_wait 'systemctl reboot' committed-reboot
  expect "the committed release after a reboot" "$D3_NEW" b
  reboot_wait "$APPLIER rollback-now" rollback
  expect "rollback-now" "$D3_BASE" a
}

c_health() {
  start_case health
  install_new
  on_box 'touch /data/fjarr-test/fail-health'
  set_next
  reboot_wait 'systemctl reboot' trial
  trial_end rolled-back
  on_box 'rm -f /data/fjarr-test/fail-health'
  [[ $(j .last_trial.result) == fell-back ]] || lab_die "the failed trial ended '$(j .last_trial.result)', not fell-back"
  expect "a failed health check rolls back" "$D3_BASE" a
  install_new
  set_next
  reboot_wait 'systemctl reboot' trial-2
  expect "the next update into the same slot boots" "$D3_NEW" b
  trial_end confirmed-2
  [[ $(j .last_trial.result) == good ]] || lab_die "the second trial ended '$(j .last_trial.result)', not good"
  reboot_wait "$APPLIER rollback-now" rollback
  expect "rollback-now" "$D3_BASE" a
}

c_panic() {
  start_case panic
  install_new
  on_box 'touch /data/fjarr-test/panic'
  set_next
  reboot_wait 'systemctl reboot' trial-or-after
  trial_end rolled-back
  [[ $(j .last_trial.result) == fell-back ]] || lab_die "the panicked trial ended '$(j .last_trial.result)', not fell-back"
  expect "a panic on the trial boot rolls back" "$D3_BASE" a
}

c_badroot() {
  start_case badroot
  install_new
  on_box 'dd if=/dev/zero of=/dev/disk/by-partlabel/APP_b bs=4096 count=1 conv=fsync status=none && echo "slot B: superblock zeroed"'
  set_next
  lab_log "slot B cannot mount its root: L4TLauncher's retries, then slot A (several boots)"
  reboot_wait 'systemctl reboot' fell-back-boot
  trial_end fell-back
  [[ $(j .last_trial.result) == fell-back ]] || lab_die "the trial ended '$(j .last_trial.result)', not fell-back"
  expect "a slot whose root never mounts falls back" "$D3_BASE" a
}

c_powercut() {
  local before
  start_case powercut
  install_new
  on_box 'touch /data/fjarr-test/hold-health'
  set_next
  reboot_wait 'systemctl reboot' trial-held
  expect "the trial, held" "$D3_NEW" b
  [[ $(j .is_trial) == true ]] || lab_die "the held boot is not a trial"
  before=$LAB_BOOTID
  lab_lock_check
  lab_power_cycle
  up_wait "$before" "$D3_BOOT_TMO" || lab_die "nothing came back after the power cycle"
  trial_end rolled-back
  on_box 'rm -f /data/fjarr-test/hold-health'
  [[ $(j .last_trial.result) == fell-back ]] || lab_die "the cut trial ended '$(j .last_trial.result)', not fell-back"
  expect "a power cut during the trial rolls back" "$D3_BASE" a
}

if [[ $case == all ]]; then
  for c in setup ab health panic badroot powercut; do "c_$c"; done
else
  "c_$case"
fi
lab_log "d3 $case done: ${OUT#"$LAB_HOME"/}"
