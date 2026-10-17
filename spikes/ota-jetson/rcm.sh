#!/usr/bin/env bash
# rcm.sh: RCM round trips on the ZED Box Mini (slice 8.0, phase B4), written for the stock image.
# spec: docs/32-fleet-ota.md#jetson-test-image ("Flashing from the mini-PC", rules L1, L2, L5-L7)
#
#   rcm.sh [N]          N round trips, default 5
#   rcm.sh --check      the checks before the first trip, then stop: changes nothing
#
# One round trip: Linux on the box reboots into USB recovery mode by software (reboot --force
# forced-recovery); the mini-PC must see it in RCM (0955:7323) within 90 s; a power cycle through
# the plug brings it back, and RCM must stay away; the box must answer ssh on its MAC with a new
# boot id, finish starting in the state it started in before the first trip (systemd's state and
# its failed units), and show the same slot state. One CSV line per trip goes to
# results/<date>-rcm.csv.
#
# Before the first trip: the backup is verified (B3 before B4, L2; LAB_NO_BACKUP_OK=1 for a later
# image backed up elsewhere), the plug answers and is on, the box answers on its MAC, runs L4T
# (0955:7020 on the mini-PC's USB), has finished starting, and BootNext is not set (L5). Each trip
# starts only when its worst case ends before the hard stop (L1), and only on a box that has
# finished starting again.
#
# The first unexpected state stops the run (L7), with the state recorded (the mini-PC's USB, the
# plug, the box's slot state when it answers) and the trip's CSV line written:
#   - no RCM within 90 s, or RCM back after the power cycle: stop;
#   - after the power cycle the box back in RCM, another USB device, L4T on USB without ssh, ssh
#     to the box's MAC failing the pinned host key, or the old boot id: stop;
#   - nothing at all (no USB device, no answer) for 5 min after power on: one more power cycle and
#     5 min more, then stop whether it came back or not;
#   - not started as before, failed units changed, or another slot state: stop.
# After a stop, probe.sh records the box's full state if it answers.
#
# Each boot of the box shows its USB device to the mini-PC, next to a desktop session that is not
# the lab's. A person chooses before the first trip (lab_desktop_guard in lib/lab.sh):
# LAB_DESKTOP_ISOLATE=1 isolates the box's USB controller on the mini-PC for the run, LAB_DESKTOP_OK=1
# says the desktop may see the box come and go; with neither the run refuses.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=lib/lab.sh
. "$here/lib/lab.sh"

check=0
n=${1:-5}
if [[ $n == --check ]]; then
  check=1
  n=1
fi
[[ $n =~ ^[1-9][0-9]?$ ]] || {
  echo "usage: rcm.sh [N | --check]   (1-99 round trips, default 5)" >&2
  exit 2
}

# Limits, in seconds, and from them the worst case of one trip (L1).
READY_TMO=180 # systemd on the box done starting
SSH_TMO=20    # one short command on the box
SLOT_TMO=60   # the slot state
# Before: done starting, ssh and sudo, the reboot. Then RCM, the power cycle out of it, RCM
# staying away, the box back on ssh (with the one extra power cycle), done starting again, the
# reset reason, the failed units and the slot state; at the end the state recorded on a stop.
trip_worst=$((READY_TMO + 15 + 2 * SSH_TMO + LAB_RCM_TMO + LAB_POLL_MAX + LAB_CYCLE_WORST + LAB_RCM_GONE_S +
  LAB_POLL_MAX + 2 * (LAB_BACK_TMO + LAB_POLL_MAX) + LAB_CYCLE_WORST + READY_TMO + 15 + 2 * SSH_TMO +
  2 * SLOT_TMO + 60))
lab_init rcm $((n * trip_worst + 600))

