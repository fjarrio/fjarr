# shellcheck shell=bash
# lib/lab.sh: the shared helpers of the ZED Box Mini lab, slice 8.0's Jetson half.
# spec: docs/32-fleet-ota.md#jetson-test-image (lab safety rules L1, L2, L7 and L8)
#
# Sourced by every script that touches the box, its plug or RCM, never run on its own:
#
#     here=$(cd "$(dirname "$0")" && pwd)
#     . "$here/lib/lab.sh"
#     lab_init <name> <seconds the run needs> [nolock]
#
# lab_init refuses inside the nightly window (00:00-06:00 Europe/Stockholm) and when the run could
# not end before the day's hard stop (LAB_DEADLINE_MARGIN before 00:00: 23:00), then takes the lab
# lock on the mini-PC and holds it until the script exits (L1). The lock is the flock on
# /run/lock/fjarr-zedbox.lock on the mini-PC. Run on the mini-PC itself, the script holds it on an
# open file descriptor. Run anywhere else (the workstation, the usual case), one ssh session to the
# mini-PC holds it for the whole run, answers the script's questions about the mini-PC's USB, and
# lab_lock_check proves it is still held before every action that changes the box, its plug or
# RCM: a lost session stops the script before its next action. lab_need refuses any action whose
# worst case would run past the hard stop. See lib/README.md for every function.
#
# Functions that set LAB_* variables must run in the calling shell, not in $(...); so must
# everything that can call lab_die, which only ends the shell it runs in.

# --- configuration: environment, then .lab/lab.env (gitignored), then the lab's defaults ----------

LAB_HOME=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd) # spikes/ota-jetson
: "${LAB_DIR:=$LAB_HOME/.lab}"                            # gitignored: logs, raw dumps, backups
if [[ -r $LAB_DIR/lab.env ]]; then
  # shellcheck source=/dev/null
  . "$LAB_DIR/lab.env"
fi
: "${LAB_BOX_MAC:=}"                     # the box's Ethernet MAC: a unique ID, so only in lab.env
: "${LAB_BOX_USER:=user}"                # the stock image's user (key login, passwordless sudo)
: "${LAB_SUBNET:=192.168.10}"            # the /24 swept when the box's address is unknown
: "${LAB_MINIPC:=robot@192.168.10.108}"  # the mini-PC: the lock, the USB cable to the box
: "${LAB_PLUG:=192.168.10.202}"          # the box's Shelly plug (RPC over HTTP, digest auth)
: "${LAB_PLUG_USER:=admin}"
: "${LAB_PLUG_PASS_FILE:=$HOME/.config/fjarr/shelly-zedbox.pass}"
: "${LAB_LOCK_FILE:=/run/lock/fjarr-zedbox.lock}"
: "${LAB_TZ:=Europe/Stockholm}"
: "${LAB_WINDOW_END_M:=390}"             # refused from 00:00 to this minute of the day: 06:00, and
                                         # fjarr-lab's GRACE_MIN (30) for a nightly job still running
: "${LAB_DEADLINE_MARGIN:=3600}"         # every run ends this long before 00:00: the hard stop, 23:00
: "${LAB_RCM_USB:=0955:7323}"            # an Orin NX 16 GB in USB recovery mode
: "${LAB_L4T_USB:=0955:7020}"            # L4T's USB device mode, while Linux runs
: "${LAB_OFF_SECONDS:=10}"               # how long a power cycle keeps the box off
: "${LAB_RCM_TMO:=90}"                   # from reboot forced-recovery to RCM on the mini-PC (B4)
: "${LAB_BACK_TMO:=300}"                 # from power on to the box on ssh with a new boot id
: "${LAB_RCM_GONE_S:=30}"                # after a power cycle out of RCM, RCM must stay away this long
: "${LAB_SSH_TMO:=300}"                  # the default limit of one remote command (lab_box, lab_mpc)
: "${LAB_BOX_HOSTKEY_ALIAS:=zedbox}"     # the box's host key is pinned under this name, any address
: "${LAB_DEADMAN_FILE:=/run/fjarr-lab-ok}" # touched by lab_deadman_touch on Fjarr images
: "${LAB_DESKTOP_USER:=desktop}"         # the mini-PC's GNOME desktop account
: "${LAB_USB_SYSFS:=/sys/bus/usb/devices}"             # the mini-PC's USB devices
: "${LAB_USB_RESTORE:=/run/fjarr-zedbox-usb.restore}"  # on the mini-PC: the isolated hubs' settings
: "${LAB_USB_CTL:=0000:01:00.0}"         # the mini-PC's USB controller the box's cable is on (B2):
                                         # isolated when the box shows nothing on USB

# Worst cases, in seconds, that lab_need checks against the hard stop (L1).
# A power cycle: the plug's answers (curl -m 5 each) to the cut, 5 polls for off, the off time,
# 10 polls for on, then the switch-on fallback.
LAB_CYCLE_WORST=$((LAB_OFF_SECONDS + 140))
# One round of polling for a booting box: the USB (15), finding its MAC (5), its boot id (20), 3 s.
LAB_POLL_MAX=45
# From the reboot into RCM to the box back on ssh, with rcm.sh's one extra power cycle (L7).
LAB_RCM_ROUND_WORST=$((20 + LAB_RCM_TMO + LAB_POLL_MAX + LAB_CYCLE_WORST + LAB_RCM_GONE_S + LAB_POLL_MAX +
  2 * (LAB_BACK_TMO + LAB_POLL_MAX) + LAB_CYCLE_WORST))

LAB_SSH_COMMON=(-o BatchMode=yes -o ConnectTimeout=8 -o ServerAliveInterval=5
  -o ServerAliveCountMax=3 -o ControlMaster=no -o ControlPath=none -o LogLevel=ERROR)
# The box's host key is pinned: an unknown key is refused, so a lost known_hosts entry cannot
# make the "something else booted" check (lab_host_key_failed) go blind. LAB_PIN_HOSTKEY=1 is the
# explicit first contact with a new image, which records its key.
if [[ ${LAB_PIN_HOSTKEY:-} == 1 ]]; then LAB_HOSTKEY_MODE=accept-new; else LAB_HOSTKEY_MODE=yes; fi
LAB_SSH_BOX=("${LAB_SSH_COMMON[@]}" -o "StrictHostKeyChecking=$LAB_HOSTKEY_MODE"
  -o "UserKnownHostsFile=$LAB_DIR/known_hosts" -o "HostKeyAlias=$LAB_BOX_HOSTKEY_ALIAS")
LAB_SSH_MPC=("${LAB_SSH_COMMON[@]}")

LAB_NAME=lab LAB_LOG='' LAB_RUN='' LAB_PLUG_PASS='' LAB_BOX_IP='' LAB_IS_MINIPC='' LAB_DEADLINE=''
LAB_LOCK_MODE='' LAB_LOCK_FD='' LAB_LOCK_IN='' LAB_LOCK_OUT='' LAB_LOCK_PID='' LAB_LOCK_REPLY=''
LAB_PLUG_OUTPUT=unknown LAB_PLUG_APOWER='' LAB_LAST_SWEEP=-1000 LAB_USB_STATE=unknown
LAB_CYCLE_OFF_APOWER='' LAB_CYCLE_SECONDS='' LAB_CYCLE_ON_AT='' LAB_USB_L4T_AT='' LAB_USB_ISOLATED=0
LAB_BOOTID='' LAB_SSH_ERR='' LAB_DYING='' LAB_BG_PIDS=()
: "${LAB_DIE_HOOK:=}" # a function lab_die calls once before it exits, e.g. to record the state (L7)

