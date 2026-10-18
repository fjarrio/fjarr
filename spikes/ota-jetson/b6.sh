#!/usr/bin/env bash
# shellcheck disable=SC2016 # the single-quoted scripts run on the box or the mini-PC, and expand there
# b6.sh: one-shot boot tests on the stock ZED Box Mini through BootNext (slice 8.0, phase B6).
# spec: docs/32-fleet-ota.md#jetson-test-image (phase B, "Three rescue layers", rules L1-L8)
#
#   b6.sh check           every test's preconditions, read only: changes nothing anywhere
#   b6.sh t2              BootNext honoured and consumed (the NVMe entry), then the PXEv4 entry with
#                         no rescue server: the fall-through to the NVMe, timed
#   b6.sh net-up          serve the network rescue from the mini-PC (rescue/net-rescue.sh)
#   b6.sh t5 [--deadman | --http]
#                         the network rescue by PXEv4, then by HTTPv4: ssh, the NVMe read only, the
#                         NIC, RCM by reboot-recovery (--deadman: the last one by its dead-man;
#                         --http: HTTPv4 only, after a stop in the PXE step)
#   b6.sh t4              the stock recovery through a bootmode=recovery entry, with the safety script
#   b6.sh t6              Fjarr's rescue (A5) in recovery, through the T4 entry; skipped without it
#   b6.sh t6 --restore    recovery and recovery-dtb back as the backup has them, checked by sha256
#   b6.sh t3              the watchdog and panic tests, last
#   b6.sh serial          the box's USB serial console (cdc_acm on the mini-PC) during one reboot
#   b6.sh clean           remove what T4 and T3 leave on the box: the entry, the safety script, the
#                         drop-in
#   b6.sh net-down        stop serving the network rescue (net-rescue.sh --undo); takes no lock, so
#                         it refuses while another lab run holds it
#
# The order is the plan's: t2, net-up, t5, t4, t6, t3, then clean and net-down. Each test needs the
# one before it to have passed (.lab/b6/<test>.pass; t6 may be skipped). The network rescue stays
# up from net-up to net-down: L2 wants it serving for every write after T5 (T4's entry and script,
# T6's recovery partition, T3's resets), and t2 refuses while it serves (its fall-through needs
# no server). T2 is L2's one exception: its BootNext writes come before the network rescue is
# proven, to entries already in BootOrder, since its PXE step must find no server. Every stop
# says when the network rescue is still serving; net-down ends it at the end of each session.
#
# Every boot here is one-shot: BootNext is consumed by the firmware as it boots, so a power cycle
# returns the box to its stock system. Each test checks its preconditions first (L2 for anything
# that writes: the plug answers, the box answers on its MAC, a verified backup, and from T5 on the
# network rescue serving), never leaves BootNext set (L5: every stop, signal or failed command
# removes a BootNext it finds on whatever answers on the box's MAC, since every test starts with
# none and only b6.sh sets it), allows at most two warm resets without the stock verifier in
# between and none while L4TLauncher's own count for slot A is below 2 (L6), and stops at the
# first unexpected state with the state recorded (L7): a box that shows nothing at all, or that
# sits in the stock recovery (no dead-man, no way to RCM), gets exactly one power cycle, then the
# run stops either way. One CSV line per step goes to results/<date>-b6.csv; the details, with the box's
# identifiers replaced, to results/<date>-b6-<test>.txt.
#
# Each boot shows the box's USB device to the mini-PC's desktop: LAB_DESKTOP_ISOLATE=1 or
# LAB_DESKTOP_OK=1, as for rcm.sh (lib/lab.sh, lab_desktop_guard). serial needs LAB_DESKTOP_OK=1,
# since an isolated controller binds no tty.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=lib/lab.sh
. "$here/lib/lab.sh"

cmd=${1:-help}
opt=${2:-}

# --- settings ---------------------------------------------------------------------------------------

B6_DIR=$LAB_DIR/b6
: "${B6_NET_EFI:=$LAB_HOME/out/rescue/36.4.4/fjarr-net-rescue.efi}"
: "${B6_NET_KNOWN_HOSTS:=$LAB_HOME/out/rescue/36.4.4/known_hosts}"
: "${B6_MPC_DIR:=fjarr-net-rescue}" # on the mini-PC, under the lab user's home unless absolute
: "${B6_A5_DIR:=$LAB_HOME/out/yocto/build/tmp/deploy/images/zedbox-mini-orin-nx16}"
: "${B6_A5_IMG:=}"                  # the A5 rescue's Android boot image, else found in B6_A5_DIR
: "${B6_A5_DTB:=}"                  # its device tree, else found in B6_A5_DIR
: "${B6_A5_HOSTKEY_PUB:=$LAB_DIR/yocto/ssh/ssh_host_ed25519_key.pub}"
: "${B6_PUBKEYS:=}"                 # the keys the stock recovery's sshd takes, else ~/.ssh/id_*.pub
: "${B6_NET_TMO:=900}"              # from the reboot to the network rescue on ssh (a 50 MB download)
: "${B6_FALL_TMO:=900}"             # from the reboot to the stock system after a failed network boot
: "${B6_REC_TMO:=600}"              # from the reboot to a recovery-partition system on ssh
: "${B6_HANG_S:=300}"               # how long a panic with kernel.panic=0 may stay without a reset
: "${B6_DEADMAN_S:=1500}"           # from the last touch to RCM by a rescue's dead-man (20 min + 5)
: "${B6_UPLOAD_TMO:=3600}"          # one upload (the workstation's uplink can be slow)
: "${B6_SERIAL_S:=300}"             # how long serial watches the tty
: "${B6_SERIAL_POKE:=1}"            # 1: send one carriage return to the tty, for the login prompt

SSH_TMO=20 READY_TMO=180 DOWN_TMO=120
NV=781e084c-a330-417c-b678-38e696380cb9   # NVIDIA's UEFI variables
EFIG=8be4df61-93ca-11d2-aa0d-00e098032b8c # the UEFI global variables
ENTRY_LABEL=fjarr-recovery
ENTRY_OPTS=bootmode=recovery
NET_ALIAS=fjarr-net-rescue REC_ALIAS=zedbox-stock-recovery A5_ALIAS=fjarr-recovery-rescue
OTA_DIR=/ota_work
# Last in sort order: the stock box's own watchdog.conf there sets RuntimeWatchdogSec=120 and,
# sorting after a 90- name, outvoted it (B6 T3 on 2026-10-18).
DROPIN=/etc/systemd/system.conf.d/zz-fjarr-b6-no-watchdog.conf
ONBOX=/var/lib/fjarr-b6 # on the box's APP: T6's copies of recovery and recovery-dtb
PMC=/sys/devices/platform/bus@0/c360000.pmc
# What must not change across a one-shot boot: the UEFI variables always (T3 included), the PMC's
# slot registers except in T3, which measures them.
CRIT_UEFI=(BootOrder L4TDefaultBootMode RootfsRedundancyLevel RootfsRetryCountMax RootfsStatusSlotA
  RootfsStatusSlotB BootChainFwCurrent BootChainFwNext BootChainFwStatus)
CRIT=("${CRIT_UEFI[@]}" boot_chain_current rootfs_current)
SR_RF_MAGIC=0xFACE # L4TLauncher's mark of a set-up PMC rootfs register (L4TRootfsValidation.h)

boot_worst() { # <seconds to come up>: one boot step at its worst, with the one power cycle back
  echo $((20 + DOWN_TMO + $1 + LAB_POLL_MAX + LAB_CYCLE_WORST + LAB_BACK_TMO + LAB_POLL_MAX + READY_TMO + 120))
}

usage() {
  sed -n '6,20p' "$0" >&2
  exit 2
}

case $cmd in
  check) need=900 mode=lock ;;
  t2) need=$((2 * $(boot_worst "$B6_FALL_TMO") + 600)) mode=lock ;;
  net-up) need=$((B6_UPLOAD_TMO + 1200)) mode=lock ;;
  t5) need=$((1200 + 2 * ($(boot_worst "$B6_NET_TMO") + LAB_RCM_ROUND_WORST) + B6_DEADMAN_S)) mode=lock ;; # net-up's upload: its own lab_need
  t4) need=$(($(boot_worst "$B6_REC_TMO") + 900)) mode=lock ;;
  t6) need=$((3 * B6_UPLOAD_TMO + $(boot_worst "$B6_REC_TMO") + B6_DEADMAN_S + LAB_RCM_ROUND_WORST + 1200)) mode=lock ;;
  t3) need=$((300 + 4 * $(boot_worst "$B6_HANG_S") + 900)) mode=lock ;;
  serial) need=$(($(boot_worst "$LAB_BACK_TMO") + B6_SERIAL_S + 300)) mode=lock ;;
  clean) need=900 mode=lock ;;
  net-down) need=600 mode=nolock ;;
  *) usage ;;
esac
case $cmd:$opt in
  t5: | t5:--deadman | t5:--http | t6: | t6:--restore | *:) ;;
  *) usage ;;
esac

TEST=$cmd
[[ $cmd == t6 && $opt != --restore ]] && ! { [[ -n $B6_A5_IMG ]] || compgen -G "$B6_A5_DIR/*recovery*.img" >/dev/null || compgen -G "$B6_A5_DIR/*rescue*.img" >/dev/null; } && mode=nolock need=60 # the skip: no box, no time
lab_init "b6-$cmd" "$need" "$mode"
mkdir -p "$B6_DIR" "$B6_DIR/raw"
chmod 700 "$B6_DIR"
DETAIL=$LAB_RUN/detail.txt
SECRETS=$B6_DIR/secrets
: >"$DETAIL"

# --- results: one CSV line per step, a redacted detail file per run ------------------------------

run=$(date -u +%Y%m%dT%H%M%SZ)
csv=$LAB_HOME/results/$(lab_today)-b6.csv
COLS=(run test step start_utc entry pmc_before came_up_as to_down_s to_up_s to_ready_s boot_current
  boot_next_after reset_reason retry_a status_a note result)
declare -A R=() BASE=() NOW=() PRE=()
IN_STEP=0 STOP_CODE='' BN_PENDING=0 BN_EVER=0 SIGNALLED=0 BASE_BOOT='' BASE_READY='' T_CMD=0 DOWN_AT='' UP=nothing
UP_AT='' UP_BOOT='' WHO=none WHO_BOOT='' MPC_D='' NET_STATUS='' EFI_SHA=''
E_NVME='' E_PXE4='' E_HTTP4='' E_FJARR=''

