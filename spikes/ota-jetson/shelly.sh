#!/usr/bin/env bash
# shelly.sh: the box's Shelly plug, rule L8 (slice 8.0, phase B5).
# spec: docs/32-fleet-ota.md#jetson-test-image (L8: the plug powers on by default after its own
# outage, and has no timers)
#
#   shelly.sh [show]            the plug's configuration and state (read only)
#   shelly.sh apply             initial_state "on", auto_on and auto_off off; then read back
#   shelly.sh cycle [--running] power cycle the box (off 10 s, on by the plug's own timer), e.g. out
#                               of RCM after a script stopped there (L7)
#
# apply changes the plug's configuration only, never its output. Schedules, webhooks and scripts
# on the plug are listed, never changed: any of them, or a list the plug did not answer, makes
# show and apply exit 3 for a person to look at.
#
# cycle only cuts a box that is in RCM or shows nothing on the mini-PC's USB: a box running L4T
# is rebooted instead, unless --running says the cut of a running system is meant. Like every
# power cycle it needs a person's choice about the mini-PC's desktop (lab_desktop_guard in
# lib/lab.sh): LAB_DESKTOP_ISOLATE=1 (isolate the box's USB controller, also when the box shows
# nothing: LAB_USB_CTL) or LAB_DESKTOP_OK=1. The isolation outlives the run until the box is back
# on USB and deauthorized; the next run that takes the lock lifts it then.
#
# The plug's device name doubles as its password: it is never printed or written, and neither is
# its MAC; only the fields below are shown. The digest password reaches curl through a pipe, never
# a file or a command line. results/<date>-shelly.txt gets what show printed.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=lib/lab.sh
. "$here/lib/lab.sh"

mode=${1:-show}
running=0
case $mode in
  show | apply) (($# <= 1)) || mode=usage ;;
  cycle)
    case ${2:-} in
      '') ;;
      --running) running=1 ;;
      *) mode=usage ;;
    esac
    ;;
  *) mode=usage ;;
esac
if [[ $mode == usage ]]; then
  echo "usage: shelly.sh [show | apply | cycle [--running]]" >&2
  exit 2
fi
lab_init "shelly-$mode" $((LAB_CYCLE_WORST + 120))
if [[ $mode == cycle ]]; then
  lab_usb_poll
  lab_log "the mini-PC's USB sees the box as: $LAB_USB_STATE"
  case $LAB_USB_STATE in
    rcm | none) ;;
    l4t)
      ((running)) || lab_die "the box runs L4T: reboot it instead, or shelly.sh cycle --running to cut the power of a running system"
      ;;
    *) lab_die "the mini-PC sees '$LAB_USB_STATE' on the box's USB: not cycling (L7)" ;;
  esac
  lab_power_cycle
  sleep 5
  lab_usb_poll
  lab_log "the mini-PC's USB sees the box as: $LAB_USB_STATE (L4T shows as $LAB_L4T_USB once it runs)"
  exit 0
fi

# The switch's own settings, without "name" (the device name lives elsewhere, but stay safe).
switch_config() { lab_plug 'Switch.GetConfig?id=0' | jq -c 'del(.name)'; }