slot_state() { # what must not change across a round trip
  # shellcheck disable=SC2016 # runs on the box and expands there
  lab_box_root -t "$SLOT_TMO" '
nvbootctrl dump-slots-info 2>&1
nvbootctrl -t rootfs dump-slots-info 2>&1
nv=781e084c-a330-417c-b678-38e696380cb9
tok=ed3374ef-767b-42fa-af70-db520a392822
for v in L4TDefaultBootMode RootfsRedundancyLevel RootfsRetryCountMax RootfsStatusSlotA RootfsStatusSlotB \
  BootChainFwCurrent BootChainFwNext BootChainFwStatus BootChainFwResetCount; do
  g=$nv; [ "$v" = BootChainFwResetCount ] && g=$tok # boot-services-only by the source: absent expected
  f=/sys/firmware/efi/efivars/$v-$g
  if [ -e "$f" ]; then echo "$v $(od -An -tx1 -v "$f" | tr -s " \n" "  ")"; else echo "$v absent"; fi
done
[ -e /sys/firmware/efi/efivars/BootNext-8be4df61-93ca-11d2-aa0d-00e098032b8c ] && echo "BootNext set"
p=/sys/devices/platform/bus@0/c360000.pmc
for a in boot_chain_current rootfs_current rootfs_retry_count_a rootfs_retry_count_b; do echo "$a $(cat $p/$a 2>&1)"; done'
}

failed_units() { # systemd's failed units on the box, one line
  # shellcheck disable=SC2016 # runs on the box and expands there
  lab_box -t "$SSH_TMO" 'systemctl --failed --plain --no-legend --no-pager 2>/dev/null | awk "{ print \$1 }" | sort | paste -sd, -'
}

record_state() { # <why>: what the box, its plug and the mini-PC show, into the log and .lab/state (L7)
  local f
  f=$LAB_DIR/state/rcm-stop-$(TZ=$LAB_TZ date +%Y%m%dT%H%M%S).txt
  lab_usb_poll
  lab_log "STATE ($1): the mini-PC's USB sees '$LAB_USB_STATE'"
  if lab_plug_status; then lab_log "STATE: plug output=$LAB_PLUG_OUTPUT, ${LAB_PLUG_APOWER} W"; else lab_log "STATE: the plug does not answer"; fi
  LAB_BOX_IP=''
  if lab_find_box && lab_box_bootid; then
    lab_log "STATE: the box answers ssh at $LAB_BOX_IP, boot id $LAB_BOOTID, slot state in ${f#"$LAB_HOME"/}; run probe.sh next"
    {
      echo "== $1"
      slot_state
      echo "== systemd: $(lab_wait_ready 10), failed units: $(failed_units)"
    } >"$f" 2>&1 || true
  elif lab_host_key_failed; then
    lab_log "STATE: something answers on the box's MAC at $LAB_BOX_IP, but not with the box's host key"
  else
    lab_log "STATE: the box does not answer ssh on its MAC${LAB_BOX_IP:+ (it answers ping at $LAB_BOX_IP)}"
  fi
}

csv=$LAB_HOME/results/$(lab_today)-rcm.csv
((check)) && csv=/dev/null
[[ -s $csv || $csv == /dev/null ]] || echo "run,trip,start_utc,to_rcm_s,rcm_w,off_s,to_l4t_usb_s,to_ssh_s,to_ready_s,state,reset_reason,slots,result" >"$csv"
run=$(date -u +%Y%m%dT%H%M%SZ)
trip=0 start='' in_trip=0 stop_code=''
row() { # the trip's line, the fields set so far
  printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' "$run" "$trip" "$start" "${to_rcm:-}" "${rcm_w:-}" \
    "${off_s:-}" "${to_usb:-}" "${to_ssh:-}" "${to_ready:-}" "${state:-}" "${reset:-}" "${slots:-}" "$1" >>"$csv"
}
on_die() { # lab_die's hook: inside a trip, the trip's line and the state (L7)
  ((in_trip)) || return 0
  in_trip=0
  row "${stop_code:-stopped}"
  record_state "$1"
}
LAB_DIE_HOOK=on_die
on_signal() { # <exit code>: Ctrl-C or TERM; inside a trip, the trip's line and what to do next (L7)
  if ((in_trip)); then
    in_trip=0
    row interrupted
    lab_log "interrupted in trip $trip: if the mini-PC's USB shows $LAB_RCM_USB (RCM), run shelly.sh cycle"
  fi
  exit "$1"
}
trap 'on_signal 130' INT
trap 'on_signal 143' TERM
stop() { # <result> <why>: stop the run (L7); on_die records it
  stop_code=$1
  lab_die "$2 (L7)"
}

