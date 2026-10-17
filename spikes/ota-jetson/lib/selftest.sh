#!/usr/bin/env bash
# lib/selftest.sh: offline tests of lib/lab.sh; nothing in the lab is touched.
# spec: docs/32-fleet-ota.md#jetson-test-image (L1, L7, L8: the helpers the lab scripts rely on)
#
#   lib/selftest.sh
#
# A mock of the plug's RPC (Switch.GetStatus, Switch.Set with toggle_after; HTTP digest auth with
# SHA-256, as the plug asks for, so the credentials must reach both of curl's requests) runs on 127.0.0.1 and
# drives a fake sysfs of the mini-PC's USB: the box comes and goes on a controller of its own, next
# to another controller with another device. The power cycle, leaving RCM and the USB isolation
# run for real against them. The lock runs in its local mode (as on the mini-PC) on a temporary
# file; a fake sudo runs as the caller. Needs python3, curl, jq and setsid.
# shellcheck disable=SC2016 # the single-quoted code runs in a shell that sources the lib
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
t=$(mktemp -d)
mock=''
cleanup() {
  [[ -n $mock ]] && kill "$mock" 2>/dev/null
  rm -rf "$t"
}
trap cleanup EXIT
pass=0 fail=0
ok() {
  echo "PASS $1"
  pass=$((pass + 1))
}
bad() {
  echo "FAIL $1"
  fail=$((fail + 1))
}

