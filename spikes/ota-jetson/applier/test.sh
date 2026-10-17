#!/bin/bash
# The Jetson applier's tests, off the box (docs/32-fleet-ota.md#jetson-test-image, phase A).
# Every verb runs against a fake root (efivarfs, the PMC nodes, the ESRT, the ESP, /data and the
# partitions as plain files) with the tools in stubs/ first in its PATH. Each case must make
# exactly the writes its row in README.md lists, in that order; a refusal makes none. Then the
# end-to-end runs drive whole updates through a model of the firmware read from edk2-nvidia.
# Run it as an ordinary user, which the model of efivarfs' immutable files needs:
#   docker run --rm --network none -u "$(id -u):$(id -g)" \
#     -v "$PWD/spikes/ota-jetson/applier:/a:ro" fjarr-dev bash /a/test.sh
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
applier=$here/applier
readme=$here/README.md
[ "$(id -u)" != 0 ] || { echo "run test.sh as an ordinary user: root ignores the immutable model" >&2; exit 2; }
NV=781e084c-a330-417c-b678-38e696380cb9
NVTOK=ed3374ef-767b-42fa-af70-db520a392822
EFIG=8be4df61-93ca-11d2-aa0d-00e098032b8c
PMCD=sys/devices/platform/bus@0/c360000.pmc
fails=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }
expect() { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1: expected '$3', got '$2'"; fi; }

# --- The fake box --------------------------------------------------------------------------
le() { local i b out=''; for ((i = 0; i < $2; i++)); do printf -v b '%02x' $((($1 >> (8 * i)) & 255)); out+=$b; done; echo "$out"; }
hex_bytes() { local i esc=''; for ((i = 0; i < ${#1}; i += 2)); do esc+="\\x${1:i:2}"; done; printf '%b' "$esc"; }
var() { # <name> <guid> <attributes> <bytes> <value>: a UEFI variable; vendor ones are immutable
  local f=$R/sys/firmware/efi/efivars/$1-$2
  [ ! -e "$f" ] || chmod u+w "$f"
  hex_bytes "$(le "$3" 4)$(le "$5" "$4")" > "$f"
  [ "$2" = "$EFIG" ] || chmod a-w "$f"
}
unvar() { rm -f "$R/sys/firmware/efi/efivars/$1-$2"; }
varv() { # <name> <guid>: the value, or nothing when absent
  local f=$R/sys/firmware/efi/efivars/$1-$2 b i v=0
  [ -e "$f" ] || return 0
  read -r -a b <<<"$(od -An -v -tu1 "$f" | tr '\n' ' ')"
  for ((i = 4; i < ${#b[@]}; i++)); do v=$((v | (b[i] << (8 * (i - 4))))); done
  echo "$v"
}
pmc() { cat "$R/$PMCD/$1"; }
pmcw() { printf '0x%x\n' "$2" > "$R/$PMCD/$1"; }
kvw() { local f=$1; shift; printf '%s\n' "$@" > "$f"; }
kv() { sed -n "s/^$2=//p" "$1" 2>/dev/null | head -n 1; }
base=${TMPDIR:-/tmp}
tmps=()
cleanup() { local d; for d in ${tmps[@]+"${tmps[@]}"}; do chmod -R u+w "$d"; rm -rf "$d"; done; }
trap cleanup EXIT
setup() { # booted 1.0 on slot A, this boot verified, both chains on 36.5.2; release 1.2 staged
  T=$(cd "$(mktemp -d "$base/applier-test.XXXXXX")" && pwd -P); R=$T/root; LOG=$T/log; tmps+=("$T")
  export T R LOG FJARR_ROOT=$R TMPDIR=$T/tmp
  mkdir -p "$T/tmp" "$T/fw" "$R/sys/firmware/efi/efivars" "$R/$PMCD" "$R/sys/firmware/efi/esrt/entries/entry0" \
    "$R/sys/class/dmi/id" "$R/boot/efi/EFI/BOOT" "$R/dev/disk/by-partlabel" "$R/etc/fjarr" "$R/run/fjarr-applier" \
    "$R/data/fjarr-update/applier" "$R/data/fjarr-update/tmp" "$R/data/fjarr-update/stage/1.2" "$R/proc/sys/kernel/random"
  # NVIDIA's NVMe A/B table (flash_l4t_t234_nvme_rootfs_ab.xml) in its partition order, then /data.
  local n=0 p
  : > "$T/partmap"
  for p in APP APP_b A_kernel A_kernel-dtb A_reserved_on_user B_kernel B_kernel-dtb B_reserved_on_user \
    recovery recovery-dtb esp recovery_alt recovery-dtb_alt esp_alt UDA reserved data; do
    n=$((n + 1)); printf 'partition %s\n' "$p" > "$R/dev/nvme0n1p$n"
    ln -s "../../nvme0n1p$n" "$R/dev/disk/by-partlabel/$p"; echo "dev/nvme0n1p$n $p" >> "$T/partmap"
  done
  printf 'label=fjarr_1.0\n' >> "$R/dev/nvme0n1p1"; printf 'label=fjarr_1.0\n' >> "$R/dev/nvme0n1p2"
  # UEFI variables as the 36.5.2 firmware sets them on a box flashed for root A/B.
  var RootfsStatusSlotA "$NV" 7 4 0; var RootfsStatusSlotB "$NV" 7 4 0
  var RootfsRetryCountMax "$NV" 6 4 3; var RootfsRedundancyLevel "$NV" 6 4 1
  var BootChainFwCurrent "$NV" 6 4 0; var BootChainOsCurrent "$NV" 6 4 0; var L4TDefaultBootMode "$NV" 7 4 1
  pmcw rootfs_sr_magic 0xface; pmcw rootfs_current 0; pmcw rootfs_retry_count_a 3; pmcw rootfs_retry_count_b 3
  pmcw boot_chain_status_a 0; pmcw boot_chain_status_b 0; pmcw boot_chain_current 0
  local e=$R/sys/firmware/efi/esrt/entries/entry0
  echo 2360578 > "$e/fw_version"; echo 2360578 > "$e/lowest_supported_fw_version"   # 0x00240502: 36.5.2
  echo 0 > "$e/last_attempt_version"; echo 0 > "$e/last_attempt_status"
  echo 36.5.2-gcid-00000000 > "$R/sys/class/dmi/id/bios_version"
  printf 'ID=fjarr-spike\nIMAGE_ID=fjarr\nIMAGE_VERSION=1.0\n' > "$R/etc/os-release"
  echo test-certificate > "$R/etc/fjarr/swu.crt"
  echo 00000000-0000-4000-8000-000000000001 > "$R/proc/sys/kernel/random/boot_id"
  echo L4TLauncher > "$R/boot/efi/EFI/BOOT/BOOTAA64.efi"
  kvw "$R/data/fjarr-update/applier/chain-a" fw=36.5.2 state=complete
  kvw "$R/data/fjarr-update/applier/chain-b" fw=36.5.2 state=complete
  kvw "$R/run/fjarr-applier/verified" "boot=00000000-0000-4000-8000-000000000001"
  st=$R/data/fjarr-update/stage/1.2   # global: the cases name the release directory by it
  echo "swupdate package 1.2" > "$st/fjarr_1.2.swu"; echo 36.5.2 > "$st/fjarr_1.2.fw"; echo "capsule 36.5.2" > "$st/fjarr_1.2.cap"
  echo 0 > "$T/fw/current"; echo "$R/dev/nvme0n1p1" > "$T/fw/root"
  echo 36.5.2 > "$T/fw/chain-0"; echo 36.5.2 > "$T/fw/chain-1"; sysfw
}
fwhex() { local IFS=.; read -r x y z <<<"$1"; echo $(((x << 16) | (y << 8) | z)); }
sysfw() { # SystemFwVersions as UEFI sets it at each boot, from each chain's VER: [A, B], or <a> <b> raw
  local a b
  a=${1:-$(fwhex "$(cat "$T/fw/chain-0")")}; b=${2:-$(fwhex "$(cat "$T/fw/chain-1")")}
  var SystemFwVersions "$NV" 6 8 $((a | (b << 32)))
}
b_on() { echo "$1" > "$T/fw/chain-1"; sysfw; kvw "$R/data/fjarr-update/applier/chain-b" "fw=$1" state=complete; }
on_slot() { # <a|b>: what Linux sees after booting that slot (firmware chain, root, bios_version)
  local i; i=$([ "$1" = a ] && echo 0 || echo 1)
  echo "$i" > "$T/fw/current"
  echo "$R/dev/nvme0n1p$((i + 1))" > "$T/fw/root"
  var BootChainFwCurrent "$NV" 6 4 "$i"; var BootChainOsCurrent "$NV" 6 4 "$i"
  local fw; fw=$(cat "$T/fw/chain-$i")
  echo "$fw-gcid-00000000" > "$R/sys/class/dmi/id/bios_version"
  fwhex "$fw" > "$R/sys/firmware/efi/esrt/entries/entry0/fw_version"
  sysfw
  sed -i "s/^IMAGE_VERSION=.*/IMAGE_VERSION=$(sed -n 's/^label=fjarr_//p' "$R/dev/nvme0n1p$((i + 1))")/" "$R/etc/os-release"
}

# --- Running a verb and checking what it wrote -------------------------------------------------
snap() { # every path under the fake root with its type, mode, link and content
  (cd "$R" && find . -mindepth 1 ! -path ./run/fjarr-applier.lock -printf '%y %m %p -> %l\n' | LC_ALL=C sort)
  (cd "$R" && find . -type f ! -path ./run/fjarr-applier.lock -exec sha256sum {} + | LC_ALL=C sort -k2)
}
run() { # <args...>: run the applier as the box would, with the stubs first in its PATH
  : > "$LOG"
  snap > "$T/before"
  PATH="$here/stubs:$PATH" "$applier" "$@" > "$T/out" 2> "$T/err"
  echo $? > "$T/rc"
  snap > "$T/after"
}
changed() { # the paths a run changed, as area tokens
  diff "$T/before" "$T/after" | sed -n 's/^[<>] //p' | while read -r a b c _; do
    if [[ $a =~ ^[0-9a-f]{64}$ ]]; then echo "${b#./}"; else echo "${c#./}"; fi
  done | LC_ALL=C sort -u | while read -r p; do
    case "$p" in
    sys/firmware/efi/efivars/*) n=${p##*/}; echo "efi:${n%-*-*-*-*-*}" ;;
    "$PMCD"/*) echo "pmc:${p##*/}" ;;
    boot/efi/*) echo "esp:${p#boot/efi/}" ;;
    data/fjarr-update/*) echo "data:${p#data/fjarr-update/}" ;;
    run/fjarr-applier/*) echo "run:${p#run/fjarr-applier/}" ;;
    dev/nvme*) echo "part:$(awk -v d="$p" '$1 == d {print $2}' "$T/partmap")" ;;
    *) echo "other:$p" ;;
    esac
  done | LC_ALL=C sort -u
}
writes() { grep -vE '^(chattr:|sync:data$|sync:run$)' "$LOG" || true; }
row() { # <case>: the writes its row in README.md lists (the backticked tokens of the last column)
  local line; line=$(grep -E "^\| $1 \|" "$readme" | head -n 1)
  [ -n "$line" ] || { echo "no README row"; return; }
  # shellcheck disable=SC2016 # the backticks are markdown, not command substitution
  awk -F'|' '{print $(NF-1)}' <<<"$line" | grep -oE '`[^`]+`' | tr -d '`' || true
}
invariants() { # <case>: what every run must keep, whatever its row
  local c=$1 i prev line t
  # A UEFI variable is deleted only right after chattr -i (efivarfs refuses an immutable unlink).
  prev=
  while IFS= read -r line; do
    if [[ $line == efi:-* ]] && [ "$prev" != "chattr:-i:efi:${line#efi:-}" ]; then bad "$c: $line without chattr -i first"; fi
    prev=$line
  done < "$LOG"
  # Vendor variables stay immutable (chattr +i after each write); the model is the write bit.
  for t in "$R"/sys/firmware/efi/efivars/*; do
    [ -e "$t" ] || continue
    case "$t" in *-$EFIG) ;; *) [ ! -w "$t" ] || bad "$c: ${t##*/} left mutable" ;; esac
  done
  # Every change to /data is flushed before the next step.
  mapfile -t ev < "$LOG"
  for ((i = 0; i < ${#ev[@]}; i++)); do
    if [[ ${ev[i]} == data:* ]] && [ "${ev[i + 1]:-}" != sync:data ]; then bad "$c: ${ev[i]} not followed by sync -f"; fi
  done
  # Never the ESP's boot files (L5), never anything outside what was logged.
  grep -q '^esp:.*EFI/BOOT/' "$LOG" && bad "$c: wrote EFI/BOOT on the ESP"
  changed > "$T/changed"
  while read -r t; do covered "${t%%:*}" "${t#*:}" || bad "$c: unlogged change $t"; done < "$T/changed"
}
covered() { # <area> <path>: a logged write, rename or removal of the path, or of something below it
  awk -v a="$1:" -v p="$2" 'index($0, a) == 1 {
    s = substr($0, length(a) + 1); sub(/^-/, "", s); sub(/=.*/, "", s); n = split(s, q, ">")
    for (i = 1; i <= n; i++) if (q[i] == p || index(q[i], p "/") == 1) f = 1
  } END { exit !f }' "$LOG"
}
case_run() { # <case> <expected exit> <args...>: one row of README.md's table
  local c=$1 rc=$2; shift 2
  run "$@"
  local got; got=$(cat "$T/rc")
  if [ "$got" != "$rc" ]; then bad "$c: exit $got, expected $rc ($(tr '\n' ' ' < "$T/err"))"; fi
  if [ "$(writes)" = "$(row "$c")" ]; then ok "$c: $*"; else
    bad "$c: $*: the writes differ from README.md"; diff <(row "$c") <(writes) | sed 's/^/       /'
  fi
  invariants "$c"
}
refused() { # <case> <reason> <args...>: exit 2 for that reason, nothing written, nothing changed
  local c=$1 why=$2; shift 2
  run "$@"
  local got; got=$(cat "$T/rc")
  if [ "$got" = 2 ] && [ ! -s "$LOG" ] && [ -z "$(changed)" ] && grep -q -- "$why" "$T/err"; then
    ok "$c refused: $(head -n 1 "$T/err")"
  else bad "$c: expected a refusal ($why) with no writes: exit $got, $(tr '\n' ' ' < "$T/err") $(tr '\n' ' ' < "$LOG")"; fi
  grep -qE "^\| $c \|" "$readme" || bad "$c: no row in README.md"
}
json_ok() { # 0 when the state's JSON parses, 1 when it does not, 2 without a parser to tell
  if command -v jq >/dev/null; then jq -e . "$1" >/dev/null 2>&1
  elif command -v python3 >/dev/null; then python3 -I -c 'import json,sys; json.load(open(sys.argv[1]))' "$1" 2>/dev/null
  else return 2; fi
}
jget() { if command -v jq >/dev/null; then jq -r "$2" "$1"; else echo "(no jq)"; fi; }

# --- state ------------------------------------------------------------------------------------
setup
case_run S1 0 state
cp "$T/out" "$T/state.json"
json_ok "$T/state.json"
case $? in
0) ok "S1: state is JSON" ;;
2) echo "skip S1: no jq or python3 to parse the state" ;;
*) bad "S1: state is not JSON: $(head -c 300 "$T/state.json")" ;;
esac
if ! command -v jq >/dev/null; then echo "skip S1: the state's fields (no jq)"; else
  expect "S1: booted" "$(jget "$T/state.json" '"\(.booted) \(.booted_slot) \(.is_trial) \(.verified_this_boot)"')" "1.0 a false true"
  expect "S1: slots" "$(jget "$T/state.json" '[.slots[] | "\(.slot) \(.label) \(.retry_count)"] | join(", ")')" "a fjarr_1.0 3, b fjarr_1.0 3"
  expect "S1: efivars" "$(jget "$T/state.json" '.efivars | "\(.RootfsStatusSlotB.value) \(.RootfsRetryCountMax.attributes) \(.BootChainFwNext) \(.BootChainFwResetCount)"')" "0 6 null null"
  expect "S1: firmware" "$(jget "$T/state.json" '"\(.esrt.fw) \(.bios_version) \(.capsule_status) \(.pmc.rootfs_sr_magic)"')" "36.5.2 36.5.2-gcid-00000000 0 0xface"
  expect "S1: chains" "$(jget "$T/state.json" '[.chains[] | "\(.chain) \(.fw) \(.marker.fw) \(.marker.state)"] | join(", ")')" "a 36.5.2 36.5.2 complete, b 36.5.2 36.5.2 complete"
  expect "S1: nvbootctrl" "$(jget "$T/state.json" '.nvbootctrl.rootfs | split("\n")[0]')" "Current rootfs slot: A"
fi
setup; sysfw 2360578 4294967295; run state
if command -v jq >/dev/null; then
  expect "S1: an unreadable chain" "$(jget "$T/out" '[.chains[].fw] | join(" ")')" "36.5.2 ?"
fi
setup; unvar SystemFwVersions "$NV"; run state
if command -v jq >/dev/null; then
  expect "S1: no SystemFwVersions" "$(jget "$T/out" '[.chains[].fw] | map(tostring) | join(" ")')" "null null"
fi

# --- install ----------------------------------------------------------------------------------
setup
case_run I1 0 install 1.2 "$R/data/fjarr-update/stage/1.2"
expect "I1: the slot record" "$(tr '\n' ' ' < "$R/data/fjarr-update/applier/slot-b")" \
  "version=1.2 fw=36.5.2 capsule=$R/data/fjarr-update/stage/1.2/fjarr_1.2.cap capsule_sha256=$(sha256sum < "$R/data/fjarr-update/stage/1.2/fjarr_1.2.cap" | cut -d' ' -f1) "
expect "I1: slot B bootable again" "$(varv RootfsStatusSlotB "$NV") $(pmc rootfs_retry_count_b)" "0 0x3"
expect "I1: slot A untouched" "$(cat "$R/dev/nvme0n1p1" "$R/dev/nvme0n1p3" | tr '\n' ' ')" "partition APP label=fjarr_1.0 partition A_kernel "
setup; touch "$T/fw/swupdate-fails"
case_run I2 1 install 1.2 "$R/data/fjarr-update/stage/1.2"
expect "I2: a failed install leaves slot B unbootable" "$(varv RootfsStatusSlotB "$NV") $(pmc rootfs_retry_count_b)" "255 0x0"
setup; echo 9.9 > "$T/fw/swupdate-label"
case_run I5 1 install 1.2 "$R/data/fjarr-update/stage/1.2"
expect "I5: a wrong label leaves slot B unbootable" "$(varv RootfsStatusSlotB "$NV") $(pmc rootfs_retry_count_b)" "255 0x0"
setup; rm -f "$R/data/fjarr-update/stage/1.2/fjarr_1.2.cap"
case_run I3 0 install 1.2 "$R/data/fjarr-update/stage/1.2"
expect "I3: no capsule recorded" "$(kv "$R/data/fjarr-update/applier/slot-b" capsule)" ""
setup; kvw "$R/data/fjarr-update/applier/slot-b" version=1.1 fw=36.5.2 capsule= capsule_sha256=
case_run I4 0 install 1.2 "$R/data/fjarr-update/stage/1.2"
setup; refused I10 "the booted version" install 1.0 "$st"
setup; refused I11 "no .*fjarr_1.3.swu" install 1.3 "$st"
setup; rm "$st/fjarr_1.2.fw"; refused I12 "no firmware version" install 1.2 "$st"
setup; kvw "$R/data/fjarr-update/applier/trial" version=1.1 slot=b previous=a boots=1 capsule=0 fw=36.5.2; refused I13 "a trial of 1.1 is in progress" install 1.2 "$st"
setup; rm "$R/run/fjarr-applier/verified"; refused I14 "not been verified" install 1.2 "$st"
setup; var BootChainFwStatus "$NV" 7 4 12; refused I15 "BootChainFwStatus is 12" install 1.2 "$st"
setup; var BootChainFwNext "$NV" 7 4 1; refused I16 "BootChainFwNext is 1" install 1.2 "$st"
setup; var BootNext "$EFIG" 7 2 3; refused I17 "BootNext is set" install 1.2 "$st"
setup; var BootChainFwResetCount "$NVTOK" 3 4 2; refused I18 "BootChainFwResetCount is 2" install 1.2 "$st"
setup; var OsIndications "$EFIG" 7 8 4; refused I19 "a capsule is pending" install 1.2 "$st"
setup; var L4TDefaultBootMode "$NV" 7 4 3; refused I20 "L4TDefaultBootMode is recovery" install 1.2 "$st"
setup; echo 1 > "$T/fw/current"; refused I21 "current slot (b) is not the root partition's (a)" install 1.2 "$st"
setup; rm "$R/$PMCD/rootfs_retry_count_b"; refused I22 "no PMC retry counters" install 1.2 "$st"
setup; var BootChainFwResetCount "$NVTOK" 3 4 1
run install 1.2 "$st"; expect "I18: a visible BootChainFwResetCount of 1 is accepted" "$(cat "$T/rc")" 0

# --- set-next ---------------------------------------------------------------------------------
installed() { # slot B holds 1.2, as I1 leaves it
  setup
  PATH="$here/stubs:$PATH" "$applier" install 1.2 "$R/data/fjarr-update/stage/1.2" > /dev/null 2>&1 || bad "install for set-next"
}
installed
case_run N1 0 set-next 1.2
expect "N1: the trial marker" "$(tr '\n' ' ' < "$R/data/fjarr-update/applier/trial")" "version=1.2 slot=b previous=a boots=0 capsule=0 fw=36.5.2 "
installed; b_on 36.4.4
case_run N2 0 set-next 1.2
expect "N2: the capsule on the ESP" "$(cat "$R/boot/efi/EFI/UpdateCapsule/TEGRA_BL.Cap")" "capsule 36.5.2"
expect "N2: chain B in progress" "$(tr '\n' ' ' < "$R/data/fjarr-update/applier/chain-b")" "fw=36.5.2 state=in-progress "
expect "N2: the trial marker" "$(kv "$R/data/fjarr-update/applier/trial" capsule)" 1
installed; rm "$R/data/fjarr-update/applier/chain-b"
case_run N3 0 set-next 1.2
installed; kvw "$R/data/fjarr-update/applier/chain-b" fw=36.5.2 state=in-progress
case_run N4 0 set-next 1.2
installed; b_on 36.4.4; var OsIndications "$EFIG" 7 8 16
case_run N5 0 set-next 1.2
expect "N5: OsIndications keeps its other bits" "$(varv OsIndications "$EFIG")" 20
installed; b_on 36.4.4
chmod a-w "$R/boot/efi/EFI"
case_run N6 1 set-next 1.2
chmod u+w "$R/boot/efi/EFI"
installed; touch "$T/fw/fail-set-active"
case_run N7 1 set-next 1.2
installed; echo 36.4.4 > "$T/fw/chain-1"; sysfw   # the marker still says 36.5.2, complete
case_run N8 0 set-next 1.2
installed; sysfw 2360578 4294967295
case_run N9 0 set-next 1.2
installed; unvar SystemFwVersions "$NV"; run set-next 1.2
expect "N1 without SystemFwVersions: the marker decides" "$(cat "$T/rc") $(writes | tr '\n' ' ')" "0 $(row N1 | tr '\n' ' ')"
installed; unvar SystemFwVersions "$NV"; kvw "$R/data/fjarr-update/applier/chain-b" fw=36.4.4 state=complete; run set-next 1.2
expect "N2 without SystemFwVersions: the marker decides" "$(cat "$T/rc") $(writes | tr '\n' ' ')" "0 $(row N2 | tr '\n' ' ')"
installed; refused N10 "1.3 is not installed" set-next 1.3
installed; b_on 36.4.4
sed -i 's/^capsule=.*/capsule=/' "$R/data/fjarr-update/applier/slot-b"; refused N11 "carries none" set-next 1.2
installed; b_on 36.4.4
echo "a different capsule" > "$R/data/fjarr-update/stage/1.2/fjarr_1.2.cap"; refused N12 "changed since install" set-next 1.2
installed; b_on 36.4.4
echo 2360579 > "$R/sys/firmware/efi/esrt/entries/entry0/lowest_supported_fw_version"; refused N13 "below the lowest supported firmware 36.5.3" set-next 1.2
installed; var RootfsStatusSlotB "$NV" 7 4 255; refused N14 "slot b is marked unbootable" set-next 1.2
installed; pmcw rootfs_retry_count_b 0; refused N15 "no retries left" set-next 1.2
installed; kvw "$R/data/fjarr-update/applier/trial" version=1.2 slot=b previous=a boots=0 capsule=0 fw=36.5.2; refused N16 "a trial of 1.2 is in progress" set-next 1.2
installed; var BootChainFwStatus "$NV" 7 4 0; refused N17 "BootChainFwStatus is 0" set-next 1.2

# --- the trial counter (fjarr-trial.service) ---------------------------------------------------
trial() { # booted on slot B for the trial of 1.2: <boots> <capsule 0|1> [BootChainFwStatus]
  installed
  kvw "$R/data/fjarr-update/applier/trial" version=1.2 slot=b previous=a "boots=$1" "capsule=$2" fw=36.5.2
  rm -f "$R/run/fjarr-applier/verified"; on_slot b
  if [ -n "${3:-}" ]; then var BootChainFwStatus "$NV" 7 4 "$3"; fi
}
setup
case_run T1 0 trial-boot
trial 0 0 0
case_run T2 0 trial-boot
expect "T2: one trial boot counted" "$(kv "$R/data/fjarr-update/applier/trial" boots)" 1
trial 1 0 0
case_run T3 0 trial-boot
expect "T3: slot B retired" "$(varv RootfsStatusSlotB "$NV") $(pmc rootfs_retry_count_b)" "255 0x0"
trial 0 0 0; on_slot a
case_run T4 0 trial-boot
expect "T4: last-trial" "$(tr '\n' ' ' < "$R/data/fjarr-update/applier/last-trial")" \
  "version=1.2 slot=b previous=a boots=0 capsule=0 fw=36.5.2 result=fell-back "
trial 1 0 0; var RootfsStatusSlotA "$NV" 7 4 255
case_run T5 2 trial-boot
trial 1 0 0; touch "$T/fw/fail-set-active"
case_run T6 0 trial-boot

# --- mark-good --------------------------------------------------------------------------------
setup
case_run M1 0 mark-good
trial 1 0 0
case_run M2 0 mark-good
expect "M2: last-trial" "$(kv "$R/data/fjarr-update/applier/last-trial" result)" good
trial 1 1; echo 1 > "$T/fw/capsule_status"; kvw "$R/data/fjarr-update/applier/chain-b" fw=36.5.2 state=in-progress
case_run M3 0 mark-good
expect "M3: chain B complete" "$(tr '\n' ' ' < "$R/data/fjarr-update/applier/chain-b")" "fw=36.5.2 state=complete "
trial 1 1; echo 3 > "$T/fw/capsule_status"
case_run M4 1 mark-good
trial 1 1; echo 1 > "$T/fw/capsule_status"; echo v36.4.4 > "$R/sys/class/dmi/id/bios_version"
case_run M5 1 mark-good
trial 1 0 0; on_slot a
refused M10 "the trial of 1.2 is on slot b" mark-good
trial 1 1; echo 1 > "$T/fw/capsule_status"; echo v36.5.2 > "$R/sys/class/dmi/id/bios_version"
kvw "$R/data/fjarr-update/applier/chain-b" fw=36.5.2 state=in-progress
case_run M6 0 mark-good

# --- boot-verify (fjarr-boot-verify.service) ---------------------------------------------------
setup; rm "$R/run/fjarr-applier/verified"
case_run V1 0 boot-verify
trial 1 0
case_run V2 0 boot-verify
setup; rm "$R/run/fjarr-applier/verified" "$R/data/fjarr-update/applier/chain-a"; var BootChainFwStatus "$NV" 7 4 0
case_run V3 0 boot-verify
setup; rm "$R/run/fjarr-applier/verified"; var BootChainFwStatus "$NV" 7 4 12
case_run V4 0 boot-verify
expect "V4: an error status stays for a person" "$(varv BootChainFwStatus "$NV")" 12

# --- rollback-now -----------------------------------------------------------------------------
committed_b() { trial 1 0; rm "$R/data/fjarr-update/applier/trial"; }
committed_b
case_run R1 0 rollback-now
committed_b
case_run R2 0 rollback-now --mark-bad
trial 1 0 0
case_run R3 0 rollback-now
committed_b; var RootfsStatusSlotA "$NV" 7 4 255; refused R10 "slot a is marked unbootable" rollback-now
committed_b; kvw "$R/data/fjarr-update/applier/chain-a" fw=36.5.2 state=in-progress; refused R11 "half-written capsule" rollback-now
committed_b; var BootNext "$EFIG" 7 2 3; refused R12 "BootNext is set" rollback-now
committed_b; touch "$T/fw/fail-set-active"
case_run R4 1 rollback-now

# --- is-trial ---------------------------------------------------------------------------------
trial 1 0; run is-trial; expect "is-trial on the trial slot" "$(cat "$T/rc")" 0
trial 1 0; on_slot a; run is-trial; expect "is-trial elsewhere" "$(cat "$T/rc")" 1
setup; run is-trial; expect "is-trial without a trial" "$(cat "$T/rc")" 1

# --- End to end, through a model of the firmware -------------------------------------------------
# What the box does at a reset, as read in edk2-nvidia r36.4.4/r36.5.1 (BootChainDxe.c,
# L4TRootfsValidation.c) and NVIDIA's capsule documentation; the box measures it (D2, E1).
fwboot() { # <warm|cold>
  local cur nxt st osi s n o
  cur=$(cat "$T/fw/current")
  rm -rf "$R/run/fjarr-applier"; mkdir -p "$R/run/fjarr-applier"   # /run is a tmpfs
  [ "$1" = warm ] || pmcw rootfs_sr_magic 0                         # a power cut clears the scratch register
  # Capsule on disk: OsIndications bit 2 and TEGRA_BL.Cap. A pending chain switch cancels it.
  osi=$(varv OsIndications "$EFIG")
  if [ -n "$osi" ] && (((osi & 4) != 0)) && [ -e "$R/boot/efi/EFI/UpdateCapsule/TEGRA_BL.Cap" ]; then
    if [ -n "$(varv BootChainFwNext "$NV")" ] || [ -n "$(varv BootChainFwStatus "$NV")" ]; then
      var BootChainFwStatus "$NV" 7 4 3; unvar BootChainFwNext "$NV"; echo 3 > "$T/fw/capsule_status"
    else
      o=$((1 - cur)); sed 's/^capsule //' "$R/boot/efi/EFI/UpdateCapsule/TEGRA_BL.Cap" > "$T/fw/chain-$o"
      cur=$o; echo 1 > "$T/fw/capsule_status"
    fi
    rm -f "$R/boot/efi/EFI/UpdateCapsule/TEGRA_BL.Cap"; unvar OsIndications "$EFIG"
  fi
  # BootChainDxe: a requested switch. A status already there ends it without switching.
  nxt=$(varv BootChainFwNext "$NV"); st=$(varv BootChainFwStatus "$NV")
  if [ -n "$nxt" ]; then
    if [ -n "$st" ]; then unvar BootChainFwNext "$NV"
    elif [ -n "$(varv BootNext "$EFIG")" ]; then var BootChainFwStatus "$NV" 7 4 14; unvar BootChainFwNext "$NV"
    else cur=$nxt; var BootChainFwStatus "$NV" 7 4 0; unvar BootChainFwNext "$NV"; fi
  fi
  # L4TLauncher: seed the retry counters at a cold boot, then take one try from the slot.
  if [ "$(pmc rootfs_sr_magic)" != 0xface ]; then
    for s in a b; do
      if [ "$(varv "RootfsStatusSlot${s^^}" "$NV")" = 255 ]; then pmcw "rootfs_retry_count_$s" 0; else pmcw "rootfs_retry_count_$s" 3; fi
    done
    pmcw rootfs_sr_magic 0xface
  fi
  s=$([ "$cur" = 0 ] && echo a || echo b); o=$([ "$cur" = 0 ] && echo b || echo a)
  n=$(($(pmc "rootfs_retry_count_$s")))
  if ((n == 0)); then
    var "RootfsStatusSlot${s^^}" "$NV" 7 4 255
    if (($(pmc "rootfs_retry_count_$o") > 0)); then
      var BootChainFwNext "$NV" 7 4 $((1 - cur)); unvar BootChainFwStatus "$NV"; pmcw rootfs_sr_magic 0
      echo "$cur" > "$T/fw/current"; fwboot warm; return
    fi
    echo recovery > "$T/fw/booted"; return
  fi
  pmcw "rootfs_retry_count_$s" $((n - 1))
  echo "$cur" > "$T/fw/current"; on_slot "$s"; echo "$s" > "$T/fw/booted"
}
up() { # the units of a boot: fjarr-trial (early), then fjarr-boot-verify when not a trial
  : > "$LOG"
  PATH="$here/stubs:$PATH" "$applier" trial-boot > /dev/null 2>&1
  if grep -q '^systemctl:reboot' "$LOG"; then return 1; fi
  [ -e "$R/data/fjarr-update/applier/trial" ] || PATH="$here/stubs:$PATH" "$applier" boot-verify > /dev/null 2>&1
  return 0
}
A() { : > "$LOG"; PATH="$here/stubs:$PATH" "$applier" "$@" > "$T/out" 2>&1 || { bad "E2E: applier $* failed: $(cat "$T/out")"; return 1; }; }
booted() { cat "$T/fw/booted"; }
stage() { # <version> <fw>: a release in the cache, as the updater leaves it
  local s=$R/data/fjarr-update/stage/$1; mkdir -p "$s"
  echo "swupdate package $1" > "$s/fjarr_$1.swu"; echo "$2" > "$s/fjarr_$1.fw"; echo "capsule $2" > "$s/fjarr_$1.cap"
}

e2e() { setup; touch "$T/fw/model"; }   # nvbootctrl's two state changes, modelled (stubs/nvbootctrl)

# U1: an update without a capsule, confirmed, then rolled back on purpose.
e2e; fwboot cold; up
A install 1.2 "$R/data/fjarr-update/stage/1.2" && A set-next 1.2
fwboot warm; up
expect "U1: the trial boots slot B" "$(booted) $(kv "$R/data/fjarr-update/applier/trial" boots)" "b 1"
A mark-good
expect "U1: confirmed, the switch's status cleared" "$(kv "$R/data/fjarr-update/applier/last-trial" result) $(varv BootChainFwStatus "$NV")" "good "
fwboot warm; up
expect "U1: slot B stays, verified" "$(booted) $([ -e "$R/run/fjarr-applier/verified" ] && echo verified)" "b verified"
A rollback-now
fwboot warm; up
expect "U1: rollback-now boots slot A" "$(booted)" a

# U2: a failed trial, rolled back by its second boot; the next update must still go through.
e2e; fwboot cold; up
A install 1.2 "$R/data/fjarr-update/stage/1.2" && A set-next 1.2
fwboot warm; up
expect "U2: trial boot 1 on slot B" "$(booted)" b
fwboot warm   # health failed: the trial reboots
if up; then bad "U2: the second trial boot did not roll back"; else ok "U2: the second trial boot rolls back"; fi
fwboot warm; up
expect "U2: back on slot A, the trial over" "$(booted) $(kv "$R/data/fjarr-update/applier/last-trial" result)" "a fell-back"
stage 1.3 36.5.2
A install 1.3 "$R/data/fjarr-update/stage/1.3" && A set-next 1.3
fwboot warm; up
expect "U2: the next update boots despite the retired slot's counter" "$(booted) $(sed -n 's/^IMAGE_VERSION=//p' "$R/etc/os-release")" "b 1.3"

# U3: a capsule that moves chain B to 36.5.2 with slot B.
e2e; echo 36.4.4 > "$T/fw/chain-1"; kvw "$R/data/fjarr-update/applier/chain-b" fw=36.4.4 state=complete
fwboot cold; up
A install 1.2 "$R/data/fjarr-update/stage/1.2" && A set-next 1.2
fwboot warm; up
expect "U3: the capsule switched chain and slot" "$(booted) $(cat "$R/sys/class/dmi/id/bios_version") $(cat "$T/fw/capsule_status")" \
  "b 36.5.2-gcid-00000000 1"
A mark-good
expect "U3: chain B complete on 36.5.2" "$(tr '\n' ' ' < "$R/data/fjarr-update/applier/chain-b")" "fw=36.5.2 state=complete "

# U4: a power cut during the trial: a cold boot gives slot B its retries back, the counter does not.
e2e; fwboot cold; up
A install 1.2 "$R/data/fjarr-update/stage/1.2" && A set-next 1.2
fwboot warm; up
fwboot cold
if up; then bad "U4: the trial survived a power cut"; else ok "U4: a power cut during the trial rolls back (one try)"; fi
fwboot warm; up
expect "U4: back on slot A" "$(booted)" a

# U5: a power cut during set-next's capsule staging, before OsIndications: nothing applies.
e2e; echo 36.4.4 > "$T/fw/chain-1"; kvw "$R/data/fjarr-update/applier/chain-b" fw=36.4.4 state=complete
fwboot cold; up
A install 1.2 "$R/data/fjarr-update/stage/1.2"
kvw "$R/data/fjarr-update/applier/trial" version=1.2 slot=b previous=a boots=0 capsule=1 fw=36.5.2
kvw "$R/data/fjarr-update/applier/chain-b" fw=36.5.2 state=in-progress   # as set-next writes them first
fwboot cold; up
expect "U5: slot A boots and ends the trial" "$(booted) $(kv "$R/data/fjarr-update/applier/last-trial" result)" "a fell-back"
A set-next 1.2
expect "U5: the next set-next applies the capsule" "$(grep -c '^esp:EFI/UpdateCapsule/TEGRA_BL.Cap.tmp$' "$LOG")" 1

# U6: rollback-now from a committed slot while a switch's BootChainFwStatus is still set (before
# boot-verify cleared it): left set, BootChainDxe would end the switch without making it.
e2e; fwboot cold; up
A install 1.2 "$R/data/fjarr-update/stage/1.2" && A set-next 1.2
fwboot warm; up; A mark-good
var BootChainFwStatus "$NV" 7 4 0
A rollback-now
fwboot warm; up
expect "U6: rollback-now switches despite a stale status" "$(booted)" a

# U7: the second trial boot, and nvbootctrl fails to switch: the reboot still ends on slot A,
# because the retired slot's counter is 0 and L4TLauncher leaves it.
e2e; fwboot cold; up
A install 1.2 "$R/data/fjarr-update/stage/1.2" && A set-next 1.2
fwboot warm; up
touch "$T/fw/fail-set-active"; fwboot warm
if up; then bad "U7: the second trial boot did not reboot"; else ok "U7: the second trial boot reboots without nvbootctrl"; fi
rm -f "$T/fw/fail-set-active"; fwboot warm; up
expect "U7: L4TLauncher leaves the retired slot" "$(booted) $(kv "$R/data/fjarr-update/applier/last-trial" result)" "a fell-back"

echo
if [ "$fails" = 0 ]; then echo "all passed"; else echo "$fails failed"; exit 1; fi