step_begin() { # <step>: a new CSV line
  R=([run]=$run [test]=$TEST [step]=$1 [start_utc]=$(date -u +%FT%TZ))
  IN_STEP=1 STOP_CODE=''
  lab_log "-- $TEST: $1"
}
note() { R[note]="${R[note]:+${R[note]} }$*"; }
row() { # <result>: the step's line, redacted like the details (the CSV is committed)
  local c v out='' one=$LAB_RUN/row.csv
  R[result]=$1
  for c in "${COLS[@]}"; do
    v=${R[$c]:-}
    v=${v//,/;}
    v=${v//$'\n'/ }
    out+=${out:+,}$v
  done
  printf '%s\n' "$out" >>"$B6_DIR/raw/$(lab_today)-b6.csv" # the unredacted line, kept in .lab
  if [[ -s $SECRETS ]]; then
    printf '%s\n' "$out" | lab_redact "$SECRETS" >"$one"
  else
    printf '%s\n' "$out" >"$one"
  fi
  if ! lab_leak_check "$one" "${SECRETS}" 2>/dev/null; then
    R[note]="(the note held an identifier: it is in .lab/b6/raw/$(lab_today)-b6.csv)"
    out=''
    for c in "${COLS[@]}"; do
      v=${R[$c]:-}
      v=${v//,/;}
      v=${v//$'\n'/ }
      out+=${out:+,}$v
    done
    printf '%s\n' "$out" | lab_redact "$SECRETS" >"$one" 2>/dev/null || printf '%s\n' "$out" >"$one"
  fi
  [[ -s $csv ]] || (IFS=, && echo "${COLS[*]}") >"$csv"
  cat "$one" >>"$csv"
  IN_STEP=0
  lab_log "$TEST ${R[step]}: $1${R[note]:+ (${R[note]})}"
}
det() { printf '\n===== %s\n%s\n' "$1" "$2" >>"$DETAIL"; } # <title> <text>
det_box() { # <title> <identity> <command>: its output into the details, its failure too
  local out
  out=$(on_box_as -t 60 "$2" "$3" 2>&1) || out+=$'\n'"[exit $?]"
  det "$1" "$out"
}

results_out() { # the details, redacted, into results/; the raw copy stays in .lab/b6/raw
  local out red=$LAB_RUN/detail.red stamp
  [[ -s $DETAIL ]] || return 0
  stamp=$(TZ=$LAB_TZ date +%Y%m%dT%H%M%S)
  cp "$DETAIL" "$B6_DIR/raw/$stamp-$TEST.txt"
  out=$LAB_HOME/results/$(lab_today)-b6-$TEST.txt
  [[ -e $out ]] && out=${out%.txt}-$(TZ=$LAB_TZ date +%H%M).txt
  if ! [[ -s $SECRETS ]] || ! grep -q '^hostname ' "$SECRETS"; then
    lab_log "no identifiers collected from the box: the details stay in .lab/b6/raw/$stamp-$TEST.txt"
    return 1
  fi
  {
    printf '# ZED Box Mini, phase B6 %s: %s %s\n' "$TEST" "$(TZ=$LAB_TZ date '+%Y-%m-%d %H:%M')" "$LAB_TZ"
    printf '# spec: docs/32-fleet-ota.md#jetson-test-image. Written by b6.sh; the steps are in\n'
    printf '# %s (run %s). Host names, MACs, serial numbers, GUIDs and machine IDs are <kind>.\n' "${csv##*/}" "$run"
    lab_redact "$SECRETS" <"$DETAIL"
  } >"$red"
  if lab_leak_check "$red" "$SECRETS"; then
    cp "$red" "$out"
    lab_log "written: ${out#"$LAB_HOME"/}"
  else
    lab_log "an identifier survived the redaction: the details stay in .lab/b6/raw/$stamp-$TEST.txt"
    return 1
  fi
}

collect_secrets() { # the box's and the mini-PC's identifiers, for the redaction (kept in .lab/b6)
  local new=$LAB_RUN/secrets.new
  lab_box_root -t 60 "$LAB_SECRETS_SCRIPT" >"$new" || lab_die "could not collect the box's identifiers"
  grep -q '^hostname ' "$new" || lab_die "no host name among the box's identifiers: not redacting blind"
  lab_mpc_root -t 60 "$LAB_SECRETS_SCRIPT" >>"$new" 2>/dev/null || lab_log "the mini-PC's identifiers: not collected"
  [[ -s $SECRETS ]] && cat "$SECRETS" >>"$new"
  sort -u "$new" >"$SECRETS"
  chmod 600 "$SECRETS"
}

marker_set() { printf '%s %s\n' "$(date -u +%FT%TZ)" "${2:-}" >"$B6_DIR/$TEST.$1"; } # <pass|skip> [note]
marker_need() { # <test> [skip-ok]: the plan's order
  [[ -e $B6_DIR/$1.pass ]] && return 0
  [[ ${2:-} == skip-ok && -e $B6_DIR/$1.skip ]] && return 0
  lab_die "$1 has not passed (no .lab/b6/$1.pass): the order is t2, t5, t4, t6, t3"
}

# --- L6: warm resets since the stock verifier last ran --------------------------------------------

warm_get() { cat "$B6_DIR/warm-resets" 2>/dev/null || echo 0; }
warm_guard() { # before a warm reset: the lab's count, and L4TLauncher's own count for slot A in the PMC
  local n m c
  n=$(warm_get)
  ((n < 2)) || lab_die "$n warm resets since the last verified boot: no third (L6)"
  # L4TLauncher takes one try from slot A on each boot through it once the register carries its
  # magic, and boots the recovery at 0 (BOOT_ONLY, L4TRootfsValidation.c); the stock verifier
  # leaves the register cleared (magic 0), so the next boot sets it up afresh with 3 tries.
  read_state PRE || lab_die "could not read the PMC's rootfs register before the reset (L6)"
  m=${PRE[rootfs_sr_magic]:-?} c=${PRE[rootfs_retry_count_a]:-?}
  [[ $m =~ ^(0x)?[0-9a-fA-F]+$ && $c =~ ^(0x)?[0-9a-fA-F]+$ ]] || lab_die "the PMC's rootfs register reads magic '$m', slot A count '$c' (L6, L7)"
  [[ $m == 0x* ]] || m=0x$m
  [[ $c == 0x* ]] || c=0x$c
  R[pmc_before]="magic=$m/retry_a=$c"
  if ((m == SR_RF_MAGIC && c < 2)); then
    lab_die "L4TLauncher's count for slot A is $c: a warm reset now could leave the next one to the recovery (L6)"
  fi
}
warm_add() { echo $(($(warm_get) + 1)) >"$B6_DIR/warm-resets"; }

verified_check() { # the stock verifier (nv-l4t-bootloader-config: nvbootctrl verify) ran in this boot
  local out
  out=$(lab_box -t 30 'systemctl show -p Result -p ExecMainExitTimestampMonotonic nv-l4t-bootloader-config.service
sudo -n journalctl -b -o cat --no-pager -u nv-l4t-bootloader-config.service 2>/dev/null | grep -c "Verifying bootloader and rootfs status" || true' 2>/dev/null) || return 1
  grep -qx 'Result=success' <<<"$out" || return 1
  ! grep -qx 'ExecMainExitTimestampMonotonic=0' <<<"$out" || return 1
  [[ $(tail -n 1 <<<"$out") =~ ^[1-9] ]] || return 1
  echo 0 >"$B6_DIR/warm-resets"
}

# --- who answers on the box's MAC: the stock system, a rescue, or the stock recovery ---------------

id_opts() { # <identity>: ID_USER and ID_OPTS for ssh to it, each with its own pinned host key
  case $1 in
    stock) ID_USER=$LAB_BOX_USER ID_OPTS=("${LAB_SSH_BOX[@]}") ;;
    net) ID_USER=root ID_OPTS=("${LAB_SSH_COMMON[@]}" -o StrictHostKeyChecking=yes
      -o "UserKnownHostsFile=$B6_NET_KNOWN_HOSTS" -o "HostKeyAlias=$NET_ALIAS") ;;
    rec) ID_USER=root ID_OPTS=("${LAB_SSH_COMMON[@]}" -o StrictHostKeyChecking=yes
      -o "UserKnownHostsFile=$B6_DIR/known_hosts" -o "HostKeyAlias=$REC_ALIAS") ;;
    a5) ID_USER=root ID_OPTS=("${LAB_SSH_COMMON[@]}" -o StrictHostKeyChecking=yes
      -o "UserKnownHostsFile=$B6_DIR/a5-known_hosts" -o "HostKeyAlias=$A5_ALIAS") ;;
    *) lab_die "unknown identity $1" ;;
  esac
}

on_box_as() { # [-t s] <identity> <command>
  local tmo=$SSH_TMO
  if [[ ${1:-} == -t ]]; then
    tmo=$2
    shift 2
  fi
  id_opts "$1"
  [[ -n $LAB_BOX_IP ]] || lab_find_box || return 255
  _lab_tmo "$tmo" ssh -n "${ID_OPTS[@]}" "$ID_USER@$LAB_BOX_IP" "$2"
}

on_box_root() { # [-t s] <identity> <script> [args]: as root; bash on the stock system, sh elsewhere
  local t=() id script c a
  if [[ ${1:-} == -t ]]; then
    t=(-t "$2")
    shift 2
  fi
  id=$1 script=$2
  shift 2
  if [[ $id == stock ]]; then c="sudo -n bash -c $(_lab_sq "$script") b6"; else c="sh -c $(_lab_sq "$script") b6"; fi
  for a in "$@"; do c+=" $(_lab_sq "$a")"; done
  on_box_as "${t[@]}" "$id" "$c"
}

box_root_stdin() { # <timeout s> <script> [args]: as root on the stock system, standard input passed on
  local tmo=$1 script=$2 c a
  shift 2
  c="sudo -n bash -c $(_lab_sq "$script") b6"
  for a in "$@"; do c+=" $(_lab_sq "$a")"; done
  [[ -n $LAB_BOX_IP ]] || lab_find_box || return 255
  _lab_tmo "$tmo" ssh "${LAB_SSH_BOX[@]}" "$LAB_BOX_USER@$LAB_BOX_IP" "$c"
}

who_answers() { # sets WHO (stock net rec a5 | hostkey | <id>-noauth | none) and WHO_BOOT; 0 when one answers
  local id err rc
  WHO=none WHO_BOOT=''
  LAB_BOX_IP=''
  lab_find_box || return 1
  for id in stock net rec a5; do
    case $id in
      net) [[ -s $B6_NET_KNOWN_HOSTS ]] || continue ;;
      rec) [[ -s $B6_DIR/known_hosts ]] || continue ;;
      a5) [[ -s $B6_DIR/a5-known_hosts ]] || continue ;;
    esac
    rc=0
    WHO_BOOT=$(on_box_as -t 20 "$id" 'cat /proc/sys/kernel/random/boot_id' 2>"$LAB_RUN/who.err") || rc=$?
    if ((rc == 0)) && [[ -n $WHO_BOOT ]]; then
      WHO=$id
      return 0
    fi
    err=$(tail -n 4 "$LAB_RUN/who.err" 2>/dev/null | tr '\n' ' ')
    case $err in
      *"Host key verification failed"* | *"IDENTIFICATION HAS CHANGED"*) WHO=hostkey ;;
      *"Permission denied"*)
        WHO=$id-noauth
        return 1
        ;;
      *)
        WHO=none # refused or timed out: nothing listens yet, whoever it is
        return 1
        ;;
    esac
  done
  return 1
}

wait_down() { # <old stock boot id> <s>: until that boot no longer answers; sets DOWN_AT
  local old=$1 t0=$SECONDS b
  while ((SECONDS - t0 < $2)); do
    b=$(lab_box -t 8 'cat /proc/sys/kernel/random/boot_id' 2>/dev/null) || b=''
    if [[ $b != "$old" ]]; then
      DOWN_AT=$SECONDS
      return 0
    fi
    sleep 1
  done
  return 1
}

wait_up() { # <s> <old stock boot id> <accepted...>: sets UP (what answered) and UP_AT, UP_BOOT; 0 if accepted
  local limit=$1 old=$2 t0=$SECONDS accept
  shift 2
  accept=" $* "
  UP=nothing UP_AT='' UP_BOOT=''
  while ((SECONDS - t0 < limit)); do
    lab_usb_poll
    case $LAB_USB_STATE in
      rcm)
        UP=rcm UP_AT=$SECONDS
        [[ $accept == *" rcm "* ]]
        return
        ;;
      other:*)
        UP="usb-$LAB_USB_STATE"
        return 1
        ;;
    esac
    if who_answers; then
      UP=$WHO UP_AT=$SECONDS UP_BOOT=$WHO_BOOT
      if [[ $WHO == stock && $WHO_BOOT == "$old" ]]; then
        UP=same-boot
        return 1
      fi
      BN_PENDING=0 # a new boot: the firmware has read BootNext (whether it consumed it is checked after)
      [[ $accept == *" $WHO "* ]]
      return
    fi
    case $WHO in
      hostkey | *-noauth)
        UP=$WHO
        return 1
        ;;
    esac
    sleep 3
  done
  return 1
}

# --- the boot state ----------------------------------------------------------------------------------

state_script() { # sh (BusyBox's too): name=hex lines, the UEFI variables and the PMC's counters
  cat <<EOF
nv=$NV efi=$EFIG p=$PMC
EOF
  cat <<'EOF'
d=/sys/firmware/efi/efivars
[ -d $d ] && [ -z "$(ls $d 2>/dev/null)" ] && mount -t efivarfs efivarfs $d 2>/dev/null
hex() { if command -v od >/dev/null 2>&1; then od -An -tx1 -v "$1"; elif command -v xxd >/dev/null 2>&1; then xxd -p "$1"; else busybox od -An -tx1 -v "$1"; fi | tr -d ' \n'; }
v() { if [ -e "$d/$1-$2" ]; then echo "$1=$(hex "$d/$1-$2")"; else echo "$1=absent"; fi; }
for n in BootCurrent BootNext BootOrder; do v $n "$efi"; done
for n in L4TDefaultBootMode RootfsRedundancyLevel RootfsRetryCountMax RootfsStatusSlotA RootfsStatusSlotB \
  BootChainFwCurrent BootChainFwNext BootChainFwStatus; do v $n "$nv"; done
for a in boot_chain_current rootfs_current rootfs_retry_count_a rootfs_retry_count_b rootfs_sr_magic reset_reason; do
  x=$(cat "$p/$a" 2>/dev/null | tr -d ' \n'); echo "$a=${x:-?}"
done
EOF
}

read_state() { # <array name> [identity]: the boot state into that associative array; 1 if unreadable
  local -n st=$1
  local out k v
  out=$(on_box_root -t 60 "${2:-stock}" "$(state_script)" 2>/dev/null) || return 1
  st=()
  while IFS='=' read -r k v; do [[ -n $k ]] && st["$k"]=$v; done <<<"$out"
  [[ -n ${st[BootOrder]:-} ]]
}

entry_of() { # <hex of a UINT16 variable, its 4 attribute bytes first>: the entry, e.g. 0001
  local h=${1:8:4}
  [[ $1 == absent ]] && {
    echo absent
    return
  }
  printf '%s%s\n' "${h:2:2}" "${h:0:2}" | tr a-f A-F
}
u32_of() { # <hex of a UINT32 variable>: its value in hex
  [[ $1 == absent ]] && {
    echo absent
    return
  }
  local h=${1:8:8}
  printf '%s%s%s%s\n' "${h:6:2}" "${h:4:2}" "${h:2:2}" "${h:0:2}"
}

state_fields() { # the CSV's state fields from NOW
  R[boot_current]=$(entry_of "${NOW[BootCurrent]:-absent}")
  R[boot_next_after]=$(entry_of "${NOW[BootNext]:-absent}")
  R[reset_reason]=${NOW[reset_reason]:-}
  R[retry_a]=${NOW[rootfs_retry_count_a]:-}
  R[status_a]=$(u32_of "${NOW[RootfsStatusSlotA]:-absent}")
}