# --- logging -------------------------------------------------------------------------------------

lab_log() { printf '%s %s\n' "$(TZ=$LAB_TZ date +%H:%M:%S)" "$*" >&2; }
lab_die() {
  lab_log "STOP: $*"
  if [[ -n $LAB_DIE_HOOK && -z $LAB_DYING ]]; then
    LAB_DYING=1
    "$LAB_DIE_HOOK" "$*" || true
  fi
  exit 1
}
lab_today() { TZ=$LAB_TZ date +%F; }
_lab_sq() { printf "'%s'" "${1//\'/\'\\\'\'}"; } # quote for any remote sh, busybox included
_lab_tmo() { # <s> <command...>: run it, killed after <s> s (0: no limit); exit 124 when it timed out
  local s=$1
  shift
  # --foreground: the command stays in the script's process group, so Ctrl-C reaches it.
  if ((s > 0)); then timeout --foreground -k 5 "$s" "$@"; else "$@"; fi
}

# --- start and end of a run ------------------------------------------------------------------------

lab_init() { # <name> <seconds the run needs> [nolock]
  LAB_NAME=${1:?name}
  local need=${2:?seconds} mode=${3:-lock} t
  for t in ssh curl jq ping ip awk sed timeout flock mktemp mkfifo tee; do
    command -v "$t" >/dev/null || { echo "lab: $t is not installed" >&2; exit 1; }
  done
  # A zone name this machine does not know reads as UTC, which would move the window by hours.
  if [[ $LAB_TZ == */* && ! -e /usr/share/zoneinfo/$LAB_TZ ]]; then
    echo "lab: LAB_TZ=$LAB_TZ: no such time zone on this machine (/usr/share/zoneinfo)" >&2
    exit 1
  fi
  mkdir -p "$LAB_DIR" "$LAB_DIR/logs" "$LAB_DIR/state"
  chmod 700 "$LAB_DIR"
  LAB_LOG=$LAB_DIR/logs/$(lab_today)-$LAB_NAME.log
  # Logging first: the tee processes must not inherit the lock's descriptors. They ignore the
  # terminal's signals and keep writing to the log when the terminal is gone (tee -p), so the
  # cleanup at exit still reaches the log after a Ctrl-C or a closed terminal.
  exec > >(trap '' INT HUP TERM && exec tee -p -a "$LAB_LOG") \
    2> >(trap '' INT HUP TERM && exec tee -p -a "$LAB_LOG" >&2)
  LAB_RUN=$(mktemp -d "${XDG_RUNTIME_DIR:-/tmp}/fjarr-lab.XXXXXX")
  trap _lab_exit EXIT
  trap 'exit 130' INT
  trap 'exit 143' TERM
  trap 'exit 129' HUP
  LAB_DEADLINE=$(_lab_deadline) || { lab_log "cannot compute the hard stop in $LAB_TZ"; exit 1; }
  lab_log "== $LAB_NAME: start ($*), needs up to $(((need + 59) / 60)) min; hard stop $(lab_deadline_hm) $LAB_TZ"
  if lab_on_minipc; then LAB_IS_MINIPC=1; else LAB_IS_MINIPC=0; fi
  lab_window_check "$need"
  if [[ $mode != nolock ]]; then
    [[ $LAB_BOX_MAC =~ ^([0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}$ ]] ||
      lab_die "LAB_BOX_MAC is not set: put LAB_BOX_MAC=<the box's Ethernet MAC> in $LAB_DIR/lab.env"
    lab_lock
    _lab_plug_pass
  fi
}

# The order matters: secrets first, then the USB isolation and the lock (they need the network and
# may take seconds), the log line last. A second Ctrl-C, a closed terminal or a dead log pipe must
# not cut the cleanup short.
_lab_exit() {
  local rc=$? p
  trap '' PIPE INT HUP TERM
  set +e
  LAB_PLUG_PASS=''
  for p in "${LAB_BG_PIDS[@]}"; do kill "$p" 2>/dev/null; done
  [[ -n $LAB_RUN ]] && rm -rf "$LAB_RUN"
  [[ $LAB_LOCK_MODE == local ]] && _lab_usb_restore_local
  lab_unlock
  lab_log "== $LAB_NAME: end (exit $rc)"
  return "$rc"
}

# --- L1: the nightly window and the hard stop ------------------------------------------------------

_lab_deadline() { # the epoch of the next 00:00 in LAB_TZ, less LAB_DEADLINE_MARGIN
  local midnight
  midnight=$(TZ=$LAB_TZ date -d 'tomorrow 00:00' +%s) || return 1
  printf '%s\n' "$((midnight - LAB_DEADLINE_MARGIN))"
}
lab_deadline_hm() { TZ=$LAB_TZ date -d "@$LAB_DEADLINE" +%H:%M; }
lab_time_left() { printf '%s\n' "$((LAB_DEADLINE - $(date +%s)))"; } # seconds to the hard stop

lab_need() { # <seconds> <what>: stop unless <what>, at its worst, ends before the hard stop (L1)
  local left=$((LAB_DEADLINE - $(date +%s)))
  ((left >= $1)) || lab_die "$2 needs up to $((($1 + 59) / 60)) min, and $((left > 0 ? left / 60 : 0)) min are left before the hard stop at $(lab_deadline_hm) $LAB_TZ (L1)"
}

lab_window_check() { # <seconds still needed>: refuse inside the window, or when the hard stop would be passed
  local need=${1:?seconds} h m s now
  IFS=: read -r h m s < <(TZ=$LAB_TZ date +%H:%M:%S)
  now=$((10#$h * 3600 + 10#$m * 60 + 10#$s))
  if ((now < LAB_WINDOW_END_M * 60)); then
    lab_die "inside the nightly window and fjarr-lab's grace (00:00-$(printf %02d:%02d $((LAB_WINDOW_END_M / 60)) $((LAB_WINDOW_END_M % 60))) $LAB_TZ): nothing runs now (L1)"
  fi
  lab_need "$need" "the run"
}

# --- L1: the lab lock on the mini-PC ---------------------------------------------------------------

lab_on_minipc() { # true when this script runs on the mini-PC (LAB_LOCAL=1 or 0 overrides)
  case ${LAB_LOCAL:-} in 1) return 0 ;; 0) return 1 ;; esac
  ip -4 -o addr show 2>/dev/null | awk '{ sub(/\/.*/, "", $4); print $4 }' | grep -qxF "${LAB_MINIPC#*@}"
}

_lab_lock_holders() { # $1 the lock file, $2 our own pid: who holds the lock (also run on the mini-PC)
  cat <<'EOF'
pids=$(lslocks -n -r -o PID,PATH 2>/dev/null | awk -v f="$1" '$2 == f { print $1 }' | sort -u | paste -sd, -)
[ -n "$pids" ] || pids=$(fuser "$1" 2>/dev/null | tr -s ' ' '\n' | grep -vx -e '' -e "${2:-0}" | sort -u | paste -sd, -)
[ -n "$pids" ] && ps -o pid=,user=,etime=,args= -p "$pids" 2>/dev/null |
  sed 's/sh -c .* lab-lock [^ ]* /lab lock session for /' | head -n 3 | cut -c1-160 | tr -s ' ' | paste -sd ';' -
EOF
}

