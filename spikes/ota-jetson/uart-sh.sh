#!/usr/bin/env bash
# shellcheck disable=SC2016 # the remote script takes its arguments as $1, $2 there
# uart-sh.sh: one command on the box through its debug UART console (slice 8.0), for a slot whose
# network does not come up.
# spec: docs/32-fleet-ota.md#jetson-test-image (L2); README.md, "The box's console"
#
#   uart-sh.sh [-t <seconds>] '<command>'
#
# Lab images log root in on the console (fjarr-spike-lab's autologin). The mini-PC types the
# command between two markers and prints what the box answers in between; kernel messages on the
# console are silenced first (dmesg -n 1). Nothing else may read the UART meanwhile: a console
# capture (lab_uart_start) would take part of the answer, so this refuses while one runs. Exit 0
# when the end marker came back, 1 when it did not within the time.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=lib/lab.sh
. "$here/lib/lab.sh"

tmo=60
if [[ ${1:-} == -t ]]; then tmo=$2; shift 2; fi
cmd=${1:?usage: uart-sh.sh [-t <seconds>] '<command>'}

_UART_SH_PY='import os, select, sys, termios, time
dev, cmd, tmo = sys.argv[1], sys.argv[2], float(sys.argv[3])
tag = "fjarr%d" % os.getpid()
fd = os.open(dev, os.O_RDWR | os.O_NOCTTY)
a = termios.tcgetattr(fd)
a[0] = a[1] = a[3] = 0
a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
a[4] = a[5] = termios.B115200
a[6][termios.VMIN], a[6][termios.VTIME] = 0, 0
termios.tcsetattr(fd, termios.TCSANOW, a)
termios.tcflush(fd, termios.TCIOFLUSH)
def send(s):
    for ch in s.encode():  # the console reads slowly: one byte at a time
        os.write(fd, bytes([ch]))
        time.sleep(0.002)
send("\r")
time.sleep(0.5)
# The box prints the markers from a printf format and the tag apart, so the echo of the typed
# line never holds a marker.
send("dmesg -n 1; printf \"%%s-BEGIN\\n\" %s; ( %s ) 2>&1; printf \"%%s-END %%s\\n\" %s $?\r" % (tag, cmd, tag))
buf, end = b"", time.monotonic() + tmo
while time.monotonic() < end:
    if select.select([fd], [], [], 0.5)[0]:
        buf += os.read(fd, 4096)
        text = buf.decode(errors="replace").replace("\r", "")
        b, e = text.rfind(tag + "-BEGIN\n"), text.rfind(tag + "-END ")
        if b >= 0 and e > b and "\n" in text[e:]:
            sys.stdout.write(text[b + len(tag) + 7:e])
            rc = text[e + len(tag) + 5:].split("\n")[0].strip()
            sys.stderr.write("uart-sh: exit %s\n" % rc)
            sys.exit(0)
sys.stdout.write(buf.decode(errors="replace").replace("\r", "")[-3000:])
sys.stderr.write("uart-sh: no end marker within %d s\n" % tmo)
sys.exit(1)'

lab_mpc_root -t $((tmo + 30)) 'if systemctl list-units --no-legend --state=active "fjarr-uart-*" | grep -q .; then
  echo "a console capture is running (fjarr-uart-*): it would take part of the answer" >&2; exit 2
fi
[ -c "$1" ] || { echo "no $1 on the mini-PC" >&2; exit 2; }
exec python3 -Ic "$4" "$1" "$2" "$3"' "$LAB_UART" "$cmd" "$tmo" "$_UART_SH_PY"