# Run the lib with everything pointed at the temporary directory. The clock reads 12:00 unless
# the test passes another local hour, so the window never interferes by accident.
port=$((20000 + RANDOM % 20000))
tz_at() { # <hour>: a POSIX TZ in which it is about <hour>:mm now
  local off=$((10#$(date -u +%H) - $1))
  ((off > 12)) && off=$((off - 24))
  ((off < -12)) && off=$((off + 24))
  printf 'LAB%+d' "$off"
}
labenv() { # <local hour>: the environment of a lab run
  printf '%s\n' PATH="$t/bin:$PATH" HOME="$t" LAB_DIR="$t/lab" LAB_LOCAL=1 LAB_LOCK_FILE="$t/lock" \
    LAB_BOX_MAC=02:00:00:00:00:01 LAB_PLUG="127.0.0.1:$port" LAB_PLUG_PASS_FILE="$t/pass" \
    LAB_TZ="$(tz_at "$1")" LAB_OFF_SECONDS=2 LAB_RCM_GONE_S=3 XDG_RUNTIME_DIR="$t/run" \
    LAB_USB_SYSFS="$t/usbdev" LAB_USB_RESTORE="$t/usb.restore" LAB_DESKTOP_OK="${DESKTOP_OK:-}" \
    LAB_DESKTOP_ISOLATE="${DESKTOP_ISOLATE-1}" LAB_USB_CTL=c1
}
lab() { # <local hour> <bash code using the lib>
  local -a e
  mapfile -t e < <(labenv "$1")
  env -i "${e[@]}" bash -c 'set -euo pipefail; . "$1"; '"$2" lab "$here/lib/lab.sh"
}

# The fake mini-PC: sudo, a desktop nobody is logged in to, and its USB in sysfs. Controller c1
# (usb1, usb2) carries only the box at 1-1; controller c2 (usb3) carries another device at 3-4.
mkdir -p "$t/bin" "$t/lab" "$t/run" "$t/usbdev"
printf 'not-a-real-password' >"$t/pass"
cat >"$t/bin/sudo" <<'EOF'
#!/bin/sh
while [ $# -gt 0 ]; do case $1 in -n | -H) shift ;; -u) shift 2 ;; *) break ;; esac; done
exec "$@"
EOF
printf '#!/bin/sh\nexit 0\n' >"$t/bin/loginctl"
chmod +x "$t/bin/sudo" "$t/bin/loginctl"
for h in c1/usb1 c1/usb2 c2/usb3; do
  mkdir -p "$t/sys/$h"
  echo 1 >"$t/sys/$h/authorized_default"
  ln -s "../sys/$h" "$t/usbdev/${h#*/}"
done
mkdir -p "$t/sys/c2/usb3/3-4"
printf '%s\n' 0bda >"$t/sys/c2/usb3/3-4/idVendor"
printf '%s\n' 0129 >"$t/sys/c2/usb3/3-4/idProduct"
echo 3 >"$t/sys/c2/usb3/3-4/busnum"
echo 1 >"$t/sys/c2/usb3/3-4/authorized"
ln -s ../sys/c2/usb3/3-4 "$t/usbdev/3-4"

python3 -I - "$port" "$t/sys" "$t/usbdev" "$t/mode" "$t/pass" <<'PY' &
import hashlib, http.server, json, os, re, shutil, sys, threading, urllib.parse, uuid
port, sysroot, links, modefile = int(sys.argv[1]), sys.argv[2], sys.argv[3], sys.argv[4]
password = open(sys.argv[5]).read().strip()
state = {"output": True}
REALM, NONCE = "shelly-mock", "0123456789abcdef"
def sha(text):
    return hashlib.sha256(text.encode()).hexdigest()
def authorized(header, method):  # HTTP digest, SHA-256 and qop auth, as the Gen3 plug asks for
    if not header.startswith("Digest "):
        return False
    f = dict(re.findall(r'(\w+)="?([^",]*)"?', header[7:]))
    ha1, ha2 = sha(f"admin:{REALM}:{password}"), sha(f"{method}:{f.get('uri')}")
    want = sha(f"{ha1}:{NONCE}:{f.get('nc')}:{f.get('cnonce')}:{f.get('qop')}:{ha2}")
    return f.get("username") == "admin" and f.get("response") == want
def mode():  # flags: noflip (the plug's timer never fires), stayrcm (the box stays in RCM), dead (nothing boots)
    try:
        return open(modefile).read().split()
    except OSError:
        return []
def box(what):  # the box on the fake USB: rcm | l4t | none; a new device takes the hub's default
    d, link = os.path.join(sysroot, "c1/usb1/1-1"), os.path.join(links, "1-1")
    if os.path.islink(link):
        os.unlink(link)
    shutil.rmtree(d, ignore_errors=True)
    if what == "none":
        return
    os.makedirs(d)
    auth = open(os.path.join(sysroot, "c1/usb1/authorized_default")).read().strip()
    for k, v in (("idVendor", "0955"), ("idProduct", "7323" if what == "rcm" else "7020"),
                 ("busnum", "1"), ("authorized", "0" if auth == "0" else "1"), ("bootid", str(uuid.uuid4()))):
        open(os.path.join(d, k), "w").write(v + "\n")  # bootid: the fake box's, one per boot
    os.symlink(os.path.relpath(d, links), link)
def power(on):
    state["output"] = on
    box(("none" if "dead" in mode() else "rcm" if "stayrcm" in mode() else "l4t") if on else "none")
class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        q, m = dict(urllib.parse.parse_qsl(u.query)), u.path.rsplit("/", 1)[-1]
        if m != "Test.Box" and not authorized(self.headers.get("Authorization", ""), "GET"):
            self.send_response(401)
            self.send_header("WWW-Authenticate", f'Digest qop="auth", realm="{REALM}", nonce="{NONCE}", algorithm=SHA-256')
            self.send_header("Content-Length", "0"); self.end_headers(); return
        if m == "Switch.GetStatus":
            body = {"id": 0, "output": state["output"], "apower": 9.1 if state["output"] else 0.0}
        elif m == "Switch.Set":
            body, on = {"was_on": state["output"]}, q.get("on") == "true"
            power(on)
            if "toggle_after" in q and "noflip" not in mode():
                threading.Timer(float(q["toggle_after"]), power, [not on]).start()
        elif m == "Test.Box":  # the test's own hand on the fake USB
            box(q["state"])
            body = {}
        else:
            self.send_response(404); self.end_headers(); return
        data = json.dumps(body).encode()
        self.send_response(200); self.send_header("Content-Length", str(len(data))); self.end_headers()
        self.wfile.write(data)
    def log_message(self, *args):
        pass
http.server.ThreadingHTTPServer(("127.0.0.1", port), H).serve_forever()
PY
mock=$!
for _ in $(seq 1 50); do curl -s -o /dev/null "http://127.0.0.1:$port/rpc/Test.Box?state=l4t" && break; sleep 0.1; done
box() { curl -s -o /dev/null "http://127.0.0.1:$port/rpc/Test.Box?state=$1"; }
hubs() { cat "$t/sys/c1/usb1/authorized_default" "$t/sys/c1/usb2/authorized_default" | paste -sd, -; }
box l4t

expect() { # <ok|fail> <name> <pattern> <local hour> <code>: the run's status and output
  local want=$1 name=$2 pattern=$3 out rc=0
  out=$(lab "$4" "$5" 2>&1) || rc=$?
  if [[ ($want == ok && $rc -eq 0) || ($want == fail && $rc -ne 0) ]] && [[ $out == *$pattern* ]]; then
    ok "$name"
  else
    bad "$name (exit $rc): $out"
  fi
}

# L1: the window and the hard stop (23:00 with the default margin)
expect fail "window: refused at 03:00" "inside the nightly window" 3 'lab_init w 60'
expect fail "hard stop: refused at 23:xx" "before the hard stop at 23:00" 23 'lab_init w 60'
expect fail "hard stop: refused a run that would pass 23:00" "the run needs up to 120 min" 21 'lab_init w 7200'
expect ok "hard stop: a short run at 21:xx" "== w: end (exit 0)" 21 'lab_init w 600'
expect ok "window: ran at 12:00" "hard stop 23:00" 12 'lab_init w 60'
expect fail "hard stop: an action refused when its worst case would pass it" "a power cycle needs up to" 12 \
  'lab_init w 60; LAB_DEADLINE=$(($(date +%s) + 60)); lab_power_cycle'
if out=$(env -i PATH="$PATH" HOME="$t" LAB_DIR="$t/lab" LAB_TZ=Europe/Nowhere bash -c '. "$1"; lab_init tz 60' lab "$here/lib/lab.sh" 2>&1); then
  bad "time zone: an unknown zone accepted"
elif [[ $out == *"no such time zone"* ]]; then
  ok "time zone: an unknown zone refused"
else
  bad "time zone: $out"
fi

# L1: the lock, as on the mini-PC
lab 12 'lab_init holder 60; sleep 10' >/dev/null 2>&1 &
holder=$!
sleep 1
expect fail "lock: a second run refused, the holder named" "held by another run: *lab_init holder" 12 'lab_init second 60'
wait "$holder"
expect ok "lock: free again after the holder ended" "lab lock held here" 12 'lab_init third 60'

# The mini-PC's USB, and the box's controller isolated for a run (the desktop guard)
expect ok "USB: the box seen running L4T" "usb=l4t" 12 'lab_init u 60; lab_usb_poll; echo "usb=$LAB_USB_STATE"'
DESKTOP_ISOLATE='' expect fail "desktop guard: refused without a person's choice" "A person decides" 12 \
  'lab_init choice 60; lab_desktop_guard'
if [[ $(hubs) == 1,1 && ! -e $t/usb.restore ]]; then ok "desktop guard: nothing changed by the refusal"; else bad "desktop guard: hubs=$(hubs) after a refusal"; fi
expect ok "USB isolation: both hubs of the box's controller, the box deauthorized, still seen" \
  "hubs=0,0 box=0 usb=l4t restore=yes" 12 \
  'lab_init iso 60; lab_desktop_guard; lab_usb_poll
   echo "hubs=$(cat "$LAB_USB_SYSFS"/usb1/authorized_default "$LAB_USB_SYSFS"/usb2/authorized_default | paste -sd, -) box=$(cat "$LAB_USB_SYSFS"/1-1/authorized) usb=$LAB_USB_STATE restore=$([[ -e $LAB_USB_RESTORE ]] && echo yes)"'
if [[ $(hubs) == 1,1 && ! -e $t/usb.restore && $(cat "$t/sys/c2/usb3/authorized_default") == 1 ]]; then
  ok "USB isolation: undone at the end of the run, the other controller untouched"
else
  bad "USB isolation: after the run hubs=$(hubs), restore file $(ls "$t/usb.restore" 2>&1)"
fi
box l4t
# A run that died without its cleanup: the hubs still at 0, the saved values in the restore file.
echo 0 >"$t/sys/c1/usb1/authorized_default"
echo 0 >"$t/sys/c1/usb2/authorized_default"
printf '%s 1\n%s 1\n' "$t/usbdev/usb1/authorized_default" "$t/usbdev/usb2/authorized_default" >"$t/usb.restore"
# The box is on USB but authorized (it enumerated before the isolation): the leftover stays.
expect ok "USB isolation: a leftover kept while the box is authorized" "USB isolation kept: the box is at 1-1 with authorized=1" 12 'lab_init left 60'
if [[ -e $t/usb.restore && $(hubs) == 0,0 ]]; then ok "USB isolation: the kept leftover still in place"; else bad "USB isolation: kept leftover hubs=$(hubs)"; fi
echo 0 >"$t/sys/c1/usb1/1-1/authorized"
expect ok "USB isolation: a previous run's leftover undone when the lock is taken" "USB isolation undone" 12 'lab_init left 60'
if [[ ! -e $t/usb.restore && $(hubs) == 1,1 ]]; then
  ok "USB isolation: the leftover's saved values written back, its restore file gone"
else
  bad "USB isolation: after the leftover hubs=$(hubs), restore file $(ls "$t/usb.restore" 2>&1)"
fi
mkdir -p "$t/sys/c1/usb1/1-2"
printf '%s\n' 046d >"$t/sys/c1/usb1/1-2/idVendor"
printf '%s\n' c52b >"$t/sys/c1/usb1/1-2/idProduct"
ln -s ../sys/c1/usb1/1-2 "$t/usbdev/1-2"
expect fail "USB isolation: refused when another device shares the box's controller" "another device (1-2, 046d:c52b)" 12 \
  'lab_init share 60; lab_power_cycle'
DESKTOP_OK=1 expect ok "USB isolation: LAB_DESKTOP_OK=1 skips it" "power cycle: on again" 12 'lab_init share 60; lab_power_cycle'
rm "$t/usbdev/1-2"
rm -r "$t/sys/c1/usb1/1-2"
if [[ $(hubs) == 1,1 ]]; then ok "USB isolation: nothing changed by the refused run"; else bad "USB isolation: hubs=$(hubs) after a refusal"; fi

# A power cycle of a box that shows nothing on USB: its controller (LAB_USB_CTL) is isolated, and
# the isolation outlives a run that ends before the box is back and deauthorized.
box none
echo dead >"$t/mode"
expect ok "USB isolation: the box not on USB, its controller isolated for a power cycle" "the box not on USB now*power cycle: on again" 12 \
  'lab_init cyc 60; lab_power_cycle'
if [[ -e $t/usb.restore && $(hubs) == 0,0 ]] && grep -q 'USB isolation kept: the box is not on USB' "$t/lab/logs/"*-cyc.log; then
  ok "USB isolation: kept at the end of a run whose box is not back"
else
  bad "USB isolation: after a power cycle with the box absent hubs=$(hubs), restore $(ls "$t/usb.restore" 2>&1)"
fi
: >"$t/mode"
box l4t # enumerates under the kept isolation: deauthorized
expect ok "USB isolation: lifted by the next run once the box is back, deauthorized" "USB isolation undone" 12 'lab_init next 60'
if [[ ! -e $t/usb.restore && $(hubs) == 1,1 ]]; then ok "USB isolation: the next run restored the hubs"; else bad "USB isolation: next run hubs=$(hubs)"; fi
box l4t

# The end of a run, however it ends: SIGINT to the whole process group, as a Ctrl-C sends it
mapfile -t envs < <(labenv 12)
box l4t
# A background job of this script starts with SIGINT ignored, which a script cannot undo: Python
# puts the default back before the run starts in its own process group, as a terminal's would be.
env -i "${envs[@]}" python3 -I -c 'import os, signal, sys
signal.signal(signal.SIGINT, signal.SIG_DFL)
os.execvp(sys.argv[1], sys.argv[1:])' setsid -w bash -c 'set -euo pipefail; . "$1"; lab_init sig 60; lab_desktop_guard
  echo "$$ $LAB_RUN" >"$2.tmp"; mv "$2.tmp" "$2"; sleep 30' lab "$here/lib/lab.sh" "$t/sig" >"$t/sig.out" 2>&1 &
sigjob=$!
for _ in $(seq 1 100); do [[ -s $t/sig ]] && break; sleep 0.1; done
read -r sigpid sigrun <"$t/sig" || true
kill -INT -- "-$sigpid" 2>/dev/null || true
wait "$sigjob" || true
if [[ -n $sigrun && ! -e $sigrun && $(hubs) == 1,1 && ! -e $t/usb.restore ]] &&
  grep -q 'USB isolation undone' "$t/lab/logs/"*-sig.log && tail -n 1 "$t/lab/logs/"*-sig.log | grep -q 'end (exit 130)'; then
  ok "Ctrl-C: the run's files removed, the USB isolation undone, the log complete"
else
  bad "Ctrl-C: run dir $(ls -d "$sigrun" 2>&1), hubs=$(hubs), log: $(tail -n 3 "$t/lab/logs/"*-sig.log 2>&1)"
fi
if grep -rqF 'not-a-real-password' "$t/lab" "$t/run"; then bad "plug password: written to a file"; else ok "plug password: in no file"; fi

# L8 and L7: the power cycle and leaving RCM, against the mock plug
expect ok "plug: digest auth with the password from its file" "output=true" 12 \
  'lab_init plug 60; lab_plug_status; echo "output=$LAB_PLUG_OUTPUT"'
expect fail "plug: a wrong password gets no answer" "output=unknown" 12 \
  'lab_init plug 60; LAB_PLUG_PASS=a-wrong-password; lab_plug_status || { echo "output=$LAB_PLUG_OUTPUT"; exit 1; }'
box l4t
expect ok "power cycle: off, back on by the plug's timer" "off=0* on=true" 12 \
  'lab_init cycle 60; lab_power_cycle; echo "off=$LAB_CYCLE_OFF_APOWER on=$LAB_PLUG_OUTPUT"'
echo noflip >"$t/mode"
expect ok "power cycle: switched on when the plug's timer did not" "did not switch on: switching on*on=true" 12 \
  'lab_init cycle 60; lab_power_cycle; echo "on=$LAB_PLUG_OUTPUT"'
: >"$t/mode"
lab 12 'lab_init cycle 60; lab_plug "Switch.Set?id=0&on=false"' >/dev/null 2>&1
expect fail "power cycle: refused with the plug off" "already off" 12 'lab_init cycle 60; LAB_DESKTOP_OK=1; lab_power_cycle'
lab 12 'lab_init cycle 60; lab_plug "Switch.Set?id=0&on=true"' >/dev/null 2>&1
box rcm
expect ok "RCM: left by a power cycle" "usb=l4t" 12 'lab_init leave 60; lab_rcm_leave; lab_usb_poll; echo "usb=$LAB_USB_STATE"'
echo stayrcm >"$t/mode"
box rcm
expect fail "RCM: a box back in RCM after the power cycle stops the run" "in RCM again" 12 'lab_init leave 60; lab_rcm_leave'
: >"$t/mode"
box none
SECONDS=0
expect fail "RCM wait: no RCM, so it gives up" "" 12 'lab_init wait 60; lab_rcm_wait 4'
if ((SECONDS >= 4)); then ok "RCM wait: only after its timeout"; else bad "RCM wait: returned after ${SECONDS} s"; fi
box l4t
expect fail "L7: lab_die runs the script's hook, and a lab_die inside it just stops" "STOP: again" 12 \
  'lab_init hook 60; hook() { echo "hook: $1" >>"$HOME/hook.txt"; lab_die "again"; }; LAB_DIE_HOOK=hook; lab_die "the reason"'
if [[ $(cat "$t/hook.txt" 2>/dev/null) == "hook: the reason" ]]; then ok "L7: the hook ran once, with the reason"; else bad "L7: hook calls: $(cat "$t/hook.txt" 2>&1)"; fi

# rcm.sh against a simulated box: a fake ssh answers as the box while the fake USB shows it
# running L4T (with a boot id per boot), and ping and ip find it by its MAC then. Its reboot into
# RCM moves the fake USB to RCM. rcm.sh and the lib run from a copy, so their results stay in $t.
mkdir -p "$t/boxbin" "$t/spike/lib" "$t/spike/results" "$t/fake" "$t/lab/state" "$t/lab/zedbox-backup"
cp "$here/rcm.sh" "$t/spike/"
cp "$here/lib/lab.sh" "$t/spike/lib/"
echo 192.168.10.115 >"$t/lab/state/box.ip"
echo a-file >"$t/lab/zedbox-backup/a-file"
(cd "$t/lab/zedbox-backup" && sha256sum a-file >SHA256SUMS && echo "t $(sha256sum <SHA256SUMS | cut -d' ' -f1) x" >SHA256SUMS.verified)
cat >"$t/boxbin/ssh" <<'EOF'
#!/bin/bash
cmd=${!#}
dev=$FAKE_USB/1-1
[[ -d $dev && $(cat "$dev/idProduct" 2>/dev/null) == 7020 ]] || { echo "ssh: connect to host port 22: No route to host" >&2; exit 255; }
[[ -e $FAKE_DIR/hostkey ]] && { echo "Host key verification failed." >&2; exit 255; }
case $cmd in
  *random/boot_id*) cat "$dev/bootid" ;;
  *is-system-running*) cat "$FAKE_DIR/state" 2>/dev/null || echo running ;;
  *"systemctl --failed"*) echo ;;
  *reset_reason*) echo SYS_RESET_N ;;
  *"sudo -n sync"*) exit 0 ;;
  *"reboot --force forced-recovery"*)
    [[ -e $FAKE_DIR/on-reboot ]] && bash "$FAKE_DIR/on-reboot" # the test's change that comes with the reboot
    if [[ -e $FAKE_DIR/no-rcm ]]; then
      (sleep 1; curl -s -o /dev/null "$FAKE_PLUG/rpc/Test.Box?state=none"; sleep 2; curl -s -o /dev/null "$FAKE_PLUG/rpc/Test.Box?state=l4t") >/dev/null 2>&1 &
    else
      (sleep 1; curl -s -o /dev/null "$FAKE_PLUG/rpc/Test.Box?state=rcm") >/dev/null 2>&1 &
    fi
    exit 255 ;;
  *nvbootctrl*) printf 'Current version: 36.4.4\nCurrent bootloader slot: A\nRootfsStatusSlotA 07 00 00 00 00 00 00 00\n'
    cat "$FAKE_DIR/slots" 2>/dev/null; exit 0 ;;
  *) echo "fake box: no answer for: $cmd" >&2; exit 1 ;;
