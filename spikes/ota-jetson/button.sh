#!/usr/bin/env bash
# button.sh: RCM by the front panel's buttons, with a person at the ZED Box Mini (slice 8.0, Q4).
# spec: docs/32-fleet-ota.md#jetson-test-image ("Three rescue layers", item 3; Q4; rules L1, L7)
#
#   LAB_DESKTOP_OK=1 button.sh
#
# The fallback when nothing on the box boots far enough to enter RCM by software: a person holds
# RCV (a paper clip in the front panel's pinhole), presses and releases RST, then releases RCV.
# This proves it once, on the running stock box, before anything relies on it (C2b's QSPI
# restore). Under the lab lock: the plug answers, the box runs L4T on the mini-PC's USB; the
# script then waits up to BUTTON_WAIT_S (600 s) for RCM (0955:7323), leaves it by one power cycle
# through the plug (RCM must stay away, L7), and waits for the stock system on ssh with a new boot
# id, started. One CSV line goes to results/<date>-button.csv.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=lib/lab.sh
. "$here/lib/lab.sh"

: "${BUTTON_WAIT_S:=600}"
lab_init button $((BUTTON_WAIT_S + LAB_CYCLE_WORST + LAB_RCM_GONE_S + 2 * LAB_BACK_TMO + 600))
csv=$LAB_HOME/results/$(TZ=$LAB_TZ date +%F)-button.csv
row() { # <result> <note>
  [[ -s $csv ]] || echo "start,to_rcm_s,rcm_to_l4t_s,to_ssh_s,result,note" >"$csv"
  echo "$START,${TO_RCM:-},${TO_L4T:-},${TO_SSH:-},$1,$2" >>"$csv"
}
START=$(date -u +%FT%TZ)

lab_plug_check
lab_desktop_guard
lab_usb_poll
[[ $LAB_USB_STATE == l4t ]] || lab_die "the mini-PC sees the box as '$LAB_USB_STATE', not L4T: start from the running stock system"
lab_find_box || lab_die "the box does not answer on its MAC"
lab_box_bootid || lab_die "no boot id from the box: $LAB_SSH_ERR"
before=$LAB_BOOTID
lab_log "box at $LAB_BOX_IP, boot $before"

lab_lock_check
lab_log "NOW: hold RCV with the paper clip, press and release RST, then release RCV after 2 s"
lab_log "waiting up to $BUTTON_WAIT_S s for RCM (0955:7323) on the mini-PC's USB"
t0=$SECONDS
if ! lab_rcm_wait "$BUTTON_WAIT_S"; then
  row no-rcm "no RCM within $BUTTON_WAIT_S s; the mini-PC saw $LAB_USB_STATE"
  lab_die "no RCM within $BUTTON_WAIT_S s (the mini-PC sees '$LAB_USB_STATE'); nothing changed by this script"
fi
TO_RCM=$((SECONDS - t0))
lab_log "RCM seen $TO_RCM s after the prompt: the button way in works"

lab_lock_check
t1=$SECONDS
lab_rcm_leave
[[ -n ${LAB_USB_L4T_AT:-} ]] && TO_L4T=$((LAB_USB_L4T_AT - t1))
t2=$SECONDS
while ((SECONDS - t2 < 2 * LAB_BACK_TMO)); do
  if lab_find_box && lab_box_bootid && [[ $LAB_BOOTID != "$before" ]]; then break; fi
  sleep 5
done
[[ -n ${LAB_BOOTID:-} && $LAB_BOOTID != "$before" ]] || {
  row not-back "RCM ok; the stock system did not answer with a new boot id"
  lab_die "the box did not come back on ssh after the power cycle out of RCM"
}
TO_SSH=$((SECONDS - t1))
state=$(lab_wait_ready 300) || true
lab_log "stock system back: boot $LAB_BOOTID, $TO_SSH s after the power cycle, systemd $state"
row ok "systemd $state"