# The box's USB devices as the mini-PC lists them: what lsusb -d <vendor>: shows, read from sysfs
# (no device I/O, so it cannot hang on a device that is just enumerating). sh, for the lock session.
_lab_usb_ids_sh() {
  cat <<'EOF'
usb_ids() { # <sysfs> <vendor>: "vendor:product" of each such device, comma separated
  for d in "$1"/*; do
    [ -r "$d/idVendor" ] || continue
    [ "$(cat "$d/idVendor" 2>/dev/null)" = "$2" ] || continue
    printf '%s:%s\n' "$2" "$(cat "$d/idProduct" 2>/dev/null)"
  done | sort -u | paste -sd, -
}
EOF
}

# The lock, as it runs on the mini-PC. It opens the lock file without O_CREAT when it exists:
# fs.protected_regular=2 there refuses O_CREAT on another user's file in sticky /run/lock, even
# for root. It answers "ping <token>" with "ok <token>" and "usb <token>" with "usb <token> <ids>",
# until "release" or the end of its input; then it undoes the run's USB isolation, if any, still
# holding the lock, and exits, which frees the lock. A previous run's leftover isolation is undone
# as soon as the lock is taken, when the box allows it (_lab_usb_restore_script). SIGPIPE and
# SIGHUP are ignored so that the undo always runs.
_lab_lock_script() { # $1 the lock file, $2 who takes it, $3 the USB restore file, $4 sysfs, $5 vendor
  cat <<'EOF'
f=$1 rf=$3 sys=$4 vendor=$5
trap '' PIPE HUP INT
eval "$IDS"
[ -e "$f" ] || (umask 022; : >>"$f") 2>/dev/null || true
[ -r "$f" ] || { echo "ERROR cannot read $f"; exit 1; }
exec 9<"$f"
if ! flock -n 9; then
  who=$(sh -c "$HOLDERS" holders "$f" $$ 9<&-)
  echo "BUSY ${who:-holder unknown}"; exit 75
fi
undo() { [ -e "$rf" ] || return 0; sudo -n sh -c "$RESTORE" restore "$rf" "$sys" "$vendor" 9<&- 2>&1 | tr '\n' ' '; }
left=$(undo)
[ -n "$left" ] && echo "NOTE a previous run's USB isolation: $left"
echo "LOCKED $$ $(date +%s)"
while IFS= read -r line; do
  case $line in
    "ping "*) echo "ok ${line#ping }" ;;
    "usb "*) echo "usb ${line#usb } $(usb_ids "$sys" "$vendor")" ;;
    release) break ;;
  esac
done
done_=$(undo)
echo "released${done_:+ $done_}"
EOF
}

lab_lock() {
  local reply='' remote_now skew who
  who="$LAB_NAME@$(hostname -s)"
  if ((LAB_IS_MINIPC)); then
    [[ -e $LAB_LOCK_FILE ]] || (umask 022 && : >>"$LAB_LOCK_FILE") 2>/dev/null || true
    exec {LAB_LOCK_FD}<"$LAB_LOCK_FILE" || lab_die "cannot open $LAB_LOCK_FILE"
    if ! flock -n "$LAB_LOCK_FD"; then
      exec {LAB_LOCK_FD}<&- # so that only the holders have the file open
      lab_die "the lab lock $LAB_LOCK_FILE is held by another run: $(sh -c "$(_lab_lock_holders)" holders "$LAB_LOCK_FILE" "$$")"
    fi
    LAB_LOCK_MODE=local
    lab_log "lab lock held here on the mini-PC ($LAB_LOCK_FILE)"
    if [[ -e $LAB_USB_RESTORE ]]; then
      lab_log "NOTE a previous run's USB isolation: $(_lab_usb_restore_run)"
    fi
    lab_runner_check
    return 0
  fi
  # One ssh session holds the lock for the whole run. Two FIFOs carry its input and output; the
  # session is started before any descriptor of ours exists, so it inherits none. A background
  # job of a script ignores SIGINT, and so does ssh then: a Ctrl-C ends the session through
  # _lab_exit's release, or through the end of its input when the script is gone.
  mkfifo -m 600 "$LAB_RUN/lock.in" "$LAB_RUN/lock.out"
  # ServerAliveCountMax 6 (ssh takes the first value given): a 30 s outage ends the session,
  # and with it the lock.
  ssh -o ServerAliveCountMax=6 "${LAB_SSH_MPC[@]}" "$LAB_MINIPC" \
    "HOLDERS=$(_lab_sq "$(_lab_lock_holders)") RESTORE=$(_lab_sq "$(_lab_usb_restore_script)") IDS=$(_lab_sq "$(_lab_usb_ids_sh)") sh -c $(_lab_sq "$(_lab_lock_script)") lab-lock $(_lab_sq "$LAB_LOCK_FILE") $(_lab_sq "$who") $(_lab_sq "$LAB_USB_RESTORE") $(_lab_sq "$LAB_USB_SYSFS") $(_lab_sq "${LAB_RCM_USB%%:*}")" \
    <"$LAB_RUN/lock.in" >"$LAB_RUN/lock.out" &
  LAB_LOCK_PID=$!
  exec {LAB_LOCK_OUT}>"$LAB_RUN/lock.in" {LAB_LOCK_IN}<"$LAB_RUN/lock.out"
  rm -f "$LAB_RUN/lock.in" "$LAB_RUN/lock.out"
  LAB_LOCK_MODE=starting
  while read -r -t 30 reply <&"$LAB_LOCK_IN"; do
    case $reply in
      NOTE\ *) lab_log "mini-PC: ${reply#NOTE }" ;;
      *) break ;;
    esac
  done || true
  case $reply in
    LOCKED\ *) LAB_LOCK_MODE=remote ;;
    BUSY*) lab_die "the lab lock on the mini-PC is held by another run: ${reply#BUSY }" ;;
    *) lab_die "could not take the lab lock on the mini-PC (${reply:-no answer over ssh})" ;;
  esac
  # The window is wall-clock time: the two clocks must agree.
  remote_now=${reply##* }
  skew=$((remote_now - $(date +%s)))
  ((skew > -120 && skew < 120)) || lab_die "the mini-PC's clock is ${skew} s off this machine's: fix time sync first"
  lab_lock_check
  lab_log "lab lock held on the mini-PC ($LAB_MINIPC:$LAB_LOCK_FILE) by ssh session $LAB_LOCK_PID"
  lab_runner_check
}

# L1: the mini-PC is also the nightly CI runner (fjarr-lab). Its status is read, never changed:
# a runner that is online (a job running, or one that may start) refuses the run.
lab_runner_check() {
  local st
  st=$(lab_mpc -t 20 'command -v fjarr-lab >/dev/null || exit 3; fjarr-lab status 2>&1') || {
    lab_log "mini-PC: fjarr-lab status unreadable: relying on the window alone"
    return 0
  }
  case $st in
    *"runner:"*"— offline"*) ;;
    *"runner:"*) lab_die "the mini-PC's CI runner is not offline ($(printf '%s\n' "$st" | sed -n 's/^runner: *//p')): not now (L1)" ;;
    *) lab_log "mini-PC: fjarr-lab status says nothing about the runner: relying on the window alone" ;;
  esac
}

