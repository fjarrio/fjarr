#!/bin/bash
# net-rescue.sh as root on a stand-in for the mini-PC (Dockerfile.minipc: Ubuntu 26.04 booted with
# systemd, ufw active, no dnsmasq): install refused at 03:00 and 06:15 (the nightly window and its
# grace), then install, which also installs dnsmasq-base and opens ufw;
# --status; --undo refused while the box lock is held; --undo; and nothing left behind.
#   test/minipc-test.sh [36.4.4]      (after build.sh)
# spec: docs/32-fleet-ota.md#jetson-test-image ("Three rescue layers", item 2; Q6)
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
out=$(dirname "$here")/out/rescue/${1:-36.4.4}
name=fjarr-minipc-test
tmp=$(mktemp -d)
docker build -q -f "$here/test/Dockerfile.minipc" -t fjarr-jetson-rescue-minipc:26.04 "$here/test" > /dev/null
docker rm -f "$name" > /dev/null 2>&1 || true
docker run -d --name "$name" --privileged --cgroupns=private --tmpfs /run --tmpfs /run/lock \
	-v "$here:/src:ro" -v "$out:/out:ro" fjarr-jetson-rescue-minipc:26.04 > /dev/null
trap 'docker rm -f "$name" > /dev/null 2>&1 || true; rm -rf "$tmp"' EXIT
on() { docker exec "$name" "$@"; }
fail() {
	echo "FAIL $*"
	exit 1
}
for _ in $(seq 1 60); do
	on systemctl is-system-running 2> /dev/null | grep -qE 'running|degraded' && break
	sleep 1
done
# The mini-PC's LAN port, and an active ufw (it is not known whether the mini-PC has one).
on ip link add lan0 type dummy
on ip addr add 192.0.2.108/24 dev lan0
on ip link set lan0 up
on ufw --force enable > /dev/null
on sh -c 'command -v dnsmasq' && fail "the stand-in already has dnsmasq"

echo "== install refused 00:00-06:30"
# A date that tells the time of day a check asks for (/tmp/hhmm), else the real date.
docker exec -i "$name" sh -c 'cat > /usr/local/bin/date && chmod 755 /usr/local/bin/date' <<'DATE'
#!/bin/sh
if [ "$*" = "+%H%M" ] && [ -e /tmp/hhmm ]; then cat /tmp/hhmm; else exec /usr/bin/date "$@"; fi
DATE
for hm in 0300 0615; do
	on sh -c "echo $hm > /tmp/hhmm"
	if out=$(on /src/net-rescue.sh --mac 02:00:00:00:00:10 --iface lan0 --efi /out/fjarr-net-rescue.efi 2>&1); then
		fail "install at $hm was not refused"
	fi
	grep -q "nightly window" <<<"$out" || fail "install at $hm: $out"
	on sh -c 'command -v dnsmasq' && fail "the refused install at $hm installed dnsmasq"
	on test ! -e /var/lib/fjarr-net-rescue || fail "the refused install at $hm left state"
done
on rm -f /tmp/hhmm /usr/local/bin/date
echo "PASS install refused at 03:00 and 06:15, nothing changed"

echo "== install"
on /src/net-rescue.sh --mac 02:00:00:00:00:10 --iface lan0 --efi /out/fjarr-net-rescue.efi > "$tmp/install.txt" 2>&1 ||
	{ cat "$tmp/install.txt"; fail "install"; }
tail -n 9 "$tmp/install.txt"
on dpkg-query -W dnsmasq-base > /dev/null || fail "dnsmasq-base was not installed"
[ "$(on ufw status | grep -c fjarr-net-rescue)" -ge 4 ] || fail "the four ufw rules (and their IPv6 twins)"
on systemctl is-enabled fjarr-net-rescue-dhcp fjarr-net-rescue-http > /dev/null || fail "units not enabled"
on /src/net-rescue.sh --status > /dev/null || fail "--status"
echo "PASS install: dnsmasq-base, four ufw rules, two enabled units, --status passes"

echo "== --undo while the box lock is held"
on sh -c 'flock -o /run/lock/fjarr-zedbox.lock sleep 300 > /dev/null 2>&1 & echo $! > /run/lock-holder'
sleep 1
if on /src/net-rescue.sh --undo > "$tmp/undo.txt" 2>&1; then fail "--undo ran under the lock"; fi
grep -q 'holds /run/lock/fjarr-zedbox.lock' "$tmp/undo.txt" || fail "--undo: $(cat "$tmp/undo.txt")"
on /src/net-rescue.sh --status > /dev/null || fail "the refused --undo stopped the rescue"
# shellcheck disable=SC2016 # expands in the container
on sh -c 'kill "$(cat /run/lock-holder)"'
sleep 1
echo "PASS --undo refuses while a lab script holds the lock, and the rescue keeps serving"

echo "== --undo"
on /src/net-rescue.sh --undo > "$tmp/undo.txt" 2>&1 || { cat "$tmp/undo.txt"; fail "--undo"; }
on test ! -e /var/lib/fjarr-net-rescue || fail "/var/lib/fjarr-net-rescue is left"
on sh -c 'ls /etc/systemd/system/fjarr-net-rescue-* 2> /dev/null' && fail "unit files are left"
on sh -c 'systemctl is-active fjarr-net-rescue-dhcp fjarr-net-rescue-http' > /dev/null && fail "a unit still runs"
on ufw status | grep -q fjarr-net-rescue && fail "ufw rules are left"
# shellcheck disable=SC2016 # a dpkg-query format, not a shell expansion
if on dpkg-query -W -f '${db:Status-Status}' dnsmasq-base 2> /dev/null | grep -qx installed; then
	fail "dnsmasq-base is left"
fi
if on /src/net-rescue.sh --status > /dev/null 2>&1; then fail "--status passes after --undo"; fi
echo "PASS --undo: no state, units, ufw rules or dnsmasq-base left; --status fails"