state_diff() { # [uefi]: what changed in CRIT (or only in its UEFI variables) between BASE and NOW
  local k d='' keys=("${CRIT[@]}")
  [[ ${1:-} == uefi ]] && keys=("${CRIT_UEFI[@]}")
  for k in "${keys[@]}"; do
    [[ ${BASE[$k]:-} == "${NOW[$k]:-}" ]] || d+="$k ${BASE[$k]:-?} -> ${NOW[$k]:-?}; "
  done
  printf '%s' "$d"
}

state_text() { # <array name>: the state as lines, for the details
  local -n st=$1
  local k
  for k in $(printf '%s\n' "${!st[@]}" | sort); do printf '%-22s %s\n' "$k" "${st[$k]}"; done
}

entries_read() { # E_NVME, E_PXE4, E_HTTP4 and E_FJARR from efibootmgr's labels on the stock system
  local out
  out=$(lab_box -t 30 'efibootmgr' 2>&1) || lab_die "efibootmgr failed on the box: $out"
  local h='[0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f]'
  E_NVME=$(awk "/^Boot$h\\*? UEFI PCIe SSD/ { print substr(\$1, 5, 4); exit }" <<<"$out")
  E_PXE4=$(awk "/^Boot$h\\*? UEFI PXEv4/ { print substr(\$1, 5, 4); exit }" <<<"$out")
  E_HTTP4=$(awk "/^Boot$h\\*? UEFI HTTPv4/ { print substr(\$1, 5, 4); exit }" <<<"$out")
  E_FJARR=$(awk -v l="$ENTRY_LABEL" "/^Boot$h\\*? / { s = \$0; sub(/^[^ ]+ /, \"\", s); sub(/\\t.*/, \"\", s); if (s == l) print substr(\$1, 5, 4) }" <<<"$out" | paste -sd' ' -)
}

# --- the stock system, before and after -------------------------------------------------------------

stock_preflight() { # the box on its stock system, started, verified, nothing pending: BASE, BASE_BOOT
  local ready
  lab_plug_check || lab_die "the plug does not answer (L2)"
  [[ $LAB_PLUG_OUTPUT == true ]] || lab_die "the plug is off"
  LAB_BOX_IP=''
  lab_find_box || lab_die "the box does not answer on its MAC (L2)"
  lab_usb_poll
  [[ $LAB_USB_STATE == l4t ]] || lab_die "the mini-PC sees '$LAB_USB_STATE' on the box's USB, not L4T ($LAB_L4T_USB) (L7)"
  lab_box_bootid || lab_die "the box does not answer ssh on its stock system (${LAB_SSH_ERR:-no error})"
  BASE_BOOT=$LAB_BOOTID
  ready=$(lab_wait_ready "$READY_TMO")
  [[ $ready == running || $ready == degraded ]] || lab_die "the box has not finished starting: '$ready'"
  BASE_READY=$ready
  verified_check || lab_die "the stock verifier (nv-l4t-bootloader-config) has not verified this boot: no reset now (L6)"
  read_state BASE || lab_die "could not read the boot state"
  [[ ${BASE[BootNext]} == absent ]] || lab_die "BootNext is set on the box (L5)"
  [[ ${BASE[BootChainFwNext]} == absent && ${BASE[BootChainFwStatus]} == absent ]] ||
    lab_die "BootChainFwNext or BootChainFwStatus is set: a chain switch may be pending (L5)"
  entries_read
  [[ -n $E_NVME ]] || lab_die "no 'UEFI PCIe SSD' entry among the box's boot entries"
  [[ $(entry_of "${BASE[BootCurrent]}") == "$E_NVME" ]] ||
    lab_die "the box booted from Boot$(entry_of "${BASE[BootCurrent]}"), not the NVMe entry Boot$E_NVME (L7)"
  lab_log "box at $LAB_BOX_IP, boot $BASE_BOOT, systemd '$ready', verified; BootCurrent $E_NVME; warm resets since a verify: $(warm_get)"
}

stock_after() { # [lenient]: the box back on its stock system: started, verified, BootNext gone, state kept
  # lenient (T3): the PMC's slot registers may change, which T3 measures; a UEFI variable may not.
  local ready d
  BASE_BOOT=$UP_BOOT
  ready=$(lab_wait_ready "$READY_TMO")
  R[to_ready_s]=$((SECONDS - T_CMD))
  [[ $ready == "$BASE_READY" ]] || stop not-started "the stock system is '$ready' after the boot, not '$BASE_READY' as before"
  verified_check || stop not-verified "the stock verifier did not verify this boot (L6)"
  read_state NOW || stop no-state "could not read the boot state after the boot"
  state_fields
  det "boot state after $TEST ${R[step]}" "$(state_text NOW)"
  [[ ${NOW[BootNext]} == absent ]] || stop bootnext-left "BootNext is still set after the boot: not consumed (L5)"
  BN_PENDING=0
  d=$(state_diff)
  if [[ -n $d ]]; then
    note "changed: $d"
    [[ ${1:-} == lenient ]] || stop state-changed "the boot state changed: $d"
    d=$(state_diff uefi)
    [[ -z $d ]] || stop state-changed "a UEFI variable of the boot state changed: $d"
  fi
}

stop() { # <result> <why>: stop the run (L7); on_die records the step and the state
  STOP_CODE=$1
  lab_die "$2 (L7)"
}

fire() { # <script> [args]: as root on the stock system, whose session dies with it: 0, 124, 255 expected
  local rc=0
  lab_box_root -t 20 "$@" >/dev/null 2>&1 || rc=$?
  case $rc in
    0 | 124 | 255) ;;
    *) stop refused "the box refused the reset (exit $rc)" ;;
  esac
}

boot_step() { # <entry or -> <how: reboot | panic N> [up timeout]: BootNext, the reset, the box down
  local entry=$1 how=$2
  lab_lock_check
  lab_desktop_guard
  warm_guard
  lab_need "$(boot_worst "${3:-$LAB_BACK_TMO}")" "a boot step"
  R[entry]=$entry
  if [[ $entry != - ]]; then
    BN_PENDING=1 BN_EVER=1 # from here on every stop removes it again if it is still set (bootnext_undo)
    lab_box_root -t 30 'efibootmgr -q -n "$1"' "$entry" >/dev/null || stop bootnext-refused "efibootmgr -n $entry failed"
    read_state NOW || stop no-state "could not read BootNext back"
    [[ $(entry_of "${NOW[BootNext]}") == "$entry" ]] || stop bootnext-wrong "BootNext reads '$(entry_of "${NOW[BootNext]}")', not $entry"
    lab_log "BootNext $entry set"
  fi
  lab_lock_check
  T_CMD=$SECONDS
  case $how in
    reboot)
      lab_log "box: systemctl reboot"
      fire 'systemctl reboot'
      ;;
    panic)
      lab_log "box: sysrq c with kernel.panic=$4"
      fire 'sysctl -q kernel.panic="$1" && sync && echo c >/proc/sysrq-trigger' "$4"
      ;;
  esac
  warm_add
  wait_down "$BASE_BOOT" "$DOWN_TMO" || stop not-down "the box still answers with the same boot id $DOWN_TMO s after the reset"
  # BN_PENDING stays set: an ssh probe that failed is not yet a new boot (wait_up clears it).
  R[to_down_s]=$((DOWN_AT - T_CMD))
}

back_to_stock() { # <cycle | rcm>: exactly one power cycle, out of RCM or out of a rescue; then the stock system
  STOP_CODE="$1-leave"
  if [[ $1 == rcm ]]; then lab_rcm_leave; else lab_power_cycle; fi
  STOP_CODE=''
  T_CMD=$LAB_CYCLE_ON_AT
  if ! wait_up "$LAB_BACK_TMO" "$BASE_BOOT" stock; then
    [[ $UP == nothing ]] || stop "back-$UP" "after the power cycle the box shows '$UP', not its stock system"
    stop not-back "nothing on the box's MAC within $LAB_BACK_TMO s of the power cycle: no second cycle, a person looks (L7)"
  fi
  lab_log "stock system back $((UP_AT - T_CMD)) s after power on"
}

up_failed() { # <code> <what was expected>: after wait_up failed in a step; a box showing nothing gets one cycle
  if [[ $UP == nothing ]]; then
    lab_log "nothing answers on the box's MAC: one power cycle back to the stock system, then stop (L7)"
    back_to_stock cycle
    stop "no-$1" "$2 did not come up, and the box returned to its stock system after one power cycle"
  fi
  [[ $UP == rec ]] && rec_stop "up-rec" "the box came up in the stock recovery, not $2"
  stop "up-$UP" "the box came up as '$UP', not $2"
}

rec_stop() { # <code> <why>: a stop in the stock recovery, which has no dead-man and no way to RCM (L7)
  # The state is recorded, BootNext removed if set (else the cycle would boot it again), APP synced,
  # then exactly one power cycle back to the stock system, and the stop.
  lab_log "$2: recording the state, then one power cycle out of the stock recovery (L7)"
  read_state NOW rec && det "stock recovery at the stop: boot state" "$(state_text NOW)"
  det_box "stock recovery at the stop: the safety script's log" rec 'cat /run/fjarr/log 2>&1'
  bootnext_undo || true
  on_box_as -t 20 rec sync >/dev/null 2>&1 || true
  note "$2"
  back_to_stock cycle
  stop "$1" "$2; one power cycle brought back the stock system"
}