_lab_lock_ask() { # <request>: "<request> <token>" through the lock session; LAB_LOCK_REPLY = the answer's rest
  local tok="$$.$RANDOM.$SECONDS" line t0=$SECONDS want
  LAB_LOCK_REPLY=''
  case $1 in ping) want="ok $tok" ;; *) want="$1 $tok" ;; esac
  (trap '' PIPE && printf '%s %s\n' "$1" "$tok" >&"$LAB_LOCK_OUT") 2>/dev/null || return 1
  while ((SECONDS - t0 < 15)); do
    IFS= read -r -t $((15 - (SECONDS - t0) + 1)) line <&"$LAB_LOCK_IN" || return 1
    if [[ $line == "$want" || $line == "$want "* ]]; then
      LAB_LOCK_REPLY=${line#"$want"}
      LAB_LOCK_REPLY=${LAB_LOCK_REPLY# }
      return 0
    fi # else: a late answer to an earlier question, skipped
  done
  return 1
}

lab_lock_check() { # prove the lock is still held: before anything that changes the box, plug or RCM
  case $LAB_LOCK_MODE in
    local) return 0 ;;
    remote) ;;
    *) lab_die "no lab lock held (lab_init without the lock, or the lock was lost)" ;;
  esac
  if ! _lab_lock_ask ping; then
    LAB_LOCK_MODE=lost
    lab_die "the lab lock's ssh session to the mini-PC is gone: stopping before the next action (L1, L7)"
  fi
}

lab_unlock() {
  local i line held=$LAB_LOCK_MODE
  case $LAB_LOCK_MODE in
    local) exec {LAB_LOCK_FD}<&- ;;
    remote | starting | lost)
      (trap '' PIPE && printf 'release\n' >&"$LAB_LOCK_OUT") 2>/dev/null
      exec {LAB_LOCK_OUT}>&-
      # The session undoes the USB isolation before it lets the lock go; its last line says so.
      while IFS= read -r -t 15 line <&"$LAB_LOCK_IN"; do
        case $line in
          released*)
            [[ $line == released\ * ]] && lab_log "mini-PC: ${line#released }"
            break
            ;;
        esac
      done
      exec {LAB_LOCK_IN}<&-
      for i in 1 2 3 4 5 6 7 8 9 10; do
        kill -0 "$LAB_LOCK_PID" 2>/dev/null || break
        sleep 0.5
      done
      ;;
    *) return 0 ;;
  esac
  LAB_LOCK_MODE=''
  [[ $held == local || $held == remote ]] && lab_log "lab lock released"
  return 0
}

lab_bg_detach() { # first thing in a background helper ( ... ) &: it must not keep the lock alive
  case $LAB_LOCK_MODE in
    local) exec {LAB_LOCK_FD}<&- ;;
    remote | starting | lost) exec {LAB_LOCK_OUT}>&- {LAB_LOCK_IN}<&- ;;
  esac
  LAB_LOCK_MODE=''
}

# --- the mini-PC ------------------------------------------------------------------------------------

lab_mpc() { # [-t <s>] <command>: run on the mini-PC, in place when this is the mini-PC
  local tmo=$LAB_SSH_TMO
  if [[ ${1:-} == -t ]]; then
    tmo=$2
    shift 2
  fi
  if [[ $LAB_IS_MINIPC == 1 ]]; then
    _lab_tmo "$tmo" bash -c "$1" </dev/null
  else
    _lab_tmo "$tmo" ssh -n "${LAB_SSH_MPC[@]}" "$LAB_MINIPC" "$1"
  fi
}

lab_mpc_root() { # [-t <s>] <bash script> [args...]: run as root on the mini-PC (sudo -n, never a prompt)
  local t=() script cmd a
  if [[ ${1:-} == -t ]]; then
    t=(-t "$2")
    shift 2
  fi
  script=$1
  shift
  cmd="sudo -n bash -c $(_lab_sq "$script") lab"
  for a in "$@"; do cmd+=" $(_lab_sq "$a")"; done
  lab_mpc "${t[@]}" "$cmd"
}

lab_usb_poll() { # sets LAB_USB_STATE: rcm | l4t | none | other:<ids> | unknown, the box on the mini-PC's USB
  local ids
  if [[ $LAB_LOCK_MODE == remote ]]; then
    # Through the lock session: no new login on the mini-PC, and each answer proves the lock.
    if ! _lab_lock_ask usb; then
      LAB_LOCK_MODE=lost
      lab_die "the lab lock's ssh session to the mini-PC is gone: stopping before the next action (L1, L7)"
    fi
    ids=$LAB_LOCK_REPLY
  elif ! ids=$(lab_mpc -t 20 "$(_lab_usb_ids_sh)
usb_ids $(_lab_sq "$LAB_USB_SYSFS") $(_lab_sq "${LAB_RCM_USB%%:*}")" 2>/dev/null); then
    LAB_USB_STATE=unknown
    return 0
  fi
  case ,$ids, in
    *",$LAB_RCM_USB,"*) LAB_USB_STATE=rcm ;;
    *",$LAB_L4T_USB,"*) LAB_USB_STATE=l4t ;;
    ,,) LAB_USB_STATE=none ;;
    *) LAB_USB_STATE="other:$ids" ;;
  esac
}

# --- the mini-PC's desktop: the box's USB device kept away from it --------------------------------
#
# Every boot of the box shows its USB device to the mini-PC: on the stock image a drive
# (L4T-README, which GNOME mounts into the desktop user's session when automount is on), two
# network interfaces (RNDIS and NCM, 192.168.55.100 by DHCP from the box) and a serial tty, all
# next to a desktop session that is not the lab's. So before anything makes the box appear again on
# the mini-PC's USB (a power cycle, a reboot into RCM), lab_desktop_guard isolates the box's USB
# controller on the mini-PC for the run: authorized_default 0 on its root hubs, and the box itself
# deauthorized. No driver binds to the box then, while sysfs (and lsusb) still list it by its IDs,
# which is all the lab reads. It refuses when another device shares that controller, and the
# isolation is undone when the run ends: by the lock session before it lets the lock go, by
# _lab_exit on the mini-PC, or at the latest by the next run that takes the lock (and a reboot of
# the mini-PC). The undo waits for the box: only a box that is on the mini-PC's USB and
# deauthorized lets it go, since a box still booting (after a power cycle, in a reboot, out of
# RCM) would otherwise enumerate authorized moments later, in front of the desktop. Until then the
# isolation and its restore file stay, and the next run that takes the lock (or a reboot of the
# mini-PC) undoes it. A controller with nothing else on it loses nothing by staying isolated.
# The box stays deauthorized until its next boot shows it to the mini-PC again.
#
# Neither the isolation nor the alternative happens on its own: each touches the desktop of a
# machine that is not only the lab's. LAB_DESKTOP_ISOLATE=1 says to isolate (which, the first
# time, also takes away the L4T-README drive and the 192.168.55.100 interface the desktop has now);
# LAB_DESKTOP_OK=1 says the desktop may see the box come and go. With neither, lab_desktop_guard
# refuses. The choice is a person's, made with whoever uses that desktop.