# --- before the first trip (L2, L5) -----------------------------------------------------------------
lab_plug_check || lab_die "the plug does not answer (L2)"
[[ $LAB_PLUG_OUTPUT == true ]] || lab_die "the plug is off"
lab_find_box || lab_die "the box does not answer on its MAC (L2)"
lab_usb_poll
[[ $LAB_USB_STATE == l4t ]] || lab_die "the mini-PC sees '$LAB_USB_STATE' on the box's USB, not L4T ($LAB_L4T_USB) (L7)"
base_ready=$(lab_wait_ready "$READY_TMO")
[[ $base_ready == running || $base_ready == degraded ]] || lab_die "the box has not finished starting: '$base_ready'"
base_failed=$(failed_units) || lab_die "could not read the box's failed units"
lab_box_bootid || lab_die "the box does not answer ssh (${LAB_SSH_ERR:-no error})"
boot=$LAB_BOOTID
base=$(slot_state) || lab_die "could not read the slot state"
grep -q '^BootNext set' <<<"$base" && lab_die "BootNext is set on the box: not rebooting it (L5)"
lab_log "box at $LAB_BOX_IP, boot $boot, systemd '$base_ready' (failed units: ${base_failed:-none}); slot state recorded"
if ((check)); then
  printf '%s\n' "$base" | sed 's/^/  slot state: /' >&2
  [[ ${LAB_DESKTOP_OK:-} == 1 ]] || lab_usb_isolate check || true
  lab_log "GNOME automount for the mini-PC's desktop user: $(lab_desktop_automount)"
  lab_backup_check || lab_log "a run would refuse: no verified backup (B3 before B4)"
  lab_log "--check: stopping before the first reboot; a run of $n trip(s) needs up to $((n * trip_worst / 60)) min"
  exit 0
fi
if [[ ${LAB_NO_BACKUP_OK:-} != 1 ]]; then
  lab_backup_check || lab_die "no verified backup: run backup.sh first (B3 before B4, L2), or LAB_NO_BACKUP_OK=1 for an image backed up elsewhere"
fi
lab_desktop_guard
lab_log "$n round trip(s), each up to $((trip_worst / 60)) min at worst; results in ${csv#"$LAB_HOME"/}"

wait_back() { # <old boot id> <seconds>: for a new boot on ssh. Sets back, and to_usb, to_ssh, boot
  # back: ssh (a new boot answers), rcm, other, hostkey, same-boot, l4t-no-ssh or nothing
  local old=$1 limit=$2 t0=$SECONDS seen_l4t=0
  back=nothing
  while ((SECONDS - t0 < limit)); do
    lab_usb_poll
    case $LAB_USB_STATE in
      rcm) back=rcm && return 1 ;;
      other:*) back=other && return 1 ;;
      l4t)
        seen_l4t=1
        [[ -n $to_usb ]] || to_usb=$((SECONDS - t_on))
        ;;
    esac
    LAB_BOX_IP=''
    if lab_find_box; then
      if lab_box_bootid; then
        [[ $LAB_BOOTID == "$old" ]] && back=same-boot && return 1
        boot=$LAB_BOOTID
        to_ssh=$((SECONDS - t_on))
        back=ssh
        return 0
      fi
      lab_host_key_failed && back=hostkey && return 1
    fi
    sleep 3
  done
  ((seen_l4t)) && back=l4t-no-ssh
  return 1
}