bootnext_script() { # sh (BusyBox's too), as root: BootNext removed if set, then read back
  cat <<EOF
f=/sys/firmware/efi/efivars/BootNext-$EFIG
EOF
  cat <<'EOF'
d=${f%/*}
[ -d "$d" ] && [ -z "$(ls "$d" 2>/dev/null)" ] && mount -t efivarfs efivarfs "$d" 2>/dev/null
[ -e "$f" ] || { echo absent; exit 0; }
if command -v efibootmgr >/dev/null 2>&1; then efibootmgr -q -N >/dev/null 2>&1; fi
[ -e "$f" ] && { chattr -i "$f" 2>/dev/null; rm -f "$f"; }
if [ -e "$f" ]; then echo still-set; exit 1; fi
echo removed
EOF
}

bootnext_undo() { # after any stop (L5): a BootNext on the box is removed, on whatever answers on its MAC
  # Every test starts with BootNext absent and only b6.sh sets it, so once this run has set one, one
  # found now is this run's, whether or not the reboot happened: left set, the next boot by anyone
  # would take it. Before this run set any, a BootNext found is not this run's: recorded, not touched.
  local out
  ((BN_EVER)) || return 0
  if ! who_answers; then
    if ((BN_PENDING)); then
      lab_log "BootNext was set by this run and nothing answers on the box's MAC ('$WHO'): it may still be set; a person checks it with efibootmgr before the box boots again (L5)"
    fi
    return 0
  fi
  out=$(on_box_root -t 30 "$WHO" "$(bootnext_script)" 2>&1) || true
  case $out in
    *absent*) ((BN_PENDING)) && lab_log "BootNext: absent on the box ($WHO) (L5)" ;;
    *removed*) lab_log "BootNext was still set on the box ($WHO): removed, and reads absent now (L5)" ;;
    *) lab_log "BootNext MAY STILL BE SET on the box ($WHO: '${out//$'\n'/ }'): efibootmgr -N on it before anything else (L5)" ;;
  esac
  BN_PENDING=0
}

record_state() { # <why>: what the plug, the mini-PC and the box show (L7)
  lab_usb_poll
  lab_log "STATE ($1): the mini-PC's USB sees '$LAB_USB_STATE'"
  if lab_plug_status; then lab_log "STATE: plug output=$LAB_PLUG_OUTPUT, ${LAB_PLUG_APOWER} W"; else lab_log "STATE: the plug does not answer"; fi
  if who_answers; then
    lab_log "STATE: the box answers as '$WHO' at $LAB_BOX_IP"
    read_state NOW "$WHO" && det "state at the stop ($WHO)" "$(state_text NOW)"
    if [[ $WHO == stock ]]; then
      det_box "the safety script's log on APP, if any" stock "sudo -n cat $OTA_DIR/fjarr-safety.log 2>/dev/null || true"
      lab_log "STATE: run probe.sh next"
    elif [[ $WHO == rec ]]; then
      lab_log "STATE: the box is in the STOCK RECOVERY, which has no dead-man and no way to RCM: shelly.sh cycle brings back the stock system"
    fi
  else
    lab_log "STATE: the box answers as '$WHO'${LAB_BOX_IP:+ at $LAB_BOX_IP}"
  fi
  [[ $cmd == net-down ]] || ! net_dhcp_active 2>/dev/null || lab_log "STATE: the network rescue is still serving on the mini-PC: b6.sh net-down when this session ends"
}

on_die() {
  if ((IN_STEP)); then
    note "$1"
    row "${STOP_CODE:-stopped}"
  fi
  bootnext_undo || true
  record_state "$1" || true
  results_out || true
}
LAB_DIE_HOOK=on_die
on_signal() { # <exit code>
  trap '' INT TERM HUP
  SIGNALLED=1 LAB_DYING=1
  if ((IN_STEP)); then
    note interrupted
    row interrupted
    lab_log "interrupted in $TEST ${R[step]}: if the mini-PC's USB shows $LAB_RCM_USB (RCM), run shelly.sh cycle"
  fi
  bootnext_undo || true
  [[ $cmd == net-down ]] || ! net_dhcp_active 2>/dev/null || lab_log "the network rescue is still serving on the mini-PC: b6.sh net-down when this session ends"
  results_out || true
  exit "$1"
}
b6_exit() { # a failed command under set -e ends the run too: the same record and BootNext undo as a stop
  local rc=$?
  if ((rc != 0 && !SIGNALLED)) && [[ -z $LAB_DYING && ($LAB_LOCK_MODE == local || $LAB_LOCK_MODE == remote) ]]; then
    LAB_DYING=1
    lab_log "STOP: a command failed (exit $rc) (L7)"
    on_die "a command failed (exit $rc)" || true
  fi
  (exit "$rc")
  _lab_exit
}
trap 'on_signal 130' INT
trap 'on_signal 143' TERM
trap 'on_signal 129' HUP
trap b6_exit EXIT

# --- the network rescue on the mini-PC ------------------------------------------------------------

mpc_dir() {
  [[ -n $MPC_D ]] && return 0
  if [[ $B6_MPC_DIR == /* ]]; then MPC_D=$B6_MPC_DIR; else
    MPC_D=$(lab_mpc -t 20 'printf %s "$HOME"')/$B6_MPC_DIR || lab_die "the mini-PC does not answer"
  fi
}
mpc_put() { # <local file> <remote directory>
  if ((LAB_IS_MINIPC)); then cp "$1" "$2/"; else _lab_tmo "$B6_UPLOAD_TMO" scp -q "${LAB_SSH_MPC[@]}" "$1" "$LAB_MINIPC:$2/"; fi
}
efi_sha() { [[ -n $EFI_SHA ]] || EFI_SHA=$(sha256sum <"$B6_NET_EFI" | cut -d' ' -f1); }

box_mac_check() {
  local m
  m=$(cat "$LAB_DIR/rescue/box-mac" 2>/dev/null || true)
  [[ -z $m || ${m,,} == "${LAB_BOX_MAC,,}" ]] || lab_die ".lab/rescue/box-mac and LAB_BOX_MAC name different MACs"
}

net_status() { # 0 when the mini-PC serves this rescue image to the box's MAC; NET_STATUS its words
  local out
  mpc_dir
  efi_sha
  out=$(lab_mpc_root -t 60 '[ -x "$1" ] || { echo "no $1"; exit 3; }; "$1" --status 2>&1; s=$?
sed -n -e "s/^mac=/mac /p" -e "s/^sha256=/sha256 /p" /var/lib/fjarr-net-rescue/params 2>/dev/null; exit $s' \
    "$MPC_D/net-rescue.sh" 2>&1) || {
    NET_STATUS=$(grep -v -e '^mac ' -e '^sha256 ' <<<"$out" | tr '\n' ';')
    return 1
  }
  NET_STATUS=$(grep -v -e '^mac ' -e '^sha256 ' -e '^serves ' <<<"$out" | tr '\n' ';')
  grep -qx "sha256 $EFI_SHA" <<<"$out" || {
    NET_STATUS+=" serves another image than $B6_NET_EFI"
    return 1
  }
  grep -qix "mac ${LAB_BOX_MAC}" <<<"$out" || {
    NET_STATUS+=" serves another MAC than LAB_BOX_MAC"
    return 1
  }
}

net_dhcp_active() { lab_mpc -t 20 'systemctl is-active --quiet fjarr-net-rescue-dhcp.service'; }

net_up() {
  local rsha
  [[ -s $B6_NET_EFI && -s $B6_NET_KNOWN_HOSTS ]] || lab_die "no network rescue in out/rescue/36.4.4/: rescue/build.sh first"
  box_mac_check
  if net_status; then
    lab_log "the mini-PC serves the network rescue (this image, the box's MAC)"
    return 0
  fi
  lab_log "the network rescue is not serving yet: ${NET_STATUS:-no status}"
  lab_lock_check
  lab_need $((B6_UPLOAD_TMO + 900)) "the network rescue's upload and install"
  lab_mpc -t 20 "mkdir -p $(_lab_sq "$MPC_D")" || lab_die "cannot create $MPC_D on the mini-PC"
  mpc_put "$here/rescue/net-rescue.sh" "$MPC_D" || lab_die "upload of net-rescue.sh failed"
  mpc_put "$here/rescue/net-rescue-http.py" "$MPC_D" || lab_die "upload of net-rescue-http.py failed"
  rsha=$(lab_mpc -t 120 "sha256sum $(_lab_sq "$MPC_D/fjarr-net-rescue.efi") 2>/dev/null | cut -d' ' -f1" || true)
  if [[ $rsha != "$EFI_SHA" ]]; then
    lab_log "uploading the rescue image ($(($(stat -c %s "$B6_NET_EFI") / 1048576)) MiB) to the mini-PC"
    mpc_put "$B6_NET_EFI" "$MPC_D" || lab_die "upload of the rescue image failed"
    rsha=$(lab_mpc -t 120 "sha256sum $(_lab_sq "$MPC_D/fjarr-net-rescue.efi") | cut -d' ' -f1") || lab_die "could not hash the rescue image on the mini-PC"
    [[ $rsha == "$EFI_SHA" ]] || lab_die "the rescue image on the mini-PC does not match ($rsha)"
  fi
  lab_lock_check
  lab_log "mini-PC: net-rescue.sh (dnsmasq proxy DHCP and TFTP, HTTP; the box's MAC only)"
  lab_mpc_root -t 900 '"$1" --mac "$2" --efi "$3"' "$MPC_D/net-rescue.sh" "${LAB_BOX_MAC,,}" \
    "$MPC_D/fjarr-net-rescue.efi" >"$LAB_RUN/net-up.out" 2>&1 || {
    sed 's/^/  net-rescue: /' "$LAB_RUN/net-up.out" >&2
    lab_die "net-rescue.sh failed on the mini-PC"
  }
  net_status || lab_die "the network rescue does not serve after net-rescue.sh: $NET_STATUS"
  lab_log "the mini-PC serves the network rescue: $NET_STATUS"
}

net_need() { # L2, from T4 on: the network rescue serving
  net_status || lab_die "the network rescue is not serving (${NET_STATUS:-no status}): b6.sh net-up first (L2)"
}

# --- the stock recovery: the safety script and its entry (T4) --------------------------------------

safety_script() {
  cat <<'EOF'
#!/bin/bash
# nv_ota_run_tasks.sh: the lab's safety script for the ZED Box Mini's stock recovery. Placed at
# APP:/ota_work/ by spikes/ota-jetson/b6.sh t4 (slice 8.0, phase B6), removed by b6.sh clean.
# spec: docs/32-fleet-ota.md#jetson-test-image ("Three rescue layers"; phase B, T4)
#
# NVIDIA's recovery ramdisk runs it (nv_recovery.sh, PID 1) with APP mounted at /mnt and
# /mnt/ota_work as its working directory, whenever L4TLauncher boots the recovery partition: through
# the lab's bootmode=recovery entry, or with the root slot marked unbootable. It sets the two
# variables that send the box to recovery back to the values the stock box has (RootfsStatusSlotA
# 0, normal; L4TDefaultBootMode 1, normal), so that the next boot runs the stock system, and
# writes each even when it already holds that value, so that every run proves the write path
# through efivarfs ("write ok" in the log; T4 requires it, since T3 relies on it); starts
# DHCP and sshd (root, the lab's keys, its own host key); remounts APP read-only, so that the power
# cycle that ends it cannot hurt APP; and never returns, since the end of PID 1 would panic the
# kernel. Its log: /run/fjarr/log, and ota_work/fjarr-safety.log on APP until the remount.
# nv_recovery.sh hands it a PATH without /sbin, where the ramdisk keeps dhclient (B6 T4 on
# 2026-10-18: "timeout: failed to run command 'dhclient'", the port up and nothing asked for DHCP).
export PATH=/sbin:/usr/sbin:/bin:/usr/bin
w=/mnt/ota_work r=/run/fjarr log=/mnt/ota_work/fjarr-safety.log
nv=781e084c-a330-417c-b678-38e696380cb9 efi=8be4df61-93ca-11d2-aa0d-00e098032b8c
vars=/sys/firmware/efi/efivars
ro=0
mkdir -p "$r" /run/sshd /root
chmod 0700 "$r"
chmod 0755 /run/sshd
say() {
  local l
  l="$(cut -d' ' -f1 /proc/uptime) $*"
  printf '%s\n' "$l" >>"$r/log"
  [ "$ro" = 1 ] || printf '%s\n' "$l" >>"$log"
  printf 'fjarr-safety: %s\n' "$*" 2>/dev/null >/dev/kmsg
}
say "start; /proc/cmdline: $(cat /proc/cmdline)"
grep -q " $vars efivarfs " /proc/mounts || mount -t efivarfs efivarfs "$vars" || say "efivarfs: the mount failed"
for v in BootCurrent BootNext BootOrder; do
  if [ -e "$vars/$v-$efi" ]; then say "$v $(xxd -p "$vars/$v-$efi" | tr -d '\n')"; else say "$v absent"; fi
done
reset() { # <name> <the 8 bytes, the attributes then the value, as printf escapes>
  local f=$vars/$1-$nv want have now w=FAILED
  # shellcheck disable=SC2059 # the bytes are the format
  want=$(printf "$2" | xxd -p | tr -d '\n')
  [ -e "$f" ] || { say "$1 absent: left absent"; return; }
  have=$(xxd -p "$f" | tr -d '\n')
  # shellcheck disable=SC2059
  printf "$2" >"$r/value"
  chattr -i "$f" 2>/dev/null
  # One write of the attributes and the value: efivarfs hands it to SetVariable whole. The same
  # value again changes nothing in the store, and still goes through the firmware's checks.
  dd if="$r/value" of="$f" bs=8 count=1 conv=notrunc 2>/dev/null && w=ok
  now=$(xxd -p "$f" | tr -d '\n')
  [ "$now" = "$want" ] || w="$w, reads $now"
  if [ "$have" = "$want" ]; then say "$1 $have: the same value written again: write $w"; else say "$1 $have -> $now: write $w"; fi
}
reset RootfsStatusSlotA '\x07\x00\x00\x00\x00\x00\x00\x00'
reset L4TDefaultBootMode '\x07\x00\x00\x00\x01\x00\x00\x00'
sync
# The ramdisk's init loads r8168 before any PCIe controller is up; the Ethernet port appears only
# once nv_recovery.sh has loaded pcie_tegra194 for the NVMe, and the kernel's own tunnel and dummy
# interfaces exist from the start. So wait for interfaces with a device behind them, not for any.
modprobe r8168 2>/dev/null || say "modprobe r8168 failed"
nics() { for i in /sys/class/net/*; do [ -e "$i/device" ] && printf '%s ' "${i##*/}"; done; }
for _ in $(seq 1 60); do
  [ -n "$(nics)" ] && break
  sleep 1
done
all=$(for i in /sys/class/net/*; do printf '%s ' "${i##*/}"; done)
say "interfaces: $all; with a device: $(nics)"
for n in $(nics); do
  d=$(cd "/sys/class/net/$n/device/driver" 2>/dev/null && pwd -P) # the ramdisk has no readlink
  say "$n: driver ${d##*/}, mac $(cat "/sys/class/net/$n/address")"
  ip link set "$n" up
  (timeout 120 dhclient -1 -v -pf "/run/dhclient.$n.pid" -lf "/run/dhclient.$n.leases" "$n" >>"$r/dhclient.log" 2>&1 &)
done
cp "$w/ssh_host_ed25519_key" "$w/authorized_keys" "$w/sshd_config" "$r/" && chmod 0600 "$r"/*
if /bin/sshd -f "$r/sshd_config" -E "$r/sshd.log"; then say "sshd started"; else say "sshd did not start"; fi
# Keep APP writable until there is an address or 90 s have passed, so that the log on APP shows how
# the network came up (or did not) after the power cycle that ends this boot clears /run.
for _ in $(seq 1 18); do
  a=$(ip -4 -o addr show scope global | awk '{ print $2, $4 }' | tr '\n' ' ')
  [ -n "$a" ] && break
  sleep 5
done
say "addresses: ${a:-none}; sshd $(if [ -s "$r/sshd.pid" ] && kill -0 "$(cat "$r/sshd.pid")" 2>/dev/null; then echo alive; else echo gone; fi)"
{
  echo "--- ip addr"; ip addr 2>&1
  echo "--- dhclient.log"; tail -n 40 "$r/dhclient.log" 2>&1
  echo "--- sshd.log"; tail -n 20 "$r/sshd.log" 2>&1
  echo "--- dmesg (net)"; dmesg 2>&1 | grep -iE 'r8168|eth|pcie|link' | tail -n 40
} >>"$log" 2>&1
sync
if mount -o remount,ro /mnt; then say "APP read-only from here"; ro=1; else say "APP: the read-only remount failed"; fi
while :; do sleep 3600; done
EOF
}

sshd_config_rec() {
  cat <<'EOF'
# The stock recovery's sshd for the lab (b6.sh t4): root with the lab's keys only.
Port 22
HostKey /run/fjarr/ssh_host_ed25519_key
AuthorizedKeysFile /run/fjarr/authorized_keys
PermitRootLogin prohibit-password
PubkeyAuthentication yes
PasswordAuthentication no
KbdInteractiveAuthentication no
UsePAM no
StrictModes no
PidFile /run/fjarr/sshd.pid
Subsystem sftp internal-sftp
EOF
}

rec_keys() { # the stock recovery's host key for the lab, made once (.lab/b6), and its known_hosts line
  local k=$B6_DIR/stock-recovery-hostkey
  [[ -s $k ]] || ssh-keygen -q -t ed25519 -N '' -C "$REC_ALIAS" -f "$k" || lab_die "ssh-keygen failed"
  printf '%s %s\n' "$REC_ALIAS" "$(cut -d' ' -f1,2 "$k.pub")" >"$B6_DIR/known_hosts"
}

pubkeys() { # the keys the stock recovery's sshd takes
  if [[ -n $B6_PUBKEYS ]]; then cat "$B6_PUBKEYS"; else cat "$HOME"/.ssh/id_*.pub; fi 2>/dev/null
}

ota_stage() { # the files for APP:/ota_work into a directory
  local d=$1
  mkdir -p "$d"
  chmod 0700 "$d" # tar gives /ota_work this mode
  safety_script >"$d/nv_ota_run_tasks.sh"
  sshd_config_rec >"$d/sshd_config"
  pubkeys >"$d/authorized_keys"
  cp "$B6_DIR/stock-recovery-hostkey" "$d/ssh_host_ed25519_key"
  chmod 0755 "$d/nv_ota_run_tasks.sh"
  chmod 0600 "$d/sshd_config" "$d/authorized_keys" "$d/ssh_host_ed25519_key"
  (cd "$d" && sha256sum -- * | sort -k2)
}

esp_check() { # ESP_DISK, ESP_PART, ESP_FILE (\EFI\BOOT\BOOTAA64.efi as the ESP spells it), read only
  local out
  out=$(lab_box_root -t 90 '
d=$(readlink -f /dev/disk/by-partlabel/esp) || exit 3
n=${d##*/}
echo "disk=/dev/$(basename "$(readlink -f "/sys/class/block/$n/..")")"
echo "part=$(cat "/sys/class/block/$n/partition")"
echo "type=$(blkid -p -o value -s PART_ENTRY_TYPE "$d")"
fsck.vfat -n -l "$d" 2>/dev/null | sed -n "s|^Checking file \(/EFI/BOOT/BOOTAA64\.[Ee][Ff][Ii]\).*|file=\1|Ip" | head -n 1') ||
    lab_die "could not read the ESP"
  ESP_DISK=$(sed -n 's/^disk=//p' <<<"$out")
  ESP_PART=$(sed -n 's/^part=//p' <<<"$out")
  ESP_FILE=$(sed -n 's/^file=//p' <<<"$out")
  [[ $(sed -n 's/^type=//p' <<<"$out") == c12a7328-f81f-11d2-ba4b-00a0c93ec93b ]] || lab_die "the esp partition is not of the EFI system type"
  [[ $ESP_DISK == /dev/nvme0n1 && $ESP_PART =~ ^[0-9]+$ ]] || lab_die "the esp partition is '$ESP_DISK' '$ESP_PART', not on the NVMe"
  [[ -n $ESP_FILE ]] || lab_die "no /EFI/BOOT/BOOTAA64.efi in the ESP (fsck.vfat -n -l)"
  ESP_FILE=${ESP_FILE//\//\\}
  lab_log "ESP: $ESP_DISK partition $ESP_PART, L4TLauncher at $ESP_FILE"
}

