#!/usr/bin/env bash
# fjarr-lab's tests (docs/12#lab-machines-and-fjarr-lab): the schedule, reservations and the graceful
# stop, against fake systemctl and pgrep, so no runner and no root are needed. `make lab-test`.
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
lab="$here/fjarr-lab"
fails=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }
expect() { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1: expected '$3', got '$2'"; fi; }
contains() { if grep -q -- "$3" <<<"$2"; then ok "$1"; else bad "$1: no '$3' in: $2"; fi; }

setup() {
  T=$(mktemp -d)
  mkdir -p "$T/bin" "$T/state" "$T/units"
  # systemctl: records actions; is-active reads $T/active; a job is $T/job, ending after $T/job_polls polls
  cat > "$T/bin/systemctl" <<'S'
#!/usr/bin/env bash
echo "systemctl $*" >> "$T/log"
case "$1" in
  is-active) [ -f "$T/active" ] ;;
  start) touch "$T/active" ;;
  stop) rm -f "$T/active" "$T/job" ;;
  *) : ;;
esac
S
  cat > "$T/bin/pgrep" <<'S'
#!/usr/bin/env bash
[ -f "$T/job" ] || exit 1
if [ -f "$T/job_polls" ]; then n=$(cat "$T/job_polls"); [ "$n" -le 0 ] && { rm -f "$T/job"; exit 1; }; echo $((n - 1)) > "$T/job_polls"; fi
exit 0
S
  chmod +x "$T/bin/systemctl" "$T/bin/pgrep"
  printf 'RUNNER_SERVICE="actions.runner.test.service"\nWINDOW="00:00-06:00"\nGRACE_MIN=30\nPOLL_S=0\n' > "$T/conf"
  export T PATH="$T/bin:$PATH" FJARR_LAB_CONF="$T/conf" FJARR_LAB_STATE="$T/state" FJARR_LAB_UNITS="$T/units" FJARR_LAB_NO_SYSTEMD=1
}
lab_at() { local t=$1; shift; FJARR_LAB_NOW=$t "$lab" "$@" 2>&1; }

setup
# The schedule: end exclusive, and a window may cross midnight.
expect "inside the window: online" "$(lab_at 03:00 desired)" online
expect "before it opens: offline" "$(lab_at 23:59 desired)" offline
expect "the closing minute is outside" "$(lab_at 06:00 desired)" offline
expect "the opening minute is inside" "$(lab_at 00:00 desired)" online
sed -i 's/^WINDOW=.*/WINDOW="22:00-06:00"/' "$T/conf"
expect "across midnight, late: online" "$(lab_at 23:00 desired)" online
expect "across midnight, early: online" "$(lab_at 05:59 desired)" online
expect "across midnight, midday: offline" "$(lab_at 12:00 desired)" offline
sed -i 's/^WINDOW=.*/WINDOW="off"/' "$T/conf"
expect "no window: online" "$(lab_at 12:00 desired)" online

# A reservation wins over the window, and release hands back to it.
sed -i 's/^WINDOW=.*/WINDOW="00:00-06:00"/' "$T/conf"
touch "$T/active"
out=$(SUDO_USER=erik lab_at 03:00 reserve)
contains "reserve inside the window takes the runner offline" "$out" "runner offline"
expect "reserved: offline even inside the window" "$(lab_at 03:00 desired)" offline
contains "status names who holds it" "$(lab_at 03:00 status)" "reserved by erik"
out=$(lab_at 03:00 release)
contains "release inside the window brings it back" "$out" "runner online"
out=$(lab_at 12:00 release)
contains "release outside the window leaves it offline" "$out" "runner offline"

# apply follows the clock.
rm -f "$T/active"; : > "$T/log"
lab_at 01:00 apply >/dev/null
contains "apply inside the window starts the runner" "$(cat "$T/log")" "systemctl start actions.runner.test.service"
lab_at 07:00 apply >/dev/null
contains "apply outside the window stops it" "$(cat "$T/log")" "systemctl stop actions.runner.test.service"

# Never a job killed halfway: wait for it, then stop.
touch "$T/active" "$T/job"; echo 3 > "$T/job_polls"; : > "$T/log"
out=$(lab_at 07:00 apply)
contains "a running job is waited for" "$out" "a job is running: waiting"
contains "and the runner stops once it is idle" "$out" "runner offline"
if grep -q 'cancels it' <<<"$out"; then bad "an ending job was not cancelled"; else ok "an ending job was not cancelled"; fi