esac
EOF
cat >"$t/boxbin/ping" <<'EOF'
#!/bin/bash
[[ ${!#} == 192.168.10.115 && -d $FAKE_USB/1-1 && $(cat "$FAKE_USB/1-1/idProduct") == 7020 ]]
EOF
cat >"$t/boxbin/ip" <<'EOF'
#!/bin/bash
if [[ $1 == neigh && -d $FAKE_USB/1-1 && $(cat "$FAKE_USB/1-1/idProduct") == 7020 ]]; then
  echo "192.168.10.115 dev eth0 lladdr 02:00:00:00:00:01 REACHABLE"
fi
EOF
chmod +x "$t/boxbin/ssh" "$t/boxbin/ping" "$t/boxbin/ip"
rcmsim() { # <name> <ok|fail> <pattern of the log> <pattern of the csv> [kept] <rcm.sh args...>: one simulated run
  # kept: the run ends with the box not back, so the USB isolation must outlive it (then reset here)
  local name=$1 want=$2 logpat=$3 csvpat=$4 rc=0 log csv iso=undone
  shift 4
  [[ $1 == kept ]] && iso=kept && shift
  local -a e
  mapfile -t e < <(labenv 12)
  rm -f "$t/spike/results/"*.csv
  log=$(env -i "${e[@]}" PATH="$t/boxbin:$t/bin:$PATH" FAKE_USB="$t/usbdev" FAKE_DIR="$t/fake" \
    FAKE_PLUG="http://127.0.0.1:$port" LAB_RCM_TMO=6 LAB_BACK_TMO=10 bash "$t/spike/rcm.sh" "$@" 2>&1) || rc=$?
  csv=$(tail -n +2 "$t/spike/results/"*-rcm.csv 2>/dev/null | cut -d, -f2,13 | paste -sd' ' -)
  if [[ ($want == ok && $rc -eq 0) || ($want == fail && $rc -ne 0) ]] && [[ $log == *$logpat* && $csv == "$csvpat" ]] &&
    { [[ $iso == undone && $(hubs) == 1,1 && ! -e $t/usb.restore ]] ||
      [[ $iso == kept && $(hubs) == 0,0 && -e $t/usb.restore ]]; }; then
    ok "rcm.sh: $name"
  else
    bad "rcm.sh: $name (exit $rc, csv '$csv', hubs $(hubs)): $(tail -n 12 <<<"$log")"
  fi
}
: >"$t/mode"
box l4t
rcmsim "two round trips" ok "2 of 2 round trips" "1,ok 2,ok" 2
mv "$t/lab/zedbox-backup/SHA256SUMS.verified" "$t/sha.v"
rcmsim "refused without a verified backup (B3 before B4)" fail "no verified backup" "" 1
mv "$t/sha.v" "$t/lab/zedbox-backup/SHA256SUMS.verified"
touch "$t/fake/no-rcm"
rcmsim "no RCM: stop, the line written" fail "no RCM (0955:7323) within 6 s" "1,no-rcm" 1
rm "$t/fake/no-rcm"
box l4t
echo stayrcm >"$t/mode"
rcmsim "RCM again after the power cycle: stop" fail "in RCM again" "1,rcm-leave" 1
: >"$t/mode"
box l4t
echo dead >"$t/mode"
rcmsim "nothing back: one more power cycle, then stop" fail "one more power cycle" "1,not-back" kept 1
: >"$t/mode"
echo 1 >"$t/sys/c1/usb1/authorized_default"
echo 1 >"$t/sys/c1/usb2/authorized_default"
rm -f "$t/usb.restore"
box l4t
echo "touch '$t/fake/hostkey'" >"$t/fake/on-reboot"
rcmsim "another host key on the box's MAC: stop" fail "fails the pinned host key" "1,host-key" 1
rm -f "$t/fake/hostkey" "$t/fake/on-reboot"
box l4t
echo "echo 'BootChainFwStatus 07 00 00 00 01 00 00 00' >'$t/fake/slots'" >"$t/fake/on-reboot"
rcmsim "the slot state changed: stop" fail "the slot state changed in trip 1" "1,slots-changed" 1
rm -f "$t/fake/slots" "$t/fake/on-reboot"
box l4t

# Committed results: the redaction and the leak check
cat >"$t/secrets" <<'EOF'
hostname box-test-QX7RZ9
hostname QX7RZ9
mac 02:11:22:33:44:55
serial 123456789012345678
serial 0a 0b 0c 0d 0e 0f 10 11
uuid 0a1b2c3d-1111-2222-3333-444455556666
id 00112233445566778899aabbccddeeff
EOF
cat >"$t/dump" <<'EOF'
hostname BOX-TEST-qx7rz9 booted
Boot0001* UEFI PCIe SSD 123456789012345678 1 NVMe(0x1,0A-0B-0C-0D-0E-0F-10-11) MAC(021122334455,0)
root=PARTUUID=0A1B2C3D-1111-2222-3333-444455556666 machine 00112233445566778899aabbccddeeff
another MAC aa:bb:cc:dd:ee:ff, enx021122334466, fe80::11:22ff:fe33:4455/64 and 10:45:14 stays
Serial Number: XYZ-0001
DHCP4.OPTION[8]:                        host_name = lease-name-K4W2
DHCP4.OPTION[15]:                       requested_host_name = 1
     Static hostname: static-name-K4W2
EOF
lab 12 "lab_init redact 60 nolock; lab_redact '$t/secrets' <'$t/dump' >'$t/out'" >/dev/null 2>&1
if ! grep -qiE 'qx7rz9|123456789012345678|0a-0b-0c-0d|0a1b2c3d|00112233445566778899|aa:bb:cc|021122334466|ff:fe33|XYZ-0001|K4W2' "$t/out" &&
  grep -q '10:45:14 stays' "$t/out" && grep -q 'requested_host_name = 1' "$t/out"; then
  ok "redaction: every identifier and host-name field replaced, the time and other fields kept"
else
  bad "redaction: $(cat "$t/out")"
fi
expect ok "leak check: a clean result passes" "== leak: end (exit 0)" 12 "lab_init leak 60 nolock; lab_leak_check '$t/out' '$t/secrets'"
echo "leftover QX7RZ9" >>"$t/out"
expect fail "leak check: a surviving identifier is caught" "LEAK" 12 "lab_init leak 60 nolock; lab_leak_check '$t/out' '$t/secrets'"

echo "$pass passed, $fail failed"
((fail == 0))