ota_check() { # nothing on the stock system runs /ota_work at boot; OTA_STATE: absent | ours | other
  local out
  out=$(lab_box_root -t 60 '
if [ -e /ota_work ]; then
  if grep -qs "b6.sh t4" /ota_work/nv_ota_run_tasks.sh; then echo state=ours; else echo state=other; fi
else echo state=absent; fi
grep -rlsI ota_work /etc/systemd /lib/systemd/system /usr/lib/systemd/system 2>/dev/null | sed "s/^/unit=/" | head -n 20
grep -rlsI ota_work /opt/nvidia /etc/nv* 2>/dev/null | sed "s/^/ref=/" | head -n 20') || lab_die "could not look for ota_work on the box"
  OTA_STATE=$(sed -n 's/^state=//p' <<<"$out")
  det "references to ota_work on the stock system" "$out"
  ! grep -q '^unit=' <<<"$out" || lab_die "a systemd unit on the box names ota_work: $(sed -n 's/^unit=//p' <<<"$out" | paste -sd' ' -) (L7)"
  [[ $OTA_STATE != other ]] || lab_die "/ota_work on the box holds something that is not this script's: not touched (L7)"
}

ucs2hex() { # <text>: its UCS-2 bytes in hex, as UEFI stores a CHAR16 string
  local s=$1 i out=''
  for ((i = 0; i < ${#s}; i++)); do out+=$(printf '%02x00' "'${s:i:1}"); done
  printf '%s\n' "$out"
}

entry_check() { # <entry>: it starts L4TLauncher from the ESP with bootmode=recovery
  local hex want_f want_o
  hex=$(lab_box_root -t 20 'od -An -tx1 -v "/sys/firmware/efi/efivars/Boot$1-$2" | tr -d " \n"' "$1" "$EFIG") || return 1
  want_f=$(ucs2hex "${ESP_FILE:-\\EFI\\BOOT\\BOOTAA64.efi}")
  want_o=$(ucs2hex "$ENTRY_OPTS")
  [[ ${hex,,} == *"${want_f,,}"* && ${hex,,} == *"$want_o"* ]]
}

# --- one test each --------------------------------------------------------------------------------

t2() {
  local t_nvme
  stock_preflight
  collect_secrets
  lab_backup_check || lab_die "no verified backup: backup.sh first (L2)"
  [[ -n $E_PXE4 ]] || lab_die "no 'UEFI PXEv4' entry among the box's boot entries"
  ! net_dhcp_active || lab_die "the network rescue is serving: T2's fall-through needs no server (b6.sh net-down)"
  det_box "boot entries before T2" stock 'efibootmgr -v'
  det "boot state before T2" "$(state_text BASE)"

  step_begin bootnext-nvme
  boot_step "$E_NVME" reboot
  wait_up "$LAB_BACK_TMO" "$BASE_BOOT" stock || up_failed stock "the stock system"
  R[came_up_as]=$UP R[to_up_s]=$((UP_AT - T_CMD))
  stock_after
  [[ ${R[boot_current]} == "$E_NVME" ]] || stop wrong-entry "BootCurrent is ${R[boot_current]}, not $E_NVME"
  t_nvme=${R[to_up_s]}
  note "consumed"
  row ok

  step_begin bootnext-pxe-no-server
  boot_step "$E_PXE4" reboot "$B6_FALL_TMO"
  wait_up "$B6_FALL_TMO" "$BASE_BOOT" stock || up_failed fall-through "the fall-through to the NVMe"
  R[came_up_as]=$UP R[to_up_s]=$((UP_AT - T_CMD))
  stock_after
  [[ ${R[boot_current]} == "$E_NVME" ]] || stop wrong-entry "BootCurrent is ${R[boot_current]}, not $E_NVME"
  local up=${R[to_up_s]} extra
  extra=$((up - t_nvme))
  note "fall-through to the NVMe; $extra s longer than the boot through Boot$E_NVME"
  if ((extra >= 15)); then
    note "(BootNext honoured)"
    row ok
    marker_set pass "BootNext honoured and consumed; fall-through +$extra s"
  else
    note "(too short to show that BootNext was honoured)"
    row consumed
    marker_set pass "BootNext consumed; honoured not shown (fall-through +$extra s); T5 shows it"
  fi
}

net_rescue_checks() { # in the network rescue: ssh, the NVMe read only, the NIC; notes and details
  local gpt nic out
  on_box_as -t 20 net "touch $LAB_DEADMAN_FILE" || stop no-touch "could not touch the dead-man's file in the rescue"
  T_TOUCH=$SECONDS
  det_box "rescue ($1): kernel, command line, uptime" net 'uname -a; cat /proc/cmdline; cat /proc/uptime'
  det_box "rescue ($1): boot entries" net 'efibootmgr -v'
  det_box "rescue ($1): kernel log (PCIe, NVMe, Ethernet, EFI, watchdog)" net \
    'dmesg | grep -iE "nvme|r8168|pcie|tegra194|efi|watchdog|wdt" | head -n 80'
  det_box "rescue ($1): disks" net 'lsblk -o NAME,SIZE,TYPE,RO,PARTLABEL 2>&1 || cat /proc/partitions'
  read_state NOW net || stop no-state "could not read the boot state in the rescue"
  det "rescue ($1): boot state" "$(state_text NOW)"
  state_fields
  # The NVMe, read only: the GPT header as the stock system read it, and APP mounted ro,noload.
  out=$(on_box_root -t 90 net '
echo "gpt=$(dd if=/dev/nvme0n1 bs=512 skip=1 count=1 2>/dev/null | sha256sum | cut -d" " -f1)"
a=$(blkid -t PARTLABEL=APP -o device 2>/dev/null | head -n 1); echo "app=$a"
if [ -n "$a" ] && mkdir -p /tmp/app && mount -o ro,noload "$a" /tmp/app; then
  echo "release=$(head -n 1 /tmp/app/etc/nv_tegra_release 2>/dev/null)"; umount /tmp/app; fi
for i in /sys/class/net/*; do n=${i##*/}; [ "$n" = lo ] && continue
  echo "nic=$n $(basename "$(readlink "$i/device/driver" 2>/dev/null)") $(cat "$i/operstate")"; done
ip -4 -o addr show scope global | awk "{ print \"addr=\" \$2 \" \" \$4 }"' 2>&1) || true
  det "rescue ($1): the NVMe read only, the NIC" "$out"
  gpt=$(sed -n 's/^gpt=//p' <<<"$out")
  nic=$(sed -n 's/^nic=//p' <<<"$out" | grep -E ' r8168 up$' | head -n 1 || true)
  if [[ -n $gpt && $gpt == "$GPT_SHA" ]]; then note "nvme=seen"; else note "nvme=NOT-seen"; fi
  grep -q '^release=# R36' <<<"$out" && note "app=mounted-ro"
  if [[ -n $nic ]]; then note "nic=${nic%% *}/r8168"; else note "nic=NOT-up"; fi
  [[ ${R[boot_current]} == "$2" ]] || note "BootCurrent=${R[boot_current]}-not-$2"
  [[ -n $gpt && $gpt == "$GPT_SHA" && -n $nic && ${R[boot_current]} == "$2" ]]
}

rcm_from() { # <identity>: reboot-recovery there; RCM on the mini-PC
  local rc=0 t0
  lab_lock_check
  lab_desktop_guard
  lab_need "$LAB_RCM_ROUND_WORST" "a round trip through RCM"
  t0=$SECONDS
  lab_log "rescue: reboot-recovery"
  on_box_as -t 20 "$1" 'sync; reboot-recovery' >/dev/null 2>&1 || rc=$?
  case $rc in 0 | 124 | 255) ;; *) stop rcm-refused "reboot-recovery failed in the rescue (exit $rc)" ;; esac
  lab_rcm_wait "$LAB_RCM_TMO" || stop no-rcm "no RCM ($LAB_RCM_USB) within $LAB_RCM_TMO s of reboot-recovery; the mini-PC sees '$LAB_USB_STATE'"
  note "rcm=$((SECONDS - t0))s"
}

deadman_wait() { # no touch: RCM by the rescue's dead-man within B6_DEADMAN_S of the last touch
  lab_need "$((B6_DEADMAN_S + LAB_RCM_ROUND_WORST))" "the dead-man's wait"
  lab_log "waiting for the dead-man: RCM within $B6_DEADMAN_S s of the last touch, nothing touched"
  while ((SECONDS - T_TOUCH < B6_DEADMAN_S)); do
    lab_usb_poll
    case $LAB_USB_STATE in
      rcm)
        note "deadman-rcm=$((SECONDS - T_TOUCH))s-after-touch"
        return 0
        ;;
      other:*) stop usb-other "the mini-PC sees '$LAB_USB_STATE' on the box's USB" ;;
    esac
    sleep 10
  done
  stop no-deadman "no RCM within $B6_DEADMAN_S s of the last touch: the dead-man did not fire"
}

