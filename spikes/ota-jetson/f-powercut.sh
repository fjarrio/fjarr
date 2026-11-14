#!/usr/bin/env bash
# shellcheck disable=SC2016 # the remote scripts take their arguments as $1, $2 there
# f-powercut.sh: phase F, power cuts through the plug during an update (slice 8.0).
# spec: docs/32-fleet-ota.md#jetson-test-image (phase F; rules L1, L2, L7, L8); README.md, "F"
#
#   LAB_DESKTOP_OK=1 f-powercut.sh install|set-next|trial|mark-good [count]
#
# From 1.0 in slot A, committed, with release 1.2 staged (d3.sh setup): an update step, the power
# cut at a random moment of it, the box back on its MAC, its state recorded, any trial followed to
# its end, the outcome checked, and the baseline again before the next cut. No capsule: the cuts
# land on the NVMe, the ESP and the UEFI variables, never the QSPI. One CSV line per cut.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=lib/lab.sh
. "$here/lib/lab.sh"

point=${1:?usage: f-powercut.sh install|set-next|trial|mark-good [count]}
count=${2:-20}
[[ $point =~ ^(install|set-next|trial|mark-good)$ ]] || { echo "f-powercut.sh: no point '$point'" >&2; exit 2; }
[[ $count =~ ^[0-9]+$ ]] || { echo "f-powercut.sh: count is a number" >&2; exit 2; }
: "${F_BASE:=1.0}"
: "${F_NEW:=1.2}"
: "${F_UP_TMO:=600}"  # a cut to the box on ssh
STAGE=/data/fjarr-update/stage/$F_NEW
APPLIER=/usr/lib/fjarr/applier
OUT=$LAB_DIR/f/$(date -u +%Y%m%dT%H%M%SZ)-$point
mkdir -p "$OUT"

lab_init "f-$point" $((900 + count * (2 * F_UP_TMO + 900)))
csv=$LAB_HOME/results/$(TZ=$LAB_TZ date +%F)-f-$point.csv
[[ -s $csv ]] || echo "start,point,cut,delay_s,booted,slot,last_trial,slot_b_status,to_ssh_s,result,note" >"$csv"
lab_plug_check
lab_desktop_guard
lab_backup_check || lab_die "no verified B3 backup (L2)"
lab_mpc -t 30 'systemctl is-active -q fjarr-net-rescue-dhcp.service' ||
  lab_die "the network rescue is not serving on the mini-PC (b6.sh net-up; L2)"
lab_uart_start "f-$point"
lab_use_fjarr_image

LAST='' N=0
st() { # <step>: the applier's state into OUT/<n>-<step>.json (LAST); 1 when it cannot be read
  N=$((N + 1))
  LAST=$OUT/$(printf %03d $N)-$1.json
  lab_box -t 60 "$APPLIER state" >"$LAST" 2>/dev/null && jq -e . "$LAST" >/dev/null 2>&1
}
j() { jq -r "$1" "$LAST"; }
on_box() { lab_lock_check && lab_box -t 600 "$1" >>"$OUT/box.log" 2>&1; } # <command>
up() { # <old boot id>: until the image answers with another boot id; the dead-man's RCM is left
  local t0=$SECONDS
  while ((SECONDS - t0 < F_UP_TMO)); do
    if lab_find_box && lab_box_bootid && [[ $LAB_BOOTID != "$1" ]]; then
      lab_deadman_touch || true
      UP_S=$((SECONDS - t0))
      return 0
    fi
    lab_host_key_failed && lab_die "something answered on the box's MAC with another host key (L7)"
    lab_usb_poll
    if [[ $LAB_USB_STATE == rcm ]]; then
      lab_log "the box is in RCM (its dead-man): one power cycle"
      lab_rcm_leave
    fi
    sleep 5
  done
  return 1
}
reboot_box() { # <command>: one that ends the session; the next boot
  local before=$LAB_BOOTID
  lab_lock_check
  _lab_tmo 20 ssh -n "${LAB_SSH_BOX[@]}" "$LAB_BOX_USER@$LAB_BOX_IP" "$1" >/dev/null 2>&1 || true
  up "$before" || lab_die "nothing came back within $F_UP_TMO s of '$1'"
}
trial_over() { # until no trial is in progress, through its reboots; then the state
  local t0=$SECONDS
  while ((SECONDS - t0 < 2 * F_UP_TMO)); do
    if lab_find_box && lab_box_bootid; then
      lab_deadman_touch || true
      if lab_box -t 30 'test ! -e /data/fjarr-update/applier/trial' 2>/dev/null &&
        lab_box -t 30 'test -e /run/fjarr-applier/verified' 2>/dev/null; then
        st settled && return 0
      fi
    fi
    lab_usb_poll
    [[ $LAB_USB_STATE == rcm ]] && { lab_log "the box is in RCM (its dead-man): one power cycle"; lab_rcm_leave; }
    sleep 10
  done
  lab_die "the trial did not end within $((2 * F_UP_TMO)) s"
}
baseline() { # 1.0 from slot A, committed, no trial, no test flags
  trial_over
  on_box 'rm -f /data/fjarr-test/fail-health /data/fjarr-test/panic /data/fjarr-test/hold-health'
  if [[ $(j .booted_slot) == b ]]; then
    reboot_box "$APPLIER rollback-now"
    trial_over
  fi
  [[ $(j .booted) == "$F_BASE" && $(j .booted_slot) == a && $(j .is_trial) == false ]] ||
    lab_die "no baseline: $(j .booted) from slot $(j .booted_slot), trial $(j .is_trial)"
}
cut_after() { # <seconds, may be fractional>: wait, then the cut
  sleep "$1"
  lab_lock_check
  lab_power_cycle >/dev/null
}
rnd() { # <min> <max>: a random delay in tenths of a second, printed as seconds
  local tenths=$((($1 * 10) + RANDOM % (($2 - $1) * 10 + 1)))
  printf "%d.%d" $((tenths / 10)) $((tenths % 10))
}

