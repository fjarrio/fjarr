#!/usr/bin/env bash
# shellcheck disable=SC2016 # the remote scripts take their arguments as $1, $2 there
# e-capsule.sh: phase E, the bootloader capsule on the box (slice 8.0), cases E1 to E3.
# spec: docs/32-fleet-ota.md#jetson-test-image (phase E; rules L1, L2, L4, L5, L7; Q4);
# README.md, "E"; applier/README.md, "Refusals"
#
#   LAB_DESKTOP_OK=1 e-capsule.sh setup|e3|e2
#   LAB_DESKTOP_OK=1 E_PERSON_AT_BOX=1 e-capsule.sh e1
#
#   setup  stage release E_NEW (out/swu/<v>/ from MKSWU_CAP=… mkswu.sh, with its capsule) and the
#          refusal releases e3 uses
#   e3     the applier's pre-flight refusals on the box, before it stages anything: a stale
#          BootChainFwStatus (I15) or BootChainFwNext (I16), BootNext set (I17), a capsule pending
#          (I19), a release below the ESRT's lowest supported version (N13), a capsule changed
#          since install (N12). No reboot, and no capsule: the refusal releases carry a file that
#          is not one, so even a staged copy could not reach the QSPI
#   e1     the capsule as the slot switch: chain B's marker removed, so set-next stages the
#          capsule, UEFI writes chain B and boots it with slot B; the capsule status 1, the
#          firmware 36.5.2, the trial confirmed. Q4: the first capsule writes the QSPI
#   e2     after e1: rollback-now gives chain A with slot A; forward again with no capsule
#          (chain B now complete on the release's firmware); back to the baseline
#
# Each case starts on E_BASE in slot A, committed (e2: on E_NEW in slot B after a capsule trial)
# and records the applier's state at every step (.lab/e/<time>-<case>/<n>-<step>.json). L4: the
# capsule is checked by mkswu.sh (capsule-check.py) and again here by sha256, on the box.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=lib/lab.sh
. "$here/lib/lab.sh"

case=${1:?usage: e-capsule.sh setup|e3|e1|e2}
: "${E_BASE:=1.0}"                       # the release flashed in both slots
: "${E_NEW:=1.2}"                        # the release with the capsule
: "${E_REL:=$LAB_HOME/out/swu/$E_NEW}"   # its release directory (MKSWU_CAP=… mkswu.sh)
: "${E_BOOT_TMO:=600}"                   # a reboot to ssh
: "${E_CAPSULE_TMO:=1800}"               # a reboot that applies a capsule, to ssh
: "${E_TRIAL_TMO:=1200}"                 # a trial's confirmation or fall-back
STAGE=/data/fjarr-update/stage/$E_NEW
LOW=/data/fjarr-update/stage/e3-lowest   # E_NEW's root, firmware 36.4.4, a non-capsule
CHANGED=/data/fjarr-update/stage/e3-changed
APPLIER=/usr/lib/fjarr/applier
M=/data/fjarr-update/applier
NV=781e084c-a330-417c-b678-38e696380cb9   # gNVIDIAPublicVariableGuid
EFIG=8be4df61-93ca-11d2-aa0d-00e098032b8c # EFI_GLOBAL_VARIABLE
OUT=$LAB_DIR/e/$(date -u +%Y%m%dT%H%M%SZ)-$case
mkdir -p "$OUT"
[[ -s $E_REL/fjarr_$E_NEW.swu && -s $E_REL/fjarr_$E_NEW.fw && -s $E_REL/fjarr_$E_NEW.cap ]] ||
  { echo "e-capsule.sh: no release $E_NEW with a capsule in $E_REL (MKSWU_CAP=<capsule> mkswu.sh $E_NEW)" >&2; exit 2; }
cap_sha=$(sha256sum <"$E_REL/fjarr_$E_NEW.cap" | cut -d' ' -f1)
grep -q "^$cap_sha  fjarr_$E_NEW.cap$" "$E_REL/SHA256SUMS" ||
  { echo "e-capsule.sh: the capsule is not the one mkswu.sh checked (SHA256SUMS)" >&2; exit 2; }