# ...unless the grace period runs out.
sed -i 's/^GRACE_MIN=.*/GRACE_MIN=0/' "$T/conf"
touch "$T/active" "$T/job"; rm -f "$T/job_polls"
out=$(lab_at 07:00 apply)
contains "past the grace period the job is cancelled" "$out" "which cancels it"

# The window command moves the timers and the config, and refuses nonsense.
sed -i 's/^GRACE_MIN=.*/GRACE_MIN=30/' "$T/conf"
lab_at 12:00 window 01:30-05:00 >/dev/null
contains "window writes the open timer" "$(cat "$T/units/fjarr-lab-open.timer.d/window.conf")" "OnCalendar=\*-\*-\* 01:30:00"
contains "and the close timer" "$(cat "$T/units/fjarr-lab-close.timer.d/window.conf")" "OnCalendar=\*-\*-\* 05:00:00"
contains "and the config" "$(cat "$T/conf")" 'WINDOW="01:30-05:00"'
out=$(lab_at 12:00 window 25:00-06:00); rc=$?
if [ $rc -ne 0 ] && grep -q "not a window" <<<"$out"; then ok "an impossible window is refused"; else bad "an impossible window was accepted: $out"; fi
lab_at 12:00 window off >/dev/null
if [ ! -f "$T/units/fjarr-lab-open.timer.d/window.conf" ]; then ok "window off removes the timers"; else bad "window off left a timer"; fi

# reboot: a lab job asks and ends; the reboot comes once no job runs, with the runner taken offline first.
: > "$T/log"
touch "$T/active" "$T/job"; echo 2 > "$T/job_polls"
out=$(FJARR_LAB_FOREGROUND=1 FJARR_LAB_REBOOT_SETTLE=0 lab_at 00:20 reboot)
contains "reboot waits for the running job, then reboots" "$out" "runner idle"
expect "the runner goes offline before the reboot" "$(grep -E 'systemctl (stop|reboot)' "$T/log" | tr '\n' '|')" "systemctl stop actions.runner.test.service|systemctl reboot|"

# reboot asked by a job: waits for that job's process, not for "no job", then offline at once.
: > "$T/log"
touch "$T/active"
sleep 2 & job_pid=$!
out=$(FJARR_LAB_FOREGROUND=1 FJARR_LAB_WAIT_PID=$job_pid lab_at 00:20 reboot)
contains "reboot waits for the asking job's own process" "$out" "the job has ended"
if kill -0 "$job_pid" 2>/dev/null; then bad "reboot went ahead while the job's process was alive"; else ok "the job's process had ended before the reboot"; fi
expect "and the runner goes offline before the reboot" "$(grep -E 'systemctl (stop|reboot)' "$T/log" | tr '\n' '|')" "systemctl stop actions.runner.test.service|systemctl reboot|"

# unattended.sh report: the verdict counts only for the prepare run the workflow asks about. When a
# night's prepare failed before reaching the machine, the state was an older night's, and its
# "passed" was once reported as that night's (2026-10-03).
U="$T/unattended"; mkdir -p "$U/unattended"
echo "not-this-boot" > "$U/unattended/boot-id" # a reboot since prepare
echo 1111 > "$U/unattended/prepare-run"
echo passed > "$U/unattended/result"
rep() { FJARR_LAB_STATE="$U" PATH="$T/bin:$PATH" bash "$(dirname "$0")/unattended.sh" report "$@" 2>&1; echo "rc=$?"; }
out=$(rep --prepare-run 1111)
contains "report passes on the verdict of the prepare it was asked about" "$out" "rc=0"
out=$(rep --prepare-run 2222)
contains "report refuses an older night's verdict" "$out" "rc=1"
contains "and says that prepare never reached the machine" "$out" "last prepared by run 1111, not run 2222"
echo failed > "$U/unattended/result"
contains "report fails on a failed verdict" "$(rep --prepare-run 1111)" "rc=1"
cat /proc/sys/kernel/random/boot_id > "$U/unattended/boot-id"
contains "report says 'not yet' before the reboot" "$(rep --prepare-run 1111)" "rc=75"

rm -rf "$T"
echo "lab-test: $([ $fails -eq 0 ] && echo 'all passed' || echo "$fails failed")"
exit $((fails > 0))