lab_desktop_automount() { # prints true | false | unknown: GNOME automount for the mini-PC's desktop user
  local v
  # shellcheck disable=SC2016 # runs on the mini-PC and expands there
  v=$(lab_mpc_root -t 20 '
    desk=
    for s in $(loginctl list-sessions --no-legend 2>/dev/null | awk "{ print \$1 }"); do
      info=$(loginctl show-session "$s" -p Name -p Seat -p Class 2>/dev/null)
      case $info in *Seat=seat0*Class=user* | *Class=user*Seat=seat0*) ;; *) continue ;; esac
      desk=$(printf "%s\n" "$info" | sed -n "s/^Name=//p"); break
    done
    [ -n "$desk" ] || desk=$1 # nobody on seat0 now: the setting of the known desktop account
    id -u "$desk" >/dev/null 2>&1 || exit 0
    sudo -u "$desk" -H env XDG_RUNTIME_DIR="/run/user/$(id -u "$desk")" \
      gsettings get org.gnome.desktop.media-handling automount 2>/dev/null' "$LAB_DESKTOP_USER" 2>/dev/null) || v=''
  case $v in true | false) echo "$v" ;; *) echo unknown ;; esac
}

# As root on the mini-PC: $1 sysfs, $2 the restore file, $3 the box's USB vendor, $4 its RCM product,
# $5 apply | check, $6 the controller to isolate when the box shows nothing on USB (LAB_USB_CTL).
# Prints one line; exit 1 when the box's controller cannot be isolated.
_lab_usb_isolate_script() {
  cat <<'EOF'
set -euo pipefail
sys=$1 rf=$2 vendor=$3 rcm=$4 mode=$5 lastctl=${6:-}
box=()
for d in "$sys"/*; do
  n=${d##*/}
  [[ $n == usb* || $n == *:* ]] && continue
  [[ -r $d/idVendor && $(<"$d/idVendor") == "$vendor" ]] && box+=("$n")