case $case in
  setup) need=1200 ;;
  e3) need=1800 ;;
  e1) need=$((900 + E_CAPSULE_TMO + E_TRIAL_TMO + 2 * E_BOOT_TMO)) ;;
  e2) need=$((900 + 4 * E_BOOT_TMO + 2 * E_TRIAL_TMO)) ;;
  *) echo "e-capsule.sh: no case '$case'" >&2; exit 2 ;;
esac
if [[ $case == e1 && ${E_PERSON_AT_BOX:-} != 1 ]]; then
  echo "e-capsule.sh: e1's capsule writes the QSPI: a person at the box (Q4), E_PERSON_AT_BOX=1" >&2
  exit 2
fi
lab_init "e-$case" "$need"
lab_plug_check
lab_desktop_guard
lab_backup_check || lab_die "no verified B3 backup (L2)"
lab_mpc -t 30 'systemctl is-active -q fjarr-net-rescue-dhcp.service' ||
  lab_die "the network rescue is not serving on the mini-PC (b6.sh net-up; L2)"
lab_uart_start "e-$case"
lab_use_fjarr_image

N=0 LAST=''
st() { # <step>: the applier's state into OUT/<n>-<step>.json (LAST), and its summary in the log
  N=$((N + 1))
  LAST=$OUT/$(printf %02d $N)-$1.json
  lab_box -t 60 "$APPLIER state" >"$LAST" 2>"$LAST.err" || { lab_log "[$N $1] no state: $(tail -n 2 "$LAST.err")"; return 1; }
  lab_log "[$N $1] $(jq -rc '{booted, slot: .booted_slot, trial: .is_trial, bios: .bios_version, cap: [.capsule_staged, .capsule_pending, .capsule_status], chains: [.chains[] | "\(.chain):\(.fw)/\(.marker.state // "-")"], fw: .efivars.BootChainFwCurrent.value, last: (.last_trial // null | if . then {version, result} else null end)}' "$LAST" 2>/dev/null || head -c 300 "$LAST")"
}
j() { jq -r "$1" "$LAST"; }
on_box() { # <command>
  lab_lock_check
  lab_log "box: $1"
  lab_box -t 600 "$1" 2>&1 | sed 's/^/  | /' >&2
  return "${PIPESTATUS[0]}"
}
up_wait() { # <old boot id> <timeout s>: until the image answers with another boot id
  local t0=$SECONDS
  while ((SECONDS - t0 < $2)); do
    if lab_find_box && lab_box_bootid && [[ $LAB_BOOTID != "$1" ]]; then
      lab_deadman_touch || true
      lab_wait_ready 300 >/dev/null || true
      UP_S=$((SECONDS - t0))
      lab_log "up after $UP_S s: boot ${LAB_BOOTID:0:8}"
      return 0
    fi
    lab_host_key_failed && lab_die "something answered on the box's MAC with another host key (L7)"
    sleep 5
  done
  return 1
}
reboot_wait() { # <command> <step> [<timeout s>]: a command that ends the session, the next boot
  local before=$LAB_BOOTID rc=0
  lab_lock_check
  lab_log "box: $1"
  _lab_tmo 20 ssh -n "${LAB_SSH_BOX[@]}" "$LAB_BOX_USER@$LAB_BOX_IP" "$1" >/dev/null 2>&1 || rc=$?
  case $rc in 0 | 124 | 255) ;; *) lab_die "the box refused '$1' (exit $rc)" ;; esac
  up_wait "$before" "${3:-$E_BOOT_TMO}" || lab_die "nothing came back within ${3:-$E_BOOT_TMO} s of '$1'"
  st "$2" || lab_log "($2: the box went down again before its state could be read)"
}
trial_end() { # <step>: until no trial is in progress, reboots included
  local t0=$SECONDS
  while ((SECONDS - t0 < E_TRIAL_TMO)); do
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
  lab_die "the trial did not end within $E_TRIAL_TMO s"
}
expect() { # <what> <version> <slot>
  [[ $(j .booted) == "$2" && $(j .booted_slot) == "$3" ]] ||
    lab_die "$1: the box runs $(j .booted) from slot $(j .booted_slot), not $2 from slot $3"
  lab_log "PASS $1: $2 from slot $3"
}
check() { # <what> <jq condition on the last state>
  jq -e "$2" "$LAST" >/dev/null || lab_die "$1: not so ($2)"
  lab_log "PASS $1"
}
untouched() { # <what>: nothing staged, no trial, no switch pending
  check "$1: nothing staged" '.capsule_staged == false and .capsule_pending == false and .is_trial == false and .trial == null and .efivars.BootChainFwNext == null and .efivars.BootNext == null'
}
start_on() { # <case> <version> <slot>
  { lab_find_box && lab_box_bootid; } || lab_die "the Fjarr image does not answer on the box's MAC"
  lab_deadman_touch
  st start
  expect "$1: start" "$2" "$3"
  [[ $(j .is_trial) == false ]] || lab_die "$1: a trial is in progress"
  if [[ $(j .verified_this_boot) != true ]]; then
    on_box "$APPLIER boot-verify" || lab_die "$1: boot-verify failed"
    st verified
  fi
  on_box 'rm -f /data/fjarr-test/fail-health /data/fjarr-test/panic /data/fjarr-test/hold-health'
}
refused() { # <case id> <verb and arguments>: exit 2 from the applier, and nothing staged
  local rc=0
  lab_lock_check
  lab_log "box: $APPLIER $2 (expect $1's refusal)"
  lab_box -t 120 "$APPLIER $2" >"$OUT/$1.out" 2>&1 || rc=$?
  sed 's/^/  | /' "$OUT/$1.out" >&2
  ((rc == 2)) || lab_die "$1: '$2' exited $rc, not the refusal (2)"
  st "$1-refused"
  untouched "$1"
}
put_var() { # <name> <guid> <attributes hex, 8 digits LE> <value hex LE>: write one UEFI variable
  # One write() is one SetVariable: the bytes into a file, then one dd, as the applier does.
  local hex=$3$4 esc='' i
  for ((i = 0; i < ${#hex}; i += 2)); do esc+=$(printf '\\0%03o' "0x${hex:i:2}"); done
  on_box "f=/sys/firmware/efi/efivars/$1-$2 t=\$(mktemp); printf '%b' '$esc' > \$t; [ ! -e \$f ] || chattr -i \$f
dd if=\$t of=\$f bs=$((${#hex} / 2)) count=1 status=none; rm -f \$t; od -An -v -tx1 \$f"
}
stage() { # <dir on the box> <local file> [<name on the box>]
  lab_lock_check
  _lab_tmo 600 ssh "${LAB_SSH_BOX[@]}" "$LAB_BOX_USER@$LAB_BOX_IP" \
    "mkdir -p $1 && cat > $1/${3:-${2##*/}}.part && sync -f $1 && mv $1/${3:-${2##*/}}.part $1/${3:-${2##*/}}" <"$2" ||
    lab_die "could not copy ${2##*/} to $1"
}

c_setup() {
  local f
  { lab_find_box && lab_box_bootid; } || lab_die "the Fjarr image does not answer on the box's MAC"
  lab_deadman_touch
  for f in swu fw cap; do stage "$STAGE" "$E_REL/fjarr_$E_NEW.$f"; done
  lab_box -t 120 "cd $STAGE && sha256sum fjarr_$E_NEW.swu fjarr_$E_NEW.cap" | diff - "$E_REL/SHA256SUMS" >&2 ||
    lab_die "the release on the box is not out/swu/$E_NEW (SHA256SUMS)"
  # The refusal releases: E_NEW's root, so install takes them, with a file that is not a capsule.
  printf 'not a capsule: e-capsule.sh e3\n' >"$OUT/not-a-capsule"
  printf '36.4.4\n' >"$OUT/fw-36.4.4"
  for d in "$LOW" "$CHANGED"; do
    on_box "mkdir -p $d && ln -sf $STAGE/fjarr_$E_NEW.swu $d/fjarr_$E_NEW.swu"
    stage "$d" "$OUT/not-a-capsule" "fjarr_$E_NEW.cap"
  done
  stage "$LOW" "$OUT/fw-36.4.4" "fjarr_$E_NEW.fw"
  stage "$CHANGED" "$E_REL/fjarr_$E_NEW.fw"
  on_box "mkdir -p /data/fjarr-test; cat $M/chain-a $M/chain-b; ls -l $STAGE $LOW $CHANGED"
  st setup
}

e3_undo() { # what e3 sets, put back: on its way and when it stops (LAB_DIE_HOOK)
  lab_box -t 120 "for v in BootChainFwStatus-$NV BootChainFwNext-$NV OsIndications-$EFIG; do f=/sys/firmware/efi/efivars/\$v; [ ! -e \$f ] || { chattr -i \$f; rm \$f; }; done
efibootmgr -q -N 2>/dev/null; [ ! -e $M/chain-b.e3 ] || mv $M/chain-b.e3 $M/chain-b; sync -f $M" >&2 || true
}
refused_for() { # <case id> <words the refusal names> <verb and arguments>
  refused "$1" "$3"
  grep -qi "$2" "$OUT/$1.out" || lab_die "$1: refused, but not for '$2': $(cat "$OUT/$1.out")"
}

c_e3() {
  local cur
  start_on e3 "$E_BASE" a
  untouched "e3 start"
  check "e3 start: no status, no capsule request" '.efivars.BootChainFwStatus == null and .efivars.OsIndications == null'
  on_box "test -s $LOW/fjarr_$E_NEW.cap && test -s $CHANGED/fjarr_$E_NEW.cap" || lab_die "no refusal releases: e-capsule.sh setup"
  LAB_DIE_HOOK=e3_undo
  # I15: a switch's status left set. 12, STATUS_ERROR_BOOT_CHAIN_FAILED; NV, BS, RT.
  put_var BootChainFwStatus "$NV" 07000000 0c000000
  refused_for I15 BootChainFwStatus "install $E_NEW $STAGE"
  e3_undo
  # I16: a switch pending. nvbootctrl writes BootChainFwNext only (D2); undone before any reboot.
  on_box 'nvbootctrl set-active-boot-slot 1' || lab_die "nvbootctrl set-active-boot-slot 1 failed"
  lab_box -t 30 "test -e /sys/firmware/efi/efivars/BootChainFwNext-$NV" || lab_die "I16: no BootChainFwNext to test with"
  lab_box -t 120 "$APPLIER install $E_NEW $STAGE" >"$OUT/I16.out" 2>&1 && lab_die "I16: install was not refused"
  grep -q BootChainFwNext "$OUT/I16.out" || lab_die "I16: refused, but not for BootChainFwNext: $(cat "$OUT/I16.out")"
  e3_undo
  st I16-undone
  untouched I16
  # I17: BootNext set, to the entry this boot came from, so even a reboot would change nothing.
  cur=$(lab_box -t 30 "efibootmgr | sed -n 's/^BootCurrent: //p'")
  [[ $cur =~ ^[0-9A-F]{4}$ ]] || lab_die "I17: no BootCurrent ('$cur')"
  on_box "efibootmgr -q -n $cur && efibootmgr | grep '^BootNext'" || lab_die "I17: could not set BootNext"
  lab_box -t 120 "$APPLIER install $E_NEW $STAGE" >"$OUT/I17.out" 2>&1 && lab_die "I17: install was not refused"
  grep -q BootNext "$OUT/I17.out" || lab_die "I17: refused, but not for BootNext: $(cat "$OUT/I17.out")"
  e3_undo
  st I17-undone
  untouched I17
  # I19: a capsule requested, OsIndications bit 2, with no capsule on the ESP.
  put_var OsIndications "$EFIG" 07000000 0400000000000000
  lab_box -t 120 "$APPLIER install $E_NEW $STAGE" >"$OUT/I19.out" 2>&1 && lab_die "I19: install was not refused"
  grep -qi capsule "$OUT/I19.out" || lab_die "I19: refused, but not for the pending capsule: $(cat "$OUT/I19.out")"
  e3_undo
  st I19-undone
  check "I19: OsIndications cleared" '.efivars.OsIndications == null'
  # N13: a release whose firmware is below the ESRT's lowest supported version. install writes
  # slot B (the NVMe); set-next must refuse before it stages the (non-)capsule.
  on_box "$APPLIER install $E_NEW $LOW" || lab_die "N13: install from $LOW failed"
  refused_for N13 'below the lowest supported' "set-next $E_NEW"
  # N12: the capsule changed in the cache after install. Chain B's marker set aside, so set-next
  # wants the capsule; put back after.
  on_box "$APPLIER install $E_NEW $CHANGED" || lab_die "N12: install from $CHANGED failed"
  on_box "printf 'changed\n' >> $CHANGED/fjarr_$E_NEW.cap && mv $M/chain-b $M/chain-b.e3 && sync -f $M"
  refused_for N12 changed "set-next $E_NEW"
  e3_undo
  on_box "printf 'not a capsule: e-capsule.sh e3\n' > $CHANGED/fjarr_$E_NEW.cap"
  LAB_DIE_HOOK=
  st end
  untouched "e3 end"
  check "e3 end: no status, no capsule request" '.efivars.BootChainFwStatus == null and .efivars.OsIndications == null'
  check "e3 end: chain B complete on 36.5.2" '.chains[] | select(.chain == "b") | .marker.state == "complete" and .marker.fw == "36.5.2"'
}

c_e1() {
  local t0
  start_on e1 "$E_BASE" a
  untouched "e1 start"
  lab_box -t 60 "sha256sum $STAGE/fjarr_$E_NEW.cap" | grep -q "^$cap_sha " ||
    lab_die "the capsule on the box is not the checked one (L4)"
  on_box "$APPLIER install $E_NEW $STAGE" || lab_die "install failed"
  st installed
  # The chain's marker set aside: chain B's firmware is unknown to the applier, so set-next
  # stages the capsule rather than switching (applier N3).
  on_box "mv $M/chain-b $M/chain-b.e1 && sync -f $M"
  on_box "$APPLIER set-next $E_NEW" || { on_box "mv $M/chain-b.e1 $M/chain-b"; lab_die "set-next failed"; }
  st set-next
  check "the capsule staged" '.capsule_staged == true and .capsule_pending == true and .trial.capsule == 1'
  t0=$SECONDS
  reboot_wait 'systemctl reboot' capsule-boot "$E_CAPSULE_TMO"
  lab_log "the capsule's boot: ssh after $((SECONDS - t0)) s (the apply time, the reboot included)"
  trial_end confirmed
  expect "the capsule trial" "$E_NEW" b
  check "the trial confirmed" '.last_trial.result == "good"'
  check "the capsule status 1" '.capsule_status == 1'
  check "chain B current, complete on 36.5.2" '.efivars.BootChainFwCurrent.value == 1 and (.chains[] | select(.chain == "b") | .marker.state == "complete" and .marker.fw == "36.5.2")'
  check "the firmware 36.5.2" '.bios_version == "v36.5.2" and .esrt.last_attempt_status == 0 and .esrt.fw == "36.5.2"'
  on_box "rm -f $M/chain-b.e1"
  reboot_wait 'systemctl reboot' committed-reboot
  expect "the committed release after a reboot" "$E_NEW" b
}

c_e2() {
  start_on e2 "$E_NEW" b
  check "e2 starts after a capsule trial" '.last_trial.result == "good" and .efivars.BootChainFwCurrent.value == 1'
  reboot_wait "$APPLIER rollback-now" rollback
  expect "rollback-now after the capsule" "$E_BASE" a
  check "chain A with slot A" '.efivars.BootChainFwCurrent.value == 0 and .bios_version == "v36.5.2"'
  check "both chains complete on 36.5.2" '[.chains[] | .marker.state == "complete" and .marker.fw == "36.5.2"] | all'
  on_box "$APPLIER install $E_NEW $STAGE" || lab_die "install failed"
  on_box "$APPLIER set-next $E_NEW" || lab_die "set-next failed"
  st set-next
  check "forward again without a capsule" '.capsule_staged == false and .trial.capsule == 0'
  reboot_wait 'systemctl reboot' trial
  trial_end confirmed
  expect "forward again" "$E_NEW" b
  check "the trial confirmed" '.last_trial.result == "good" and .efivars.BootChainFwCurrent.value == 1'
  reboot_wait "$APPLIER rollback-now" baseline
  expect "back to the baseline" "$E_BASE" a
}

"c_$case"
lab_log "e $case done: ${OUT#"$LAB_HOME"/}"