# The start may find the box in RCM, sent there by its dead-man since the last run: up leaves it.
up "" || lab_die "the Fjarr image does not answer on the box's MAC"
lab_box -t 30 "test -s $STAGE/fjarr_$F_NEW.swu" || lab_die "release $F_NEW is not staged ($STAGE): d3.sh setup"
fails=0
for ((i = 1; i <= count; i++)); do
  baseline
  start=$(date -u +%FT%TZ) note='' result=ok
  case $point in
    install)
      d=$(rnd 2 30)
      on_box "systemd-run -q --unit=f-install-$i --collect $APPLIER install $F_NEW $STAGE" || lab_die "could not start install"
      before=$LAB_BOOTID
      cut_after "$d"
      ;;
    set-next)
      on_box "$APPLIER install $F_NEW $STAGE" || lab_die "install failed"
      d=$(rnd 0 3)
      on_box "systemd-run -q --unit=f-set-next-$i --collect $APPLIER set-next $F_NEW" || lab_die "could not start set-next"
      before=$LAB_BOOTID
      cut_after "$d"
      ;;
    trial)
      on_box "$APPLIER install $F_NEW $STAGE && $APPLIER set-next $F_NEW" || lab_die "install or set-next failed"
      d=$(rnd 5 45)
      before=$LAB_BOOTID
      lab_lock_check
      _lab_tmo 20 ssh -n "${LAB_SSH_BOX[@]}" "$LAB_BOX_USER@$LAB_BOX_IP" 'systemctl reboot' >/dev/null 2>&1 || true
      cut_after "$d"
      ;;
    mark-good)
      on_box "$APPLIER install $F_NEW $STAGE && touch /data/fjarr-test/hold-health && $APPLIER set-next $F_NEW" ||
        lab_die "install or set-next failed"
      reboot_box 'systemctl reboot'
      st held || true
      [[ $(j .booted_slot) == b && $(j .is_trial) == true ]] || lab_die "cut $i: the held boot is not the trial on slot B"
      d=$(rnd 0 3)
      on_box 'rm -f /data/fjarr-test/hold-health'
      before=$LAB_BOOTID
      cut_after "$d"
      ;;
  esac
  up "$before" || lab_die "cut $i ($point, ${d} s): nothing came back within $F_UP_TMO s"
  to_ssh=$UP_S
  st after-cut || note="state unreadable right after the cut; "
  trial_over
  on_box 'rm -f /data/fjarr-test/hold-health'
  booted=$(j .booted) slot=$(j .booted_slot) last=$(j '.last_trial.result // "none"')
  b_status=$(j '.slots[] | select(.slot == "b") | .RootfsStatusSlotB.value')
  case $point in
    install)
      [[ $booted == "$F_BASE" && $slot == a ]] || { result=FAIL note+="not back on $F_BASE in slot A"; }
      ;;
    *)
      if [[ $slot == b ]]; then
        [[ $booted == "$F_NEW" && $last == good && $b_status == 0 ]] ||
          { result=FAIL note+="on slot B, but not a confirmed $F_NEW (last $last, B status $b_status)"; }
      else
        [[ $booted == "$F_BASE" ]] || { result=FAIL note+="slot A runs $booted"; }
      fi
      ;;
  esac
  echo "$start,$point,$i,$d,$booted,$slot,$last,$b_status,$to_ssh,$result,${note%; }" >>"$csv"
  lab_log "cut $i/$count ($point at ${d} s): $booted from slot $slot, last trial $last, slot B $b_status, ssh after ${to_ssh} s: $result${note:+ ($note)}"
  [[ $result == ok ]] || fails=$((fails + 1))
  ((fails == 0)) || lab_die "cut $i failed its check: stopping for a person (L7); the state is in ${OUT#"$LAB_HOME"/}"
done
baseline
lab_log "F $point: $count cuts, all ok; ${csv#"$LAB_HOME"/}"