done
((${#box[@]} <= 1)) || { echo "more than one device of vendor $vendor on the mini-PC's USB: ${box[*]}"; exit 1; }
if ((${#box[@]} == 1)); then
  bus=$(<"$sys/${box[0]}/busnum")
  ctl=$(readlink -f "$sys/usb$bus/..")
  [[ -z $lastctl || ${ctl##*/} == "$lastctl" ]] || echo "note: the box is on controller ${ctl##*/}, not LAB_USB_CTL=$lastctl" >&2
else
  # The box shows nothing (off, or between boots): its controller as configured, so that its
  # next appearance is already kept from the desktop.
  [[ -n $lastctl ]] || { echo "the box is not on the mini-PC's USB, and LAB_USB_CTL names no controller to isolate"; exit 1; }
  ctl=
  for h in "$sys"/usb*; do
    [[ $(readlink -f "$h/..") == */"$lastctl" ]] && { ctl=$(readlink -f "$h/.."); break; }
  done
  [[ -n $ctl ]] || { echo "the box is not on the mini-PC's USB, and controller LAB_USB_CTL=$lastctl has no root hub there"; exit 1; }
  box=(none)
fi
hubs=()
for h in "$sys"/usb*; do [[ $(readlink -f "$h/..") == "$ctl" ]] && hubs+=("${h##*/}"); done
for h in "${hubs[@]}"; do
  for d in "$sys/${h#usb}"-*; do
    [[ -e $d ]] || continue
    n=${d##*/}
    [[ $n == *:* || $n == "${box[0]}" ]] && continue
    echo "another device ($n, $(cat "$d/idVendor" 2>/dev/null):$(cat "$d/idProduct" 2>/dev/null)) is on the box's USB controller (${hubs[*]}): isolating it would cut that device off too"
    exit 1
  done
done
if [[ $mode == check ]]; then
  echo "can isolate the box's USB controller: ${hubs[*]} (controller ${ctl##*/}), the box at ${box[0]}, nothing else on it"
  exit 0
fi
if [[ ! -e $rf ]]; then # the first isolation of this run keeps the original values
  for h in "${hubs[@]}"; do printf '%s %s\n' "$sys/$h/authorized_default" "$(<"$sys/$h/authorized_default")"; done >"$rf.new"
  mv "$rf.new" "$rf"
fi
for h in "${hubs[@]}"; do echo 0 >"$sys/$h/authorized_default"; done
what="authorized_default 0 on ${hubs[*]} (controller ${ctl##*/})"
if [[ ${box[0]} == none ]]; then
  what+=", the box not on USB now"
elif [[ $(<"$sys/${box[0]}/idProduct") != "$rcm" ]]; then # RCM binds no driver: left alone
  echo 0 >"$sys/${box[0]}/authorized"
  what+=", the box at ${box[0]} deauthorized"
fi
echo "isolated: $what"
EOF
}

# As root on the mini-PC (sh): $1 the restore file, $2 sysfs, $3 the box's USB vendor. Writes the
# saved values back and removes the file, but only when the box is on USB and deauthorized: else
# it keeps both, and says so (see above).
_lab_usb_restore_script() {
  cat <<'EOF'
rf=$1 sys=$2 vendor=$3
[ -f "$rf" ] || exit 0
seen='' auth=''
for d in "$sys"/*; do
  case ${d##*/} in usb* | *:*) continue ;; esac
  [ -r "$d/idVendor" ] && [ "$(cat "$d/idVendor" 2>/dev/null)" = "$vendor" ] || continue
  seen="$seen ${d##*/}"; auth="$auth$(cat "$d/authorized" 2>/dev/null)"
done
if [ -z "$seen" ] || [ "$auth" != 0 ]; then
  if [ -n "$seen" ]; then where="at$seen with authorized=$auth"; else where="not on USB"; fi
  echo "USB isolation kept: the box is $where, so it could still appear authorized in front of the desktop; the next run that takes the lock, or a reboot of the mini-PC, undoes it"
  exit 0
fi
hubs=
while read -r p v; do
  case $p in */usb[0-9]*/authorized_default) ;; *) continue ;; esac
  case $v in 0 | 1 | 2) ;; *) continue ;; esac
  if [ -w "$p" ] && echo "$v" >"$p"; then h=${p%/authorized_default}; hubs="$hubs ${h##*/}=$v"; fi
done <"$rf"
rm -f "$rf"
echo "USB isolation undone: authorized_default${hubs:- (nothing to restore)}"
EOF
}

_lab_usb_restore_run() { # on the mini-PC itself: undo the isolation; prints what was done
  lab_mpc_root -t 30 "$(_lab_usb_restore_script)" "$LAB_USB_RESTORE" "$LAB_USB_SYSFS" "${LAB_RCM_USB%%:*}" 2>&1 | tr '\n' ' '
}

_lab_usb_restore_local() { # _lab_exit, lock held here: undo this run's isolation
  ((LAB_USB_ISOLATED)) || [[ -e $LAB_USB_RESTORE ]] || return 0
  lab_log "mini-PC: $(_lab_usb_restore_run)"
  LAB_USB_ISOLATED=0
}

lab_usb_isolate() { # [check]: isolate the box's USB controller on the mini-PC for this run (see above)
  local out mode=${1:-apply}
  [[ $LAB_LOCK_MODE == local || $LAB_LOCK_MODE == remote ]] || lab_die "the USB isolation needs the lab lock"
  [[ $mode == check ]] || lab_lock_check
  if ! out=$(lab_mpc_root -t 30 "$(_lab_usb_isolate_script)" "$LAB_USB_SYSFS" "$LAB_USB_RESTORE" \
    "${LAB_RCM_USB%%:*}" "${LAB_RCM_USB#*:}" "$mode" "$LAB_USB_CTL" 2>&1); then
    [[ $mode == check ]] && { lab_log "mini-PC: cannot isolate the box's USB: ${out:-no answer}"; return 1; }
    lab_die "cannot isolate the box's USB on the mini-PC: ${out:-no answer}. Without that, every boot of the box shows on the mini-PC a drive (L4T-README, which GNOME automounts for the desktop user), two network interfaces and a serial tty, next to a desktop session that is not the lab's. LAB_DESKTOP_OK=1 says the desktop may see them."
  fi
  out=${out##*$'\n'}
  lab_log "mini-PC: $out"
  [[ $mode == check ]] && return 0
  LAB_USB_ISOLATED=1
  [[ $out == *"not on USB now"* ]] && return 0
  lab_usb_poll
  [[ $LAB_USB_STATE == l4t || $LAB_USB_STATE == rcm ]] ||
    lab_die "after the isolation the mini-PC sees '$LAB_USB_STATE' on the box's USB, not the box (L7)"
}

lab_desktop_guard() { # before anything that makes the box appear again on the mini-PC's USB
  [[ ${LAB_DESKTOP_OK:-} == 1 ]] && return 0
  ((LAB_USB_ISOLATED)) && return 0
  [[ ${LAB_DESKTOP_ISOLATE:-} == 1 ]] ||
    lab_die "this makes the box appear on the mini-PC's USB, next to a desktop session that is not the lab's: a drive (L4T-README, which GNOME automounts for the desktop user), two network interfaces and a serial tty. A person decides, with whoever uses that desktop: LAB_DESKTOP_ISOLATE=1 keeps the box from the desktop by deauthorizing its USB controller on the mini-PC (the first time this also takes the drive and the interface the desktop has now), LAB_DESKTOP_OK=1 lets the desktop see it"
  lab_usb_isolate
}

# --- L2: the box, found by its MAC --------------------------------------------------------------

_lab_mac_at() { # <address>: "<mac> <state>" of the neighbour entry after one probe, or nothing
  local ok=0
  ping -c 1 -W 1 -n -q "$1" >/dev/null 2>&1 && ok=1
  ip neigh show to "$1" 2>/dev/null |
    awk -v ok="$ok" '{ for (i = 1; i < NF; i++) if ($i == "lladdr") print tolower($(i + 1)), $NF, ok }'
}

_lab_mac_matches() { # <address>: the box's MAC answers there (a reply, or a fresh neighbour entry)
  local mac state ok
  read -r mac state ok < <(_lab_mac_at "$1") || return 1
  [[ $mac == "${LAB_BOX_MAC,,}" ]] && { [[ $ok == 1 ]] || [[ $state == REACHABLE ]]; }
}

_lab_sweep() { # one ping to every address of the /24, to refresh the neighbour table (at most every 30 s)
  local i pids=()
  ((SECONDS - LAB_LAST_SWEEP >= 30)) || return 0
  LAB_LAST_SWEEP=$SECONDS
  for i in $(seq 1 254); do
    ping -c 1 -W 1 -n -q "$LAB_SUBNET.$i" >/dev/null 2>&1 &
    pids+=($!)
  done
  wait "${pids[@]}" 2>/dev/null || true # only these: the lock's ssh session is a child as well
}

lab_find_box() { # sets LAB_BOX_IP to the address where the box's MAC answers; 1 when it does not
  local cache=$LAB_DIR/state/box.ip ip
  LAB_BOX_IP=
  local -a candidates=()
  [[ -r $cache ]] && candidates+=("$(<"$cache")")
  mapfile -t -O "${#candidates[@]}" candidates < <(ip neigh show | awk -v m="${LAB_BOX_MAC,,}" 'tolower($0) ~ ("lladdr " m) { print $1 }')
  for ip in "${candidates[@]}"; do
    [[ $ip =~ ^[0-9.]+$ ]] && _lab_mac_matches "$ip" && { LAB_BOX_IP=$ip; break; }
  done
  if [[ -z $LAB_BOX_IP ]]; then
    _lab_sweep
    while read -r ip; do
      _lab_mac_matches "$ip" && { LAB_BOX_IP=$ip; break; }
    done < <(ip neigh show | awk -v m="${LAB_BOX_MAC,,}" 'tolower($0) ~ ("lladdr " m) { print $1 }')
  fi
  [[ -n $LAB_BOX_IP ]] || return 1
  printf '%s\n' "$LAB_BOX_IP" >"$cache"
}

lab_box() { # [-t <s>] <command>: run on the box as $LAB_BOX_USER; its host key must match the pinned one
  local tmo=$LAB_SSH_TMO
  if [[ ${1:-} == -t ]]; then
    tmo=$2
    shift 2
  fi
  [[ -n $LAB_BOX_IP ]] || lab_find_box || return 255
  _lab_tmo "$tmo" ssh -n "${LAB_SSH_BOX[@]}" "$LAB_BOX_USER@$LAB_BOX_IP" "$1"
}

lab_box_root() { # [-t <s>] <bash script> [args...]: run as root on the box (sudo -n, never a prompt)
  local t=() script cmd a
  if [[ ${1:-} == -t ]]; then
    t=(-t "$2")
    shift 2
  fi
  script=$1
  shift
  cmd="sudo -n bash -c $(_lab_sq "$script") lab"
  for a in "$@"; do cmd+=" $(_lab_sq "$a")"; done
  lab_box "${t[@]}" "$cmd"
}

lab_box_bootid() { # sets LAB_BOOTID (empty when ssh failed) and LAB_SSH_ERR (ssh's last words); 1 on failure
  local rc=0
  LAB_BOOTID=$(lab_box -t 20 'cat /proc/sys/kernel/random/boot_id' 2>"$LAB_RUN/ssh.err") || rc=$?
  LAB_SSH_ERR=$(tail -n 3 "$LAB_RUN/ssh.err" 2>/dev/null | tr '\n' ' ')
  ((rc == 0)) && [[ -n $LAB_BOOTID ]]
}

lab_host_key_failed() { # the last lab_box_bootid failed on the pinned host key: something else answered
  [[ $LAB_SSH_ERR == *"Host key verification failed"* || $LAB_SSH_ERR == *"IDENTIFICATION HAS CHANGED"* ]]
}

lab_wait_ready() { # <timeout s>: until systemd on the box has finished starting; prints its state
  local state
  state=$(lab_box -t $(($1 + 15)) "timeout $1 systemctl is-system-running --wait 2>/dev/null || true") || state=unreachable
  printf '%s\n' "${state:-unknown}"
}

lab_backup_check() { # L2: the latest backup has a verified checksum list; 1 (and why) if not
  local top=$LAB_DIR/zedbox-backup rec sum
  [[ -s $top/SHA256SUMS && -s $top/SHA256SUMS.verified ]] || { lab_log "L2: no verified backup (run backup.sh)"; return 1; }
  read -r _ rec _ <"$top/SHA256SUMS.verified"
  sum=$(sha256sum <"$top/SHA256SUMS" | cut -d' ' -f1)
  [[ $sum == "$rec" ]] || { lab_log "L2: SHA256SUMS changed since it was verified"; return 1; }
  (cd "$top" && sha256sum -c --quiet SHA256SUMS) || { lab_log "L2: the backup does not match SHA256SUMS"; return 1; }
  lab_log "L2: backup verified ($(wc -l <"$top/SHA256SUMS") files, recorded $(cut -d' ' -f1 "$top/SHA256SUMS.verified"))"
}

lab_deadman_touch() { # Fjarr images only: keep the lab dead-man from sending the box to RCM
  lab_box -t 20 "touch $LAB_DEADMAN_FILE"
}

# --- L8: the plug ----------------------------------------------------------------------------------

_lab_plug_pass() { # once, from lab_init: the plug's password into this shell only, never into a file
  LAB_PLUG_PASS=''
  if [[ ! -r $LAB_PLUG_PASS_FILE ]]; then
    lab_log "no plug password file $LAB_PLUG_PASS_FILE: the plug cannot be reached"
    return 0
  fi
  IFS= read -r LAB_PLUG_PASS <"$LAB_PLUG_PASS_FILE" || true
  [[ -n $LAB_PLUG_PASS ]] || lab_die "the plug password file is empty"
  [[ $LAB_PLUG_PASS != *[[:space:]\"\\]* ]] || lab_die "the plug password holds a character curl's configuration cannot carry"
}

lab_plug() { # <RPC method and query>: the plug's JSON answer on stdout
  [[ -n $LAB_PLUG_PASS ]] || { lab_log "no plug password (lab_init with the lock reads it)"; return 1; }
  # The digest credentials reach curl as its configuration on stdin (printf is a builtin): never
  # on a command line, in the environment or in a file. A --netrc-file through a pipe would not do:
  # curl reads the netrc again for the digest's second request, and finds the pipe empty.
  printf 'user = "%s:%s"\n' "$LAB_PLUG_USER" "$LAB_PLUG_PASS" |
    curl -sS --fail -m 5 --digest -K - "http://$LAB_PLUG/rpc/$1"
}

lab_plug_status() { # sets LAB_PLUG_OUTPUT (true|false|unknown) and LAB_PLUG_APOWER; 1 if no answer
  local js
  LAB_PLUG_OUTPUT=unknown LAB_PLUG_APOWER=
  js=$(lab_plug 'Switch.GetStatus?id=0' 2>/dev/null) || return 1
  read -r LAB_PLUG_OUTPUT LAB_PLUG_APOWER < <(jq -r '"\(.output) \(.apower)"' <<<"$js" 2>/dev/null) || return 1
  [[ $LAB_PLUG_OUTPUT == true || $LAB_PLUG_OUTPUT == false ]] || { LAB_PLUG_OUTPUT=unknown; return 1; }
}

lab_plug_check() { # L2: the plug answers; logs its output and power
  if ! lab_plug_status; then
    lab_log "plug $LAB_PLUG: no answer"
    return 1
  fi
  lab_log "plug: output=$LAB_PLUG_OUTPUT, ${LAB_PLUG_APOWER} W"
}

lab_power_cycle() { # [off seconds]: off now, back on by the plug's own flip-back timer
  # The off command carries toggle_after, so the plug switches itself back on even if this script
  # or the network dies meanwhile. It is a one-shot timer of this command; L8 is about the plug's
  # configured auto-on/auto-off timers, which stay off.
  local off=${1:-$LAB_OFF_SECONDS} t0 i
  lab_lock_check
  lab_desktop_guard
  lab_need "$LAB_CYCLE_WORST" "a power cycle"
  lab_plug_status || lab_die "the plug does not answer: no power cycle (L2)"
  [[ $LAB_PLUG_OUTPUT == true ]] || lab_die "the plug is already off: no power cycle (L7)"
  t0=$SECONDS
  lab_log "power cycle: off for $off s"
  lab_plug "Switch.Set?id=0&on=false&toggle_after=$off" >/dev/null || lab_die "the plug refused the power cut (L7)"
  for i in 1 2 3 4 5; do
    if lab_plug_status && [[ $LAB_PLUG_OUTPUT == false ]]; then break; fi
    sleep 1
  done
  [[ $LAB_PLUG_OUTPUT == false ]] || lab_die "the plug does not report off after the cut (L7)"
  LAB_CYCLE_OFF_APOWER=$LAB_PLUG_APOWER
  while ((SECONDS - t0 < off)); do sleep 1; done
  for i in $(seq 1 10); do
    if lab_plug_status && [[ $LAB_PLUG_OUTPUT == true ]]; then break; fi
    sleep 1
  done
  if [[ $LAB_PLUG_OUTPUT != true ]]; then
    # On first: it restores the state before the cycle, whatever comes next. Then the lock.
    lab_log "the plug's flip-back timer did not switch on: switching on"
    lab_plug 'Switch.Set?id=0&on=true' >/dev/null || true
    sleep 2
    lab_plug_status || true
    lab_lock_check
  fi
  [[ $LAB_PLUG_OUTPUT == true ]] || lab_die "the plug stays off after the power cycle: switch it on by hand (L7)"
  # shellcheck disable=SC2034 # read by the scripts: the moment of power on
  LAB_CYCLE_ON_AT=$SECONDS
  LAB_CYCLE_SECONDS=$((SECONDS - t0))
  lab_log "power cycle: on again after $LAB_CYCLE_SECONDS s (off: ${LAB_CYCLE_OFF_APOWER} W)"
}

# --- RCM ----------------------------------------------------------------------------------------------

lab_rcm_enter() { # the box reboots into RCM by software; it must run L4T and answer ssh just before
  local rc=0
  lab_lock_check
  lab_desktop_guard
  lab_need "$LAB_RCM_ROUND_WORST" "a round trip through RCM"
  lab_usb_poll
  [[ $LAB_USB_STATE == l4t ]] || lab_die "before RCM the mini-PC sees '$LAB_USB_STATE' on the box's USB, not L4T ($LAB_L4T_USB) (L7)"
  [[ -n $LAB_BOX_IP ]] || lab_find_box || lab_die "the box does not answer on its MAC (L2)"
  # Just before: ssh and sudo work, and the disk is synced. After this, an ssh exit of 255 can only
  # be the session closed by the reboot.
  lab_box -t 20 'sudo -n sync' >/dev/null 2>&1 || lab_die "the box does not answer ssh or sudo just before the reboot: nothing sent (L7)"
  lab_lock_check
  lab_log "box: reboot --force forced-recovery"
  # The session ends with the reboot: 255 (closed) and 124 (timed out) are expected, 0 too.
  _lab_tmo 20 ssh -n "${LAB_SSH_BOX[@]}" "$LAB_BOX_USER@$LAB_BOX_IP" 'sudo -n reboot --force forced-recovery' \
    >/dev/null 2>&1 || rc=$?
  case $rc in
    0 | 124 | 255) ;;
    *) lab_die "the box refused 'reboot --force forced-recovery' (exit $rc) (L7)" ;;
  esac
}

lab_rcm_wait() { # [timeout s]: until the box shows in RCM on the mini-PC's USB; LAB_USB_STATE: the last seen
  local timeout=${1:-$LAB_RCM_TMO} t0=$SECONDS
  while ((SECONDS - t0 < timeout)); do
    lab_usb_poll
    [[ $LAB_USB_STATE == rcm ]] && return 0
    sleep 2
  done
  return 1
}

lab_rcm_leave() { # a power cycle out of RCM; RCM must stay away for LAB_RCM_GONE_S (L7)
  local t0
  LAB_USB_L4T_AT=''
  lab_power_cycle
  t0=$SECONDS
  # A box that falls back into RCM shows within a second or two of power on; L4T takes longer.
  while ((SECONDS - t0 < LAB_RCM_GONE_S)); do
    lab_usb_poll
    case $LAB_USB_STATE in
      rcm) lab_die "the box is in RCM again after the power cycle (L7)" ;;
      other:*) lab_die "the mini-PC sees '$LAB_USB_STATE' on the box's USB after the power cycle (L7)" ;;
      l4t)
        # shellcheck disable=SC2034 # read by the scripts: L4T on USB
        LAB_USB_L4T_AT=$SECONDS
        return 0
        ;;
    esac
    sleep 2
  done
  return 0
}

# --- results: redaction of the box's identifiers -------------------------------------------------
#
# Committed results never carry serial numbers, MACs, disk or partition GUIDs, machine IDs or host
# names. The probes collect the machine's own identifiers into a secrets file under .lab ("<kind>
# <value>" per line); lab_redact replaces each of them, in any letter case and with or without
# separators, then MAC- and IPv6-shaped strings, serial-number fields and host-name fields (a DHCP
# lease's host_name, which the router may hold under another name, and hostnamectl's Static hostname)
# in general. lab_leak_check refuses a result in which any of them survived.

_lab_secret_re() { # <value>: an extended regex for the value, hex bytes with optional separators
  local v=$1 h out='' i
  h=${v//[-: .]/}
  if [[ $h =~ ^[0-9A-Fa-f]+$ ]] && ((${#h} >= 8 && ${#h} % 2 == 0)); then
    for ((i = 0; i < ${#h}; i += 2)); do out+=${out:+'[-: .]?'}${h:i:2}; done
  else
    out=$(printf '%s' "$v" | sed 's/[][\\.^$*+?(){}|/]/\\&/g')
  fi
  printf '%s\n' "$out"
}

_lab_secrets() { # <secrets file>: "<kind> <value>" lines worth redacting
  local kind value
  local filler='^(none|n/a|unknown|not specified|not applicable|default string|to be filled by o\.e\.m\.|0x0+)$'
  local blank='^[0f:. -]+$' # 00:00:..., ffff..., all-zero GUIDs
  while read -r kind value; do
    [[ ${#value} -ge 6 ]] || continue
    [[ ${value,,} =~ $filler || ${value,,} =~ $blank ]] && continue
    printf '%s %s\n' "$kind" "$value"
  done <"$1"
}

lab_redact() { # <secrets file>: stdin to stdout, identifiers replaced by <kind>
  local secrets=$1 kind value sedf
  sedf=$(mktemp "${LAB_RUN:-/tmp}/redact.XXXXXX")
  {
    while read -r kind value; do
      printf 's\001%s\001<%s>\001gI\n' "$(_lab_secret_re "$value")" "$kind"
    done < <(_lab_secrets "$secrets" | awk '{ print length($0), $0 }' | sort -rn | cut -d' ' -f2-)
    printf '%s\n' \
      's/([0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2}/<mac>/g' \
      's/\benx[0-9A-Fa-f]{12}\b/enx<mac>/g' \
      's/(MAC[(:])[0-9A-Fa-f]{12}/\1<mac>/g' \
      's/\b([0-9A-Fa-f]{1,4}:){7}[0-9A-Fa-f]{1,4}\b/<ipv6>/g' \
      's/\b([0-9A-Fa-f]{1,4}:)+:([0-9A-Fa-f]{1,4}(:[0-9A-Fa-f]{1,4})*)?/<ipv6>/g' \
      's/([Ss]erial[ _-]?([Nn]umber|NUMBER|[Nn]o\.?)?[[:space:]]*[:=][[:space:]]*)[^[:space:],;]+/\1<serial>/g' \
      's/\b(eui|nguid|naa|t10)\.[0-9A-Za-z._-]+/\1.<id>/g' \
      's/(^|[^_[:alnum:]])(host_name[[:space:]]*=[[:space:]]*)[^[:space:]]+/\1\2<hostname>/g' \
      's/(Static hostname:[[:space:]]*)[^[:space:]]+/\1<hostname>/g'
  } >"$sedf"
  sed -E -f "$sedf"
  rm -f "$sedf"
}

lab_leak_check() { # <file> <secrets file>: 1 when an identifier or a MAC survived in <file>
  local file=$1 secrets=$2 kind value n=0 relist
  relist=$(mktemp "${LAB_RUN:-/tmp}/leak.XXXXXX")
  while read -r kind value; do _lab_secret_re "$value"; done < <(_lab_secrets "$secrets") >"$relist"
  if [[ -s $relist ]] && grep -aiqE -f "$relist" "$file"; then
    lab_log "LEAK: $(grep -aicE -f "$relist" "$file") line(s) of $file still hold a collected identifier"
    n=1
  fi
  if grep -aiqE '([0-9a-f]{2}[:-]){5}[0-9a-f]{2}' "$file"; then
    lab_log "LEAK: $file still holds a MAC-shaped string"
    n=1
  fi
  rm -f "$relist"
  return "$n"
}

# The identifiers of the machine it runs on, as root: "<kind> <value>" lines for lab_redact.
# shellcheck disable=SC2034 # used by the scripts that source this file
LAB_SECRETS_SCRIPT=$(
  cat <<'EOF'
emit() { k=$1; shift; for v in "$@"; do
  v=$(printf '%s' "$v" | tr -d '\000' | sed 's/^[[:space:]]*//; s/[[:space:]]*$//')
  [ -n "$v" ] && printf '%s %s\n' "$k" "$v"; done; }
h=$(hostname); emit hostname "$h" "$(cat /etc/hostname 2>/dev/null)"
case $h in *-*) emit hostname "${h##*-}" ;; esac
for f in /sys/class/net/*/address; do emit mac "$(tr -d '\000' <"$f")"; done
for i in /sys/class/net/*; do emit mac "$(ethtool -P "${i##*/}" 2>/dev/null | awk '{ print $NF }')"; done
for f in /sys/kernel/config/usb_gadget/*/functions/*/host_addr /sys/kernel/config/usb_gadget/*/functions/*/dev_addr; do
  [ -r "$f" ] && emit mac "$(tr -d '\000' <"$f")"; done
for f in /sys/class/nvme/*/serial /sys/class/nvme/*/subsysnqn /sys/class/nvme/*/nvme*n*/eui \
  /sys/class/nvme/*/nvme*n*/nguid /sys/class/nvme/*/nvme*n*/uuid /sys/class/nvme/*/nvme*n*/wwid \
  /sys/block/*/device/serial /sys/block/*/device/wwid /sys/class/dmi/id/product_serial \
  /sys/class/dmi/id/board_serial /sys/class/dmi/id/chassis_serial /sys/class/dmi/id/product_uuid \
  /sys/class/dmi/id/board_asset_tag /sys/class/dmi/id/chassis_asset_tag /proc/device-tree/serial-number \
  /sys/devices/soc0/serial_number /sys/module/tegra_fuse/parameters/tegra_chip_uid; do
  [ -r "$f" ] && [ -f "$f" ] && emit serial "$(tr -d '\000' <"$f" 2>/dev/null)"; done
emit id "$(cat /etc/machine-id 2>/dev/null)" "$(cat /var/lib/dbus/machine-id 2>/dev/null)"
lsblk -dnro PTUUID 2>/dev/null | while read -r v; do emit uuid "$v"; done
lsblk -nro PARTUUID,UUID 2>/dev/null | tr ' ' '\n' | while read -r v; do emit uuid "$v"; done
for f in /etc/ssh/ssh_host_*_key.pub; do [ -r "$f" ] && emit key "$(awk '{ print $2 }' "$f")"; done
exit 0
EOF
)