SHOW_TIMERS=0 SHOW_UNKNOWN=''
show() { # sets SHOW_TIMERS (schedules + webhooks + scripts) and SHOW_UNKNOWN (lists that failed)
  local info cfg st jobs hooks scripts
  SHOW_UNKNOWN=''
  info=$(lab_plug 'Shelly.GetDeviceInfo') || lab_die "the plug does not answer"
  cfg=$(switch_config) || lab_die "Switch.GetConfig failed"
  st=$(lab_plug 'Switch.GetStatus?id=0') || lab_die "Switch.GetStatus failed"
  jobs=$(lab_plug 'Schedule.List') || { jobs='{"jobs":[]}' && SHOW_UNKNOWN+=' schedules'; }
  hooks=$(lab_plug 'Webhook.List') || { hooks='{"hooks":[]}' && SHOW_UNKNOWN+=' webhooks'; }
  scripts=$(lab_plug 'Script.List') || { scripts='{"scripts":[]}' && SHOW_UNKNOWN+=' scripts'; }
  jq -r '"device: \(.model) gen \(.gen), firmware \(.ver), app \(.app), auth \(.auth_en)"' <<<"$info"
  jq -r '"switch: initial_state \(.initial_state), auto_on \(.auto_on) (\(.auto_on_delay) s), auto_off \(.auto_off) (\(.auto_off_delay) s), power_limit \(.power_limit) W, current_limit \(.current_limit) A"' <<<"$cfg"
  jq -r '"state: output \(.output) (last switched by \(.source)), \(.apower) W, \(.voltage) V, \(.temperature.tC) C"' <<<"$st"
  if [[ $SHOW_UNKNOWN == *schedules* ]]; then echo "schedules: unknown (Schedule.List failed)"; else
    jq -r '"schedules: \(.jobs | length)" + (if (.jobs | length) > 0 then ": " + (.jobs | map("\(.timespec) enabled=\(.enable) " + (.calls | map(.method) | join("+"))) | join("; ")) else "" end)' <<<"$jobs"
  fi
  if [[ $SHOW_UNKNOWN == *webhooks* ]]; then echo "webhooks: unknown (Webhook.List failed)"; else jq -r '"webhooks: \(.hooks | length)"' <<<"$hooks"; fi
  if [[ $SHOW_UNKNOWN == *scripts* ]]; then echo "scripts: unknown (Script.List failed)"; else jq -r '"scripts: \(.scripts | length)"' <<<"$scripts"; fi
  SHOW_TIMERS=$(jq -n --argjson j "$jobs" --argjson h "$hooks" --argjson s "$scripts" \
    '($j.jobs // [] | length) + ($h.hooks // [] | length) + ($s.scripts // [] | length)')
}

out=$LAB_HOME/results/$(lab_today)-shelly.txt
[[ -e $out ]] && out=${out%.txt}-$(TZ=$LAB_TZ date +%H%M).txt
{
  printf '# The ZED Box Mini'"'"'s Shelly plug (L8), %s: %s %s\n' "$mode" "$(TZ=$LAB_TZ date '+%Y-%m-%d %H:%M')" "$LAB_TZ"
  printf '# spec: docs/32-fleet-ota.md#jetson-test-image. Written by shelly.sh.\n\n'
} >"$LAB_RUN/result.txt"

shown() { # show, in this shell (it sets SHOW_*), onto stdout and the result
  show >"$LAB_RUN/show.txt"
  cat "$LAB_RUN/show.txt"
  cat "$LAB_RUN/show.txt" >>"$LAB_RUN/result.txt"
}

verdict() { # L8 on what show saw last: 0 when it holds, 3 for a person to look at
  if [[ -n $SHOW_UNKNOWN ]]; then
    lab_log "L8: unknown: the plug did not list its${SHOW_UNKNOWN}; look at them by hand"
    return 3
  fi
  if ((SHOW_TIMERS > 0)); then
    lab_log "L8: the plug has $SHOW_TIMERS schedule(s), webhook(s) or script(s): left as they are, for a person to look at"
    return 3
  fi
  return 0
}

if [[ $mode == show ]]; then
  shown
  cp "$LAB_RUN/result.txt" "$out"
  lab_log "written: ${out#"$LAB_HOME"/}"
  rc=0
  verdict || rc=$?
  exit "$rc"
fi

printf 'before:\n' >>"$LAB_RUN/result.txt"
shown
want='{"initial_state":"on","auto_on":false,"auto_off":false}'
lab_lock_check
lab_log "Switch.SetConfig: $want"
answer=$(lab_plug "Switch.SetConfig?id=0&config=$(jq -rn --arg c "$want" '$c | @uri')") ||
  lab_die "the plug refused Switch.SetConfig (L7)"
lab_log "the plug answered: $answer"
cfg=$(switch_config) || lab_die "reading the configuration back failed (L7)"
jq -e '.initial_state == "on" and .auto_on == false and .auto_off == false' <<<"$cfg" >/dev/null ||
  lab_die "read back, the plug's configuration is not what was set: $cfg (L7)"
printf '\nafter Switch.SetConfig %s (answer %s):\n' "$want" "$answer" >>"$LAB_RUN/result.txt"
shown
cp "$LAB_RUN/result.txt" "$out"
lab_log "written: ${out#"$LAB_HOME"/}"
rc=0
verdict || rc=$?
((rc == 0)) && lab_log "L8: initial_state on, no auto-on or auto-off timer, no schedules, webhooks or scripts"
exit "$rc"