stop_back() { # after wait_back failed: stop with what it saw
  case $back in
    rcm) stop rcm-again "the box is in RCM again after the power cycle" ;;
    other) stop usb-other "the mini-PC sees '$LAB_USB_STATE' on the box's USB" ;;
    hostkey) stop host-key "ssh on the box's MAC fails the pinned host key: something else booted" ;;
    same-boot) stop not-rebooted "the box answers with the boot id it had before the trip: no reboot happened" ;;
    l4t-no-ssh) stop no-ssh "L4T runs (on the mini-PC's USB) but ssh on the box's MAC does not answer within $LAB_BACK_TMO s" ;;
    *) stop not-back "${1:-the box did not come back}" ;;
  esac
}

for ((trip = 1; trip <= n; trip++)); do
  to_rcm='' rcm_w='' off_s='' to_usb='' to_ssh='' to_ready='' state='' reset='' slots='' stop_code=''
  start=$(date -u +%FT%TZ)
  lab_need "$trip_worst" "trip $trip of $n"
  in_trip=1
  ready=$(lab_wait_ready "$READY_TMO")
  [[ $ready == "$base_ready" ]] || stop not-ready "before trip $trip systemd on the box is '$ready', not '$base_ready'"
  lab_log "-- trip $trip of $n"

  t0=$SECONDS
  stop_code=rcm-enter # a stop inside the helpers below names its step
  lab_rcm_enter
  stop_code=''
  lab_rcm_wait "$LAB_RCM_TMO" || stop no-rcm "no RCM ($LAB_RCM_USB) within $LAB_RCM_TMO s of reboot forced-recovery; the mini-PC sees '$LAB_USB_STATE'"
  to_rcm=$((SECONDS - t0))
  lab_plug_status || true
  rcm_w=$LAB_PLUG_APOWER
  lab_log "RCM after $to_rcm s, ${rcm_w} W"

  stop_code=rcm-leave
  lab_rcm_leave
  stop_code=''
  off_s=$LAB_CYCLE_SECONDS
  t_on=$LAB_CYCLE_ON_AT
  [[ -n $LAB_USB_L4T_AT ]] && to_usb=$((LAB_USB_L4T_AT - t_on))
  if ! wait_back "$boot" "$LAB_BACK_TMO"; then
    [[ $back == nothing ]] || stop_back
    lab_log "nothing on the mini-PC's USB or the network within $LAB_BACK_TMO s of power on: one more power cycle (L7)"
    stop_code=power-cycle
    lab_power_cycle
    stop_code=''
    t_on=$LAB_CYCLE_ON_AT
    to_usb=''
    if wait_back "$boot" "$LAB_BACK_TMO"; then
      stop back-after-second-cycle "the box came back only after a second power cycle"
    fi
    stop_back "the box did not come back after two power cycles"
  fi
  state=$(lab_wait_ready "$READY_TMO")
  to_ready=$((SECONDS - t_on))
  reset=$(lab_box -t "$SSH_TMO" 'cat /sys/devices/platform/bus@0/c360000.pmc/reset_reason 2>/dev/null' || true)
  [[ $state == "$base_ready" ]] || stop not-started "after trip $trip systemd on the box is '$state', not '$base_ready' as before"
  failed=$(failed_units) || stop no-units "could not read the failed units after trip $trip"
  [[ $failed == "$base_failed" ]] || stop failed-units "after trip $trip the failed units are '${failed:-none}', not '${base_failed:-none}'"
  now=$(slot_state) || stop no-slot-state "could not read the slot state after trip $trip"
  if [[ $now != "$base" ]]; then
    slots=changed
    diff <(printf '%s\n' "$base") <(printf '%s\n' "$now") >&2 || true
    stop slots-changed "the slot state changed in trip $trip"
  fi
  slots=same
  row ok
  in_trip=0
  lab_log "trip $trip: RCM $to_rcm s; after power on L4T on USB ${to_usb:-?} s, ssh $to_ssh s, started ($state) $to_ready s"
done
lab_log "$n of $n round trips, results in ${csv#"$LAB_HOME"/}"