t5() {
  local way entry passed=() since ways=(pxe http)
  [[ $opt == --http ]] && ways=(http) # after a stop in the PXE step: no blind retry of it (L7)
  marker_need t2
  stock_preflight
  collect_secrets
  lab_backup_check || lab_die "no verified backup: backup.sh first (L2)"
  [[ -n $E_PXE4 && -n $E_HTTP4 ]] || lab_die "no 'UEFI PXEv4' or 'UEFI HTTPv4' entry among the box's boot entries"
  net_up
  GPT_SHA=$(lab_box_root -t 30 'dd if=/dev/nvme0n1 bs=512 skip=1 count=1 2>/dev/null | sha256sum | cut -d" " -f1') ||
    lab_die "could not read the GPT header on the box"
  for way in "${ways[@]}"; do
    if [[ $way == pxe ]]; then entry=$E_PXE4; else entry=$E_HTTP4; fi
    step_begin "net-$way"
    since=$(date +%s)
    boot_step "$entry" reboot "$B6_NET_TMO"
    wait_up "$B6_NET_TMO" "$BASE_BOOT" net stock || up_failed net-rescue "the network rescue"
    R[came_up_as]=$UP R[to_up_s]=$((UP_AT - T_CMD))
    if [[ $UP == stock ]]; then
      note "fell through to the NVMe: the rescue was not reached"
      stock_after
      det "the mini-PC's journal ($way)" "$(lab_mpc_root -t 60 'journalctl -o short-iso --no-pager --since "@$1" -u fjarr-net-rescue-dhcp -u fjarr-net-rescue-http' "$since" 2>&1 | tail -n 80)"
      row fell-through
      continue
    fi
    lab_log "the network rescue answers by $way, $((UP_AT - T_CMD)) s after the reboot"
    net_rescue_checks "$way" "$entry" || lab_log "the rescue by $way: something is missing (${R[note]:-})"
    det "the mini-PC's journal ($way)" "$(lab_mpc_root -t 60 'journalctl -o short-iso --no-pager --since "@$1" -u fjarr-net-rescue-dhcp -u fjarr-net-rescue-http' "$since" 2>&1 | tail -n 80)"
    if [[ $opt == --deadman && $way == http ]]; then deadman_wait; else rcm_from net; fi
    back_to_stock rcm
    stock_after
    if [[ ${R[note]:-} == *NOT-* || ${R[note]:-} == *-not-* ]]; then row partial; else
      row ok
      passed+=("$way")
    fi
  done
  if ((${#passed[@]})); then
    marker_set pass "network rescue by ${passed[*]}"
    lab_log "T5 passed by ${passed[*]}; the network rescue stays up for T4, T6 and T3 (b6.sh net-down at the end)"
  else
    lab_die "the network rescue came up by neither PXE nor HTTP: every later write needs Q4 (L7)"
  fi
}

t4() {
  local stage=$LAB_RUN/ota_work sums onbox reclog
  marker_need t5
  stock_preflight
  collect_secrets
  lab_backup_check || lab_die "no verified backup: backup.sh first (L2)"
  net_need
  esp_check
  ota_check
  [[ -z $E_FJARR ]] || lab_die "a '$ENTRY_LABEL' entry exists already (Boot$E_FJARR): b6.sh clean first"
  [[ $OTA_STATE == absent ]] || lab_die "/ota_work exists already: b6.sh clean first"
  rec_parts_check || lab_die "recovery or recovery-dtb on the box differs from the backup: not NVIDIA's stock recovery (L7)"
  lab_log "recovery and recovery-dtb match the backup: NVIDIA's stock recovery"
  rec_keys
  [[ -n $(pubkeys) ]] || lab_die "no public key for the stock recovery's sshd (~/.ssh/id_*.pub or B6_PUBKEYS)"
  sums=$(ota_stage "$stage")
  [[ -n $sums && -s $stage/authorized_keys ]] || lab_die "could not stage the files for $OTA_DIR"
  det "boot entries before T4" "$(lab_box -t 30 'efibootmgr -v' 2>&1)"

  step_begin install
  lab_lock_check
  tar -C "$stage" --owner=0 --group=0 --numeric-owner -cf - . |
    box_root_stdin 60 'install -d -m 0700 -o root -g root "$1" && tar -C "$1" --no-same-owner -xf - && sync' "$OTA_DIR" ||
    stop install-failed "could not write $OTA_DIR on the box"
  onbox=$(lab_box_root -t 30 'cd "$1" && sha256sum -- * | sort -k2' "$OTA_DIR") || stop install-failed "could not read $OTA_DIR back"
  [[ $onbox == "$sums" ]] || stop install-mismatch "$OTA_DIR on the box does not match what was written"
  lab_log "safety script at APP:$OTA_DIR/nv_ota_run_tasks.sh"
  lab_box_root -t 30 'efibootmgr -q -C -d "$1" -p "$2" -L "$3" -l "$4" -u "$5"' \
    "$ESP_DISK" "$ESP_PART" "$ENTRY_LABEL" "$ESP_FILE" "$ENTRY_OPTS" >/dev/null || stop entry-failed "efibootmgr -C failed"
  entries_read
  [[ $E_FJARR =~ ^[0-9A-F]{4}$ ]] || stop entry-missing "the new entry '$ENTRY_LABEL' is not listed (or listed twice: '$E_FJARR')"
  printf '%s\n' "$E_FJARR" >"$B6_DIR/entry"
  entry_check "$E_FJARR" || stop entry-wrong "Boot$E_FJARR does not hold $ESP_FILE with $ENTRY_OPTS"
  read_state NOW || stop no-state "could not read the boot state"
  [[ ${NOW[BootOrder]} == "${BASE[BootOrder]}" && ${NOW[BootNext]} == absent ]] ||
    stop bootorder-changed "the new entry changed BootOrder or BootNext (efibootmgr -C must not)"
  det "boot entries after the install" "$(lab_box -t 30 'efibootmgr -v' 2>&1)"
  R[entry]=$E_FJARR
  note "entry Boot$E_FJARR: HD($ESP_PART) $ESP_FILE $ENTRY_OPTS; not in BootOrder"
  row ok

  step_begin stock-recovery
  boot_step "$E_FJARR" reboot "$B6_REC_TMO"
  wait_up "$B6_REC_TMO" "$BASE_BOOT" rec || up_failed stock-recovery "the stock recovery with the safety script"
  R[came_up_as]=$UP R[to_up_s]=$((UP_AT - T_CMD))
  lab_log "the stock recovery answers ssh $((UP_AT - T_CMD)) s after the reboot"
  reclog=$(on_box_as -t 30 rec 'cat /run/fjarr/log' 2>&1) || reclog+=$'\n[exit '$?']'
  det "stock recovery: the safety script's log" "$reclog"
  det_box "stock recovery: DHCP" rec 'tail -n 20 /run/fjarr/dhclient.log 2>/dev/null'
  det_box "stock recovery: kernel, command line, mounts" rec 'uname -a; cat /proc/cmdline; cat /proc/uptime; grep -E " /mnt | efivarfs " /proc/mounts'
  read_state NOW rec || rec_stop no-state "could not read the boot state in the stock recovery"
  det "stock recovery: boot state" "$(state_text NOW)"
  state_fields
  [[ ${R[boot_current]} == "$E_FJARR" ]] || note "BootCurrent=${R[boot_current]}-not-$E_FJARR"
  [[ ${NOW[BootNext]} == absent ]] || rec_stop bootnext-left "BootNext is still set in the stock recovery (L5)"
  [[ ${NOW[BootOrder]} == "${BASE[BootOrder]}" ]] || rec_stop bootorder-changed "the firmware changed BootOrder (${NOW[BootOrder]})"
  # T3's safety net: the safety script must have written both variables through efivarfs.
  { grep -qE ' RootfsStatusSlotA .*: write ok$' <<<"$reclog" && grep -qE ' L4TDefaultBootMode .*: write ok$' <<<"$reclog"; } ||
    rec_stop write-unproven "the safety script's log shows no 'write ok' for RootfsStatusSlotA and L4TDefaultBootMode: T3's safety net is unproven"
  note "efivarfs write ok"
  on_box_as -t 20 rec sync || true
  back_to_stock cycle
  stock_after
  det_box "the safety script's log on APP" stock "sudo -n cat $OTA_DIR/fjarr-safety.log"
  entries_read
  [[ $E_FJARR == "$(cat "$B6_DIR/entry")" ]] || stop entry-gone "the firmware removed or renumbered Boot$(cat "$B6_DIR/entry") (now: '${E_FJARR:-none}')"
  note "safety script ran; the entry and the script stay for T6 and T3 (b6.sh clean)"
  row ok
  marker_set pass "stock recovery through Boot$E_FJARR, the safety script reached on ssh, efivarfs write ok"
}

a5_find() { # A5_IMG and A5_DTB, the A5 rescue's image and device tree; 1 when there is none
  A5_IMG=$B6_A5_IMG A5_DTB=$B6_A5_DTB
  [[ -n $A5_IMG ]] || A5_IMG=$(find "$B6_A5_DIR" -maxdepth 1 \( -type f -o -type l \) \( -name '*recovery*.img' -o -name '*rescue*.img' \) 2>/dev/null | sort | tail -n 1)
  [[ -n $A5_DTB ]] || A5_DTB=$(find "$B6_A5_DIR" -maxdepth 1 \( -type f -o -type l \) \( -name '*recovery*.dtb' -o -name '*rescue*.dtb' \) 2>/dev/null | sort | tail -n 1)
  [[ -s $A5_IMG && -s $A5_DTB ]]
}

bk_sha() { zstd -dcq "$LAB_DIR/zedbox-backup/$1" | sha256sum | cut -d' ' -f1; } # <file>: of the content

part_sha() { # <partlabel>: sha256 of the partition on the box
  lab_box_root -t 120 'sha256sum "/dev/disk/by-partlabel/$1" | cut -d" " -f1' "$1"
}

rec_parts_check() { # recovery and recovery-dtb on the box hash as the backup's copies (BK8, BK9)
  BK8=$(bk_sha nvme0n1p8-recovery.img.zst) || lab_die "could not read the backup's recovery"
  BK9=$(bk_sha nvme0n1p9-recovery-dtb.img.zst) || lab_die "could not read the backup's recovery-dtb"
  [[ $BK8 =~ ^[0-9a-f]{64}$ && $BK9 =~ ^[0-9a-f]{64}$ ]] || lab_die "the backup's recovery or recovery-dtb hash is unreadable"
  [[ $(part_sha recovery) == "$BK8" && $(part_sha recovery-dtb) == "$BK9" ]]
}

t6_restore_parts() { # recovery and recovery-dtb as the backup has them; checked by sha256 (BK8, BK9)
  local p f bk now
  for p in recovery recovery-dtb; do
    if [[ $p == recovery ]]; then f=nvme0n1p8-recovery.img.zst bk=$BK8; else f=nvme0n1p9-recovery-dtb.img.zst bk=$BK9; fi
    lab_lock_check
    if [[ $(lab_box_root -t 120 '[ -f "$1" ] && sha256sum "$1" | cut -d" " -f1' "$ONBOX/$p.img" 2>/dev/null) != "$bk" ]]; then
      lab_log "uploading $p from the backup"
      lab_need "$B6_UPLOAD_TMO" "the upload of $p"
      zstd -dcq "$LAB_DIR/zedbox-backup/$f" | box_root_stdin "$B6_UPLOAD_TMO" 'mkdir -p "$(dirname "$1")" && cat >"$1" && sync' "$ONBOX/$p.img" ||
        stop restore-upload "the upload of $p failed"
    fi
    lab_box_root -t 300 '[ "$(sha256sum "$1" | cut -d" " -f1)" = "$3" ] || exit 4
dd if="$1" of="/dev/disk/by-partlabel/$2" bs=1M conv=fsync status=none && sync' "$ONBOX/$p.img" "$p" "$bk" ||
      stop restore-failed "writing $p back failed"
    now=$(part_sha "$p") || stop restore-read "could not read $p back"
    [[ $now == "$bk" ]] || stop restore-mismatch "$p does not match the backup after the restore ($now)"
    note "$p=backup"
  done
  lab_box_root -t 60 'rm -f "$1"/recovery.img "$1"/recovery-dtb.img "$1"/a5.img "$1"/a5.dtb "$1"/*.new; rmdir "$1" 2>/dev/null; sync' "$ONBOX" || true
  rm -f "$B6_DIR/t6-written"
}

t6() {
  local p8size p9size isha dsha
  if [[ $opt == --restore ]]; then
    lab_backup_check || lab_die "no verified backup (L2)"
    stock_preflight
    collect_secrets
    net_need
    BK8=$(bk_sha nvme0n1p8-recovery.img.zst) || lab_die "could not read the backup's recovery"
    BK9=$(bk_sha nvme0n1p9-recovery-dtb.img.zst) || lab_die "could not read the backup's recovery-dtb"
    step_begin restore
    t6_restore_parts
    row ok
    return 0
  fi
  if ! a5_find; then
    step_begin skip
    note "no A5 rescue image (an Android boot image *recovery*.img and its *.dtb) in ${B6_A5_DIR#"$LAB_HOME"/}; B6_A5_IMG and B6_A5_DTB name one"
    row skipped
    marker_set skip "no A5 rescue image built"
    lab_log "T6 skipped: the recovery-partition rescue (A5) is not built; T3 may follow T4"
    return 0
  fi
  marker_need t4
  [[ -s $B6_A5_HOSTKEY_PUB ]] || lab_die "no host key for the A5 rescue ($B6_A5_HOSTKEY_PUB, B6_A5_HOSTKEY_PUB)"
  printf '%s %s\n' "$A5_ALIAS" "$(cut -d' ' -f1,2 "$B6_A5_HOSTKEY_PUB")" >"$B6_DIR/a5-known_hosts"
  [[ $(head -c 8 "$A5_IMG") == ANDROID! ]] || lab_die "$A5_IMG is not an Android boot image"
  [[ $(od -An -tx1 -N4 "$A5_DTB" | tr -d ' \n') == d00dfeed ]] || lab_die "$A5_DTB is not a device tree blob"
  lab_backup_check || lab_die "no verified backup (L2)"
  stock_preflight
  collect_secrets
  net_need
  [[ -s $B6_DIR/entry && $E_FJARR == "$(cat "$B6_DIR/entry")" ]] || lab_die "T4's entry is not on the box as T4 left it ('$E_FJARR')"
  esp_check
  entry_check "$E_FJARR" || lab_die "Boot$E_FJARR is not T4's entry any more"
  p8size=$(lab_box_root -t 20 'blockdev --getsize64 /dev/disk/by-partlabel/recovery') || lab_die "could not read the size of recovery"
  p9size=$(lab_box_root -t 20 'blockdev --getsize64 /dev/disk/by-partlabel/recovery-dtb') || lab_die "could not read the size of recovery-dtb"
  (($(stat -L -c %s "$A5_IMG") <= p8size && $(stat -L -c %s "$A5_DTB") <= p9size)) ||
    lab_die "the A5 image or its device tree is larger than recovery ($p8size) or recovery-dtb ($p9size)"
  rec_parts_check || lab_die "recovery or recovery-dtb on the box differs from the backup: the backup would not restore them (L2)"
  lab_log "recovery and recovery-dtb match the backup"

  step_begin write
  lab_lock_check
  # A copy of both partitions on APP, from which --restore writes them back without an upload.
  lab_box_root -t 300 'mkdir -p "$1" && chmod 700 "$1" && dd if=/dev/disk/by-partlabel/recovery of="$1/recovery.img" bs=1M status=none &&
dd if=/dev/disk/by-partlabel/recovery-dtb of="$1/recovery-dtb.img" bs=1M status=none && sync &&
[ "$(sha256sum "$1/recovery.img" | cut -d" " -f1)" = "$2" ] && [ "$(sha256sum "$1/recovery-dtb.img" | cut -d" " -f1)" = "$3" ]' \
    "$ONBOX" "$BK8" "$BK9" || stop copy-failed "the copy of recovery and recovery-dtb on APP failed or does not match the backup"
  isha=$(sha256sum <"$A5_IMG" | cut -d' ' -f1) dsha=$(sha256sum <"$A5_DTB" | cut -d' ' -f1)
  lab_need "$((2 * B6_UPLOAD_TMO))" "the A5 upload"
  box_root_stdin "$B6_UPLOAD_TMO" 'cat >"$1" && sync' "$ONBOX/a5.img" <"$A5_IMG" || stop upload-failed "the A5 image upload failed"
  box_root_stdin "$B6_UPLOAD_TMO" 'cat >"$1" && sync' "$ONBOX/a5.dtb" <"$A5_DTB" || stop upload-failed "the A5 device tree upload failed"
  lab_lock_check
  touch "$B6_DIR/t6-written"
  # Each written whole, padded with zeros to the partition's size, and read back.
  lab_box_root -t 300 'set -e
d=$1
w() { # <file in d> <partition label> <the file'"'"'s sha256>
  [ "$(sha256sum "$d/$1" | cut -d" " -f1)" = "$3" ] || { echo "$1 does not match the upload"; exit 4; }
  cp "$d/$1" "$d/$1.new"
  truncate -s "$(blockdev --getsize64 "/dev/disk/by-partlabel/$2")" "$d/$1.new"
  dd if="$d/$1.new" of="/dev/disk/by-partlabel/$2" bs=1M conv=fsync status=none
  sync
  [ "$(sha256sum "/dev/disk/by-partlabel/$2" | cut -d" " -f1)" = "$(sha256sum "$d/$1.new" | cut -d" " -f1)" ] || { echo "$2 reads back wrong"; exit 5; }
}
w a5.img recovery "$2"
w a5.dtb recovery-dtb "$3"' "$ONBOX" "$isha" "$dsha" >"$LAB_RUN/t6-write.out" 2>&1 || stop write-failed "writing the A5 rescue failed: $(tr '\n' ' ' <"$LAB_RUN/t6-write.out") (b6.sh t6 --restore)"
  note "A5 ${A5_IMG##*/} and ${A5_DTB##*/} written to recovery and recovery-dtb"
  row ok

  step_begin a5-rescue
  boot_step "$E_FJARR" reboot "$B6_REC_TMO"
  wait_up "$B6_REC_TMO" "$BASE_BOOT" a5 || up_failed a5-rescue "the A5 rescue"
  R[came_up_as]=$UP R[to_up_s]=$((UP_AT - T_CMD))
  T_TOUCH=$UP_AT # its dead-man counts from its boot; nothing here touches it
  det_box "A5 rescue: kernel, command line, uptime" a5 'uname -a; cat /proc/cmdline; cat /proc/uptime'
  det_box "A5 rescue: disks" a5 'lsblk -o NAME,SIZE,TYPE,RO,PARTLABEL 2>&1 || cat /proc/partitions'
  read_state NOW a5 && det "A5 rescue: boot state" "$(state_text NOW)" && state_fields
  deadman_wait
  back_to_stock rcm
  stock_after
  row ok

  step_begin restore
  t6_restore_parts
  row ok
  marker_set pass "A5 rescue through Boot$E_FJARR, dead-man to RCM; recovery restored"
}

wd_alive() { # <boot id> <s>: polls every 2 s; 0 when the same boot answered throughout; ALIVE_LAST
  local t0=$SECONDS b
  ALIVE_LAST=$SECONDS
  while ((SECONDS - t0 < $2)); do
    b=$(lab_box -t 5 'cat /proc/sys/kernel/random/boot_id' 2>/dev/null) || b=''
    if [[ $b == "$1" ]]; then ALIVE_LAST=$SECONDS; elif [[ -n $b ]] || ((SECONDS - ALIVE_LAST > 15)); then return 1; fi
    sleep 2
  done
}

t3_back() { # <s>: the box reset by itself; wait for it to come up, then t3_landed
  wait_up "$1" "$BASE_BOOT" stock rec || up_failed after-reset "the box after the reset"
  t3_landed
}

t3_landed() { # after a reset in T3: the stock system verified, the counters read; the stock recovery stops the run
  R[came_up_as]=$UP R[to_up_s]=$((UP_AT - T_CMD))
  if [[ $UP == rec ]]; then
    det_box "stock recovery after $TEST ${R[step]}: the safety script's log" rec 'cat /run/fjarr/log'
    read_state NOW rec && det "stock recovery: boot state" "$(state_text NOW)" && state_fields
    note "landed in the stock recovery (the slot marked unbootable); the safety script reset it"
    on_box_as -t 20 rec sync || true
    back_to_stock cycle
    stock_after lenient
    row recovery
    lab_die "T3 reached the stock recovery: stopped here for a person to look (L7)"
  fi
  stock_after lenient
  det_box "the verifier after $TEST ${R[step]}" stock 'sudo -n journalctl -b -o cat --no-pager -u nv-l4t-bootloader-config.service | tail -n 20; sudo -n nvbootctrl -t rootfs dump-slots-info'
}

t3() {
  local out v boot
  marker_need t4
  marker_need t6 skip-ok
  stock_preflight
  collect_secrets
  lab_backup_check || lab_die "no verified backup (L2)"
  net_need
  [[ -s $B6_DIR/entry && $E_FJARR == "$(cat "$B6_DIR/entry")" ]] || lab_die "T4's entry is not on the box as T4 left it"
  ota_check
  [[ $OTA_STATE == ours ]] || lab_die "the safety script is not at APP:$OTA_DIR: T3 needs T4's safety net"
  [[ ! -e $B6_DIR/t6-written ]] || lab_die "recovery still holds T6's image: b6.sh t6 --restore first (the stock recovery is the safety net)"
  rec_parts_check || lab_die "recovery or recovery-dtb on the box differs from the backup: the stock recovery, T3's safety net, is not there (L7)"
  lab_log "recovery and recovery-dtb match the backup: the stock recovery is T3's safety net"
  grep -q 'efivarfs write ok' "$B6_DIR/t4.pass" || lab_die "T4's pass does not record the safety script's efivarfs write: t4 again first"
  lab_box -t 20 'command -v python3 >/dev/null' || lab_die "no python3 on the box for the watchdog's ioctl"
  det "boot state before T3" "$(state_text BASE)"

  step_begin systemd-watchdog-off
  lab_lock_check
  warm_guard
  lab_desktop_guard
  lab_need "$((150 + $(boot_worst "$LAB_BACK_TMO")))" "systemd letting go of the watchdog"
  out=$(lab_box_root -t 90 'mkdir -p "$(dirname "$1")"
printf "%s\n" "# spikes/ota-jetson/b6.sh t3: systemd leaves the hardware watchdog alone. Removed by t3 or b6.sh clean." "[Manager]" "RuntimeWatchdogSec=0" >"$1"
sync; systemctl daemon-reexec; sleep 3
systemctl show -p RuntimeWatchdogUSec
echo "holders=$(fuser /dev/watchdog /dev/watchdog0 2>/dev/null | tr -s " ")"
dmesg | grep -iE "watchdog|wdt" | tail -n 5' "$DROPIN" 2>&1) || stop dropin-failed "the drop-in failed: $out"
  det "systemd's watchdog off ($DROPIN)" "$out"
  grep -qx 'RuntimeWatchdogUSec=0' <<<"$out" || stop dropin-ignored "systemd still runs its watchdog"
  grep -qE '^holders= *$' <<<"$out" || stop watchdog-held "something still holds /dev/watchdog: $(grep '^holders=' <<<"$out")"
  boot=$BASE_BOOT
  lab_log "waiting 150 s: does the watchdog systemd let go of reset the box?"
  if ! wd_alive "$boot" 150; then
    T_CMD=$ALIVE_LAST
    note "reset within 150 s of systemd letting go of the watchdog"
    warm_add
    t3_back "$LAB_BACK_TMO"
    row reset
    lab_die "the watchdog reset the box after systemd let go of it: stopped for a person (L7)"
  fi
  note "no reset in 150 s"
  row ok

  step_begin wdt-open-close
  lab_lock_check
  warm_guard
  lab_desktop_guard
  lab_need "$(boot_worst "$LAB_BACK_TMO")" "the watchdog test"
  out=$(lab_box_root -t 30 'python3 - <<"PY"
import fcntl, os, struct, time
fd = os.open("/dev/watchdog", os.O_WRONLY)
try:
    t = struct.unpack("i", fcntl.ioctl(fd, 0xC0045706, struct.pack("i", 10)))[0]  # WDIOC_SETTIMEOUT
except OSError as e:
    t = "refused (%s)" % e
print("opened /dev/watchdog, timeout set to %s s, at %.1f" % (t, time.time()))
os.close(fd)  # no magic close: armed, unless the driver feeds it itself
print("closed without the magic close at %.1f" % time.time())
PY
dmesg | grep -iE "watchdog|wdt" | tail -n 3' 2>&1) || stop wdt-failed "opening the watchdog failed: $out"
  T_CMD=$SECONDS
  det "watchdog: opened with 10 s, closed without the magic close" "$out"
  grep -q 'timeout set to 10 s' <<<"$out" || stop wdt-timeout "the driver did not take a 10 s timeout"
  if wd_alive "$boot" 60; then
    # The kernel says "watchdog did not stop!" when a close leaves it running; without that line,
    # the close stopped it (a driver without WDIOF_MAGICCLOSE stops on any close).
    note "no reset in 60 s: the close stopped the watchdog, or the driver feeds it itself (the kernel log tells)"
    det_box "kernel log after 60 s" stock 'sudo -n dmesg | grep -iE "watchdog|wdt" | tail -n 5'
    row no-reset
  else
    warm_add
    note "reset $((ALIVE_LAST - T_CMD))-$((ALIVE_LAST - T_CMD + 2)) s after the close: the driver does not feed it"
    t3_back "$LAB_BACK_TMO"
    row reset
  fi

  # The question the close leaves open: with the device held open and no one feeding it, does
  # the watchdog reset the box? A holder that feeds once, then sleeps 120 s with it open.
  step_begin wdt-open-hold
  lab_lock_check
  warm_guard
  lab_desktop_guard
  lab_need "$((60 + $(boot_worst "$LAB_BACK_TMO")))" "the watchdog test"
  boot=$BASE_BOOT
  out=$(lab_box_root -t 30 'fuser /dev/watchdog /dev/watchdog0 >/dev/null 2>&1 && { echo "held by another process"; exit 1; }
setsid python3 - >/run/fjarr-b6-wdt-hold.log 2>&1 </dev/null <<"PY" &
import fcntl, os, struct, time
fd = os.open("/dev/watchdog", os.O_WRONLY)
t = struct.unpack("i", fcntl.ioctl(fd, 0xC0045706, struct.pack("i", 10)))[0]  # WDIOC_SETTIMEOUT
fcntl.ioctl(fd, 0x80045705, struct.pack("i", 0))  # WDIOC_KEEPALIVE, the last feed
print("held /dev/watchdog, timeout %s s, last fed at %.1f" % (t, time.time()), flush=True)
time.sleep(120)
os.write(fd, b"V")  # the magic close, if it is still alive after 120 s
os.close(fd)
print("released with the magic close at %.1f" % time.time(), flush=True)
PY
sleep 2; cat /run/fjarr-b6-wdt-hold.log' 2>&1) || stop wdt-hold-failed "holding the watchdog failed: $out"
  T_CMD=$SECONDS
  det "watchdog: held open with 10 s, fed once" "$out"
  grep -q 'timeout 10 s' <<<"$out" || stop wdt-timeout "the driver did not take a 10 s timeout: $out"
  if wd_alive "$boot" 60; then
    note "no reset in 60 s with the watchdog held open and not fed: something feeds it"
    det_box "kernel log after 60 s" stock 'sudo -n dmesg | grep -iE "watchdog|wdt" | tail -n 5'
    # The panics next must find the watchdog let go of, as the steps before left it.
    out=$(lab_box_root -t 120 'sleep 70; cat /run/fjarr-b6-wdt-hold.log; fuser /dev/watchdog /dev/watchdog0 2>&1 || echo "not held"' 2>&1) || true
    det "the holder at its end" "$out"
    grep -q 'not held' <<<"$out" || stop wdt-still-held "the holder still holds the watchdog: $out"
    row no-reset
  else
    warm_add
    note "reset $((ALIVE_LAST - T_CMD))-$((ALIVE_LAST - T_CMD + 2)) s after the last feed: the watchdog fires when its holder stops feeding"
    t3_back "$LAB_BACK_TMO"
    row reset
  fi

  for v in 0 10; do
    step_begin "sysrq-c-panic-$v"
    boot_step - panic "$B6_HANG_S" "$v"
    if [[ $v == 10 ]]; then
      t3_back "$LAB_BACK_TMO"
      note "back $((UP_AT - T_CMD)) s after the panic"
      row ok
    elif wait_up "$B6_HANG_S" "$BASE_BOOT" stock rec; then
      note "reset by itself $((UP_AT - T_CMD)) s after the panic"
      t3_landed
      row reset
    else
      [[ $UP == nothing ]] || stop "up-$UP" "the box came up as '$UP' after the panic"
      note "no reset within $B6_HANG_S s of the panic with kernel.panic=0: hung; power cycled"
      back_to_stock cycle
      R[came_up_as]=stock-after-power-cycle
      stock_after lenient
      row hung
    fi
  done

  step_begin systemd-watchdog-on
  lab_lock_check
  out=$(lab_box_root -t 90 'rm -f "$1"; sync; systemctl daemon-reexec; sleep 3; systemctl show -p RuntimeWatchdogUSec
echo "holders=$(fuser /dev/watchdog /dev/watchdog0 2>/dev/null | tr -s " ")"' "$DROPIN" 2>&1) ||
    stop dropin-remove "removing the drop-in failed: $out"
  det "systemd's watchdog back on" "$out"
  ! grep -qx 'RuntimeWatchdogUSec=0' <<<"$out" || stop dropin-left "systemd's watchdog is still off after the drop-in's removal"
  note "$(grep -E '^(RuntimeWatchdogUSec|holders)=' <<<"$out" | tr '\n' ' ')"
  row ok
  marker_set pass "watchdog and panic tests done"
}

serial_script() { # as root on the mini-PC: $1 seconds, $2 the box's USB vendor, $3 1 to poke
  cat <<'EOF'
dur=$1 vendor=$2 poke=$3 sys=/sys/bus/usb/devices
end=$(($(date +%s) + dur)) seen=0
echo "start $EPOCHREALTIME"
echo "modemmanager $(systemctl is-active ModemManager 2>/dev/null)"
while (($(date +%s) < end)); do
  dev=
  for d in "$sys"/*; do [[ -r $d/idVendor && $(<"$d/idVendor") == "$vendor" ]] && dev=$d; done
  if [[ -z $dev ]]; then
    ((seen)) && echo "usb-gone $EPOCHREALTIME"
    seen=0; sleep 0.2; continue
  fi
  ((seen)) || echo "usb-seen $EPOCHREALTIME $(<"$dev/idProduct")"
  seen=1
  t=
  for x in "$dev"/*/tty/ttyACM*; do [[ -e $x ]] && t=$x && break; done
  [[ -n $t ]] || { sleep 0.2; continue; }
  t=/dev/${t##*/}
  echo "tty-seen $EPOCHREALTIME $t"
  stty -F "$t" 115200 raw -echo 2>/dev/null
  ((poke)) && (sleep 5; printf '\r' >"$t") 2>/dev/null &
  while :; do
    line=''
    IFS= read -r -t 1 line; rc=$?
    line=${line//$'\r'/}
    [[ -n $line ]] && echo "tty $EPOCHREALTIME $line"
    ((rc == 0 || rc > 128)) || break
  done < <(rem=$((end - $(date +%s))); ((rem > 0)) && timeout "$rem" cat "$t" 2>/dev/null)
  echo "tty-closed $EPOCHREALTIME"
  sleep 0.2
done
echo "end $EPOCHREALTIME"
EOF
}

serial() {
  local l0 l1 r skew t_cmd pid summary
  [[ ${LAB_DESKTOP_OK:-} == 1 ]] || lab_die "serial needs LAB_DESKTOP_OK=1: the mini-PC binds the box's tty only when its USB controller is not isolated"
  stock_preflight
  collect_secrets
  step_begin serial-console
  lab_need "$(boot_worst "$LAB_BACK_TMO")" "the reboot with the console watched"
  l0=$(date +%s.%N)
  r=$(lab_mpc -t 20 'date +%s.%N') || lab_die "the mini-PC does not answer"
  l1=$(date +%s.%N)
  skew=$(awk -v a="$l0" -v b="$l1" -v r="$r" 'BEGIN { printf "%.2f", r - (a + b) / 2 }')
  (
    lab_bg_detach
    lab_mpc_root -t $((B6_SERIAL_S + 30)) "$(serial_script)" "$B6_SERIAL_S" "${LAB_RCM_USB%%:*}" "$B6_SERIAL_POKE"
  ) >"$LAB_RUN/serial.raw" 2>&1 &
  pid=$!
  LAB_BG_PIDS+=("$pid")
  sleep 10
  t_cmd=$(date +%s.%N)
  boot_step - reboot
  wait_up "$LAB_BACK_TMO" "$BASE_BOOT" stock || up_failed stock "the stock system"
  R[came_up_as]=$UP R[to_up_s]=$((UP_AT - T_CMD))
  stock_after
  lab_log "watching the tty until $B6_SERIAL_S s after the start"
  wait "$pid" || true
  summary=$(awk -v c="$t_cmd" -v k="$skew" '
    { t = $2 - k - c }
    $1 == "modemmanager" { print "ModemManager on the mini-PC: " $2; next }
    $1 == "usb-gone" && t > 0 && !gone { gone = t; printf "box gone from USB: %.1f s after the reboot command\n", t }
    $1 == "usb-seen" && gone && !seen { seen = t; printf "box back on USB (%s): %.1f s\n", $3, t }
    $1 == "tty-seen" && seen && !tty { tty = t; printf "tty %s: %.1f s\n", $3, t }
    $1 == "tty" && tty && !first { first = t; printf "first output on the tty: %.1f s, %.1f s after the tty appeared\n", t, t - tty }
    END { if (!first) print "no output on the tty after the reboot" }' "$LAB_RUN/serial.raw")
  det "serial console: summary (seconds after the reboot command; clocks $skew s apart, mini-PC minus here)" "$summary"
  det "serial console: what the mini-PC saw (epoch seconds, the mini-PC's clock)" "$(cat "$LAB_RUN/serial.raw")"
  note "$(grep -E '^(tty /|first)' <<<"$summary" | tr '\n' ' ')"
  row ok
}

clean() {
  local out e
  lab_plug_check || lab_die "the plug does not answer (L2)"
  lab_find_box || lab_die "the box does not answer on its MAC (L2)"
  lab_box_bootid || lab_die "the box does not answer ssh on its stock system"
  [[ ! -e $B6_DIR/t6-written ]] || lab_die "recovery still holds T6's image: b6.sh t6 --restore first"
  lab_backup_check || lab_die "no verified backup (L2)"
  net_need
  collect_secrets
  entries_read
  read_state BASE || lab_die "could not read the boot state"
  step_begin clean
  lab_lock_check
  for e in $E_FJARR; do
    lab_box_root -t 30 'efibootmgr -q -b "$1" -B' "$e" >/dev/null || stop clean-entry "efibootmgr -B $e failed"
    note "Boot$e removed"
  done
  out=$(lab_box_root -t 90 '
if [ -e "$1" ]; then
  if grep -qs "b6.sh t4" "$1/nv_ota_run_tasks.sh"; then rm -rf "$1" && echo "ota_work removed"; else echo "ota_work is not ours: left"; fi
fi
if [ -e "$2" ]; then rm -f "$2" && systemctl daemon-reexec && echo "drop-in removed"; fi
if [ -d "$3" ]; then rm -rf "$3" && echo "T6 copies removed"; fi
sync' "$OTA_DIR" "$DROPIN" "$ONBOX" 2>&1) || stop clean-files "the cleanup on the box failed: $out"
  [[ -n $out ]] && note "$(tr '\n' ';' <<<"$out")"
  rm -f "$B6_DIR/entry"
  read_state NOW || stop no-state "could not read the boot state"
  [[ ${NOW[BootNext]} == absent && ${NOW[BootOrder]} == "${BASE[BootOrder]}" ]] || stop clean-state "BootNext or BootOrder is not as before the cleanup"
  det "boot entries after the cleanup" "$(lab_box -t 30 'efibootmgr -v' 2>&1)"
  row ok
}

net_down() {
  mpc_dir
  lab_log "mini-PC: net-rescue.sh --undo (refused while a lab run holds the box lock)"
  lab_mpc_root -t 600 '[ -x "$1" ] || { echo "no $1: nothing to undo here"; exit 0; }; "$1" --undo' "$MPC_D/net-rescue.sh" ||
    lab_die "net-rescue.sh --undo failed or refused"
}

check() { # read only: every precondition, a line each
  local s out ok=1
  say() { lab_log "check: $*"; }
  for s in t2 t5 t4 t6 t3; do
    if [[ -e $B6_DIR/$s.pass ]]; then say "$s passed $(cut -d' ' -f1 "$B6_DIR/$s.pass")"; elif [[ -e $B6_DIR/$s.skip ]]; then say "$s skipped"; else say "$s not run"; fi
  done
  say "warm resets since a verified boot: $(warm_get)"
  stock_preflight
  collect_secrets
  say "entries: NVMe Boot$E_NVME, PXEv4 Boot${E_PXE4:-?}, HTTPv4 Boot${E_HTTP4:-?}, $ENTRY_LABEL ${E_FJARR:+Boot}${E_FJARR:-absent}"
  esp_check
  ota_check
  say "/ota_work: $OTA_STATE"
  if lab_box -t 20 'command -v python3 >/dev/null'; then say "python3 on the box: yes"; else
    say "python3 on the box: NO (T3 needs it)"
    ok=0
  fi
  if lab_box -t 20 "test -e $DROPIN"; then say "T3's drop-in is on the box"; else say "T3's drop-in: absent"; fi
  if lab_backup_check; then
    if rec_parts_check; then say "recovery and recovery-dtb match the backup"; else
      say "recovery or recovery-dtb DIFFERS from the backup"
      ok=0
    fi
  else
    say "backup: NOT verified"
    ok=0
  fi
  box_mac_check
  if [[ -s $B6_NET_EFI && -s $B6_NET_KNOWN_HOSTS ]]; then say "network rescue built: ${B6_NET_EFI#"$LAB_HOME"/}"; else
    say "network rescue: NOT built"
    ok=0
  fi
  if net_status; then say "network rescue: serving (b6.sh net-down at the end of the session)"; else say "network rescue: not serving (${NET_STATUS:-})"; fi
  if a5_find; then say "A5 rescue: ${A5_IMG##*/}, ${A5_DTB##*/}"; else say "A5 rescue: not built, T6 will be skipped"; fi
  if [[ -n $(pubkeys) ]]; then say "public keys for the stock recovery: $(pubkeys | wc -l)"; else
    say "no public key for the stock recovery"
    ok=0
  fi
  out=$(lab_mpc -t 20 "v=${LAB_RCM_USB%%:*}; "'for d in /sys/bus/usb/devices/*; do [ "$(cat "$d/idVendor" 2>/dev/null)" = "$v" ] || continue; ls -d "$d"/*/tty/ttyACM* 2>/dev/null; done | sed "s|.*/||" | paste -sd" " -; systemctl is-active ModemManager 2>/dev/null || true' 2>&1) || out=unknown
  say "the box's tty on the mini-PC: $(head -n 1 <<<"$out" | sed 's/^$/none/'); ModemManager: $(tail -n 1 <<<"$out")"
  [[ ${LAB_DESKTOP_OK:-} == 1 ]] || lab_usb_isolate check || true
  say "LAB_DESKTOP_OK=${LAB_DESKTOP_OK:-unset} LAB_DESKTOP_ISOLATE=${LAB_DESKTOP_ISOLATE:-unset}"
  if ((ok)); then say "nothing missing"; else say "something above is missing"; fi
  return 0
}

case $cmd in
  check) check ;;
  t2) t2 ;;
  net-up) net_up ;;
  t5) t5 ;;
  t4) t4 ;;
  t6) t6 ;;
  t3) t3 ;;
  serial) serial ;;
  clean) clean ;;
  net-down) net_down ;;
esac
[[ $cmd == check || $cmd == net-up || $cmd == net-down ]] || results_out || true
