#!/bin/bash
# The network rescue's off-box bench, run by netboot-test.sh inside a privileged container with no
# outside network. A private LAN (TEST-NET-1) on a bridge: a router doing plain DHCP with a fixed
# lease for the box's MAC, as the lab router does; the "mini-PC" running exactly what
# `net-rescue.sh --stage` writes (dnsmasq and the HTTP server, from their unit files); a probe
# client; and the box: AAVMF, edk2's arm64 UEFI for QEMU. The MACs are made up: the box's real one
# stays out of the repo, and nothing here depends on its value.
#   1. probes: the proxy answers PXE and HTTP clients with the box's MAC and nobody else; TFTP and
#      HTTP serve the rescue to that MAC only
#   2. PXEv4 boot of the test UKI (HTTP blocked): ssh, tools, firmware variables, the dead-man
#   3. HTTPv4 boot of the test UKI (TFTP and port 4011 blocked): ssh, reboot-recovery
#   4. boot of the lab UKI, both ways open: its host key, and the test key refused
# spec: docs/32-fleet-ota.md#jetson-test-image ("Three rescue layers", item 2)
set -euo pipefail
src=/src out=/out log=/log
mac=02:00:00:00:00:10 other=02:00:00:00:00:20
lan=192.0.2 box=192.0.2.115 proxy=192.0.2.108
name=fjarr-net-rescue.efi
key=/tmp/id_ed25519
results=$log/results.txt

note() { echo "$(date +%T) $*" | tee -a "$results"; }
pass() { note "PASS $*"; }
fail() {
	note "FAIL $*"
	exit 1
}
in_ns() { ip netns exec "$@"; }
probe() { in_ns client python3 -I "$src/test/dhcp-probe.py" "$@"; }
# ssh to the rescue as the mini-PC; ServerAlive ends a session whose guest has reset (QEMU exits on
# the reset, and without it ssh waits out TCP's retransmissions, about 20 minutes).
ssh_box() {
	in_ns minipc ssh -i "$key" -o UserKnownHostsFile="$out/known_hosts" -o HostKeyAlias=fjarr-net-rescue \
		-o StrictHostKeyChecking=yes -o BatchMode=yes -o ServerAliveInterval=5 -o ServerAliveCountMax=3 "$@"
}
rssh() { # an emulated guest on a busy host can be slow to connect, so up to 3 tries
	local i rc=255
	for i in 1 2 3; do
		ssh_box -o ConnectTimeout=30 "root@$box" "$@" && return 0
		rc=$?
		[ "$rc" = 255 ] && [ "$i" -lt 3 ] && alive || return "$rc"
		sleep 5
	done
}
block() { # <nft rules for the mini-PC's input>
	in_ns minipc nft flush ruleset
	in_ns minipc nft add table inet bench
	in_ns minipc nft "add chain inet bench in { type filter hook input priority 0; }"
	[ -z "$1" ] || in_ns minipc nft "add rule inet bench in $1 drop"
}
serve() { "$src/net-rescue.sh" --stage /srv/rescue --mac "$mac" --iface eth0 --address "$proxy/24" --efi "$1"; }

install -m 0600 "$out/test/id_ed25519" "$key"
: > "$results"
note "bench: $(qemu-system-aarch64 --version | head -n 1), AAVMF $(dpkg-query -W -f '${Version}' qemu-efi-aarch64), $(dnsmasq --version | head -n 1)"

# The LAN.
ip link add lan type bridge
ip link set lan up
for ns in router minipc client; do
	ip netns add "$ns"
	ip link add "v-$ns" type veth peer name eth0 netns "$ns"
	ip link set "v-$ns" master lan up
	in_ns "$ns" ip link set lo up
	in_ns "$ns" ip link set eth0 up
done
in_ns router ip addr add "$lan.1/24" dev eth0
in_ns minipc ip addr add "$proxy/24" dev eth0
in_ns client ip addr add "$lan.50/24" dev eth0
# (--no-ping: it offers at once, instead of first pinging a dynamic address for 3 s.)
in_ns router dnsmasq --keep-in-foreground --port=0 --interface=eth0 --bind-interfaces --pid-file= --no-ping \
	--dhcp-range="$lan.100,$lan.199,255.255.255.0,1h" --dhcp-host="$mac,$box" \
	--dhcp-option=option:router,"$lan.1" --dhcp-leasefile=/tmp/router.leases --log-dhcp \
	--log-facility=- 2> "$log/router-dnsmasq.log" &

# The mini-PC: net-rescue.sh's files, run by the command lines of its unit files.
serve "$out/test/fjarr-net-rescue-test.efi"
cp /srv/rescue/dnsmasq.conf /srv/rescue/*.service "$log/"
read -ra dhcp_cmd <<< "$(sed -n 's/^ExecStart=//p' /srv/rescue/fjarr-net-rescue-dhcp.service)"
read -ra http_cmd <<< "$(sed -n 's/^ExecStart=//p' /srv/rescue/fjarr-net-rescue-http.service)"
in_ns minipc "${dhcp_cmd[@]}" 2> "$log/minipc-dnsmasq.log" &
in_ns minipc "${http_cmd[@]}" > "$log/minipc-http.log" 2>&1 &
block ""
sleep 2

note "1. probes"
r=$(probe discover --mac "$mac" --client pxe)
echo "$r" > "$log/probe-pxe-box.txt"
grep -q "from=$proxy type=OFFER server=$proxy yiaddr=0.0.0.0 siaddr=$proxy vendor=PXEClient" <<< "$r" ||
	fail "no proxy offer to a PXE client with the box's MAC: $r"
grep -q "from=$lan.1 type=OFFER .*yiaddr=$box" <<< "$r" || fail "the router made no offer: $r"
r=$(probe request4011 --mac "$mac" --client pxe --server "$proxy" --ciaddr "$lan.50")
echo "$r" > "$log/probe-pxe-4011-box.txt"
grep -q "type=ACK server=$proxy .*siaddr=$proxy vendor=PXEClient file=$name" <<< "$r" ||
	fail "port 4011 did not name the file for the box: $r"
pass "PXE client with the box's MAC: proxy offer, then $name from port 4011"
r=$(probe discover --mac "$mac" --client http)
echo "$r" > "$log/probe-http-box.txt"
grep -q "from=$proxy type=OFFER server=$proxy yiaddr=0.0.0.0 .*vendor=HTTPClient file=http://$proxy:8471/$name opt43=no" <<< "$r" ||
	fail "no HTTPClient proxy offer with the URL for the box: $r"
pass "HTTP client with the box's MAC: proxy offer with vendor class HTTPClient and the URL"
for c in pxe http; do
	r=$(probe discover --mac "$other" --client "$c")
	echo "$r" > "$log/probe-$c-other.txt"
	grep -q "from=$lan.1 type=OFFER" <<< "$r" || fail "the router did not answer $other: $r"
	if grep -q "from=$proxy" <<< "$r"; then fail "the proxy answered $other: $r"; fi
done
r=$(probe request4011 --mac "$other" --client pxe --server "$proxy" --ciaddr "$lan.50")
[ -z "$r" ] || fail "port 4011 answered $other: $r"
pass "another MAC: only the router answers, port 4011 stays silent"

want=$(sha256sum < "$out/test/fjarr-net-rescue-test.efi" | cut -d' ' -f1)
set_client() { # <MAC> <address>: the probe client becomes another machine
	in_ns client ip link set eth0 down
	in_ns client ip link set eth0 address "$1"
	in_ns client ip addr flush dev eth0
	in_ns client ip addr add "$2/24" dev eth0
	in_ns client ip link set eth0 up
	sleep 1
}
set_client "$mac" "$lan.50"
for bs in 1468 512; do
	in_ns client curl -sS --tftp-blksize "$bs" -o "/tmp/tftp-$bs.efi" "tftp://$proxy/$name" ||
		fail "TFTP (blksize $bs) to the box's MAC"
	[ "$(sha256sum < "/tmp/tftp-$bs.efi" | cut -d' ' -f1)" = "$want" ] || fail "TFTP (blksize $bs): wrong content"
done
in_ns client curl -sS -f -o /tmp/http.efi "http://$proxy:8471/$name" || fail "HTTP to the box's MAC"
[ "$(sha256sum < /tmp/http.efi | cut -d' ' -f1)" = "$want" ] || fail "HTTP: wrong content"
pass "TFTP (blksize 1468, and 512 past block 65535) and HTTP serve the box's MAC the whole file"
# Another machine: its own MAC and address. (dnsmasq maps a TFTP client's address to its MAC
# through an ARP view it refreshes every 90 s, so the box's own address keeps the box's MAC that long.)
set_client "$other" "$lan.51"
if in_ns client curl -sS -o /tmp/x "tftp://$proxy/$name" 2> "$log/tftp-other.txt"; then fail "TFTP served $other"; fi
code=$(in_ns client curl -s -o /dev/null -w '%{http_code}' "http://$proxy:8471/$name")
[ "$code" = 403 ] || fail "HTTP gave $code to $other"
pass "another MAC: TFTP finds no file, HTTP answers 403"
in_ns client ip link set eth0 down

# The box. QEMU exits on the guest's reset (-no-reboot), so a reset into RCM ends the run.
# AAVMF's network stack needs a random number generator since edk2 2025.02 (Ubuntu LP #2069439),
# hence virtio-rng; neoverse-n1 is an Armv8.2 core like the Orin's (-cpu max spends minutes in
# the firmware under TCG). GICv3 as on the Orin: NVIDIA's kernel fails to set up its IPIs on
# QEMU's default GICv2 and oopses bringing up the second CPU. acpi=off, so AAVMF hands the kernel
# a device tree, as NVIDIA's UEFI does. No bootindex: AAVMF drops the network entries it cannot
# match to QEMU's boot order, and with no disk they are its only entries anyway (PXEv4, PXEv6,
# HTTPv4, HTTPv6).
vm() { # <run name>
	cp /usr/share/AAVMF/AAVMF_VARS.fd "/tmp/vars-$1.fd"
	ip tuntap add tap-box mode tap 2> /dev/null || true
	ip link set tap-box master lan up
	qemu-system-aarch64 -M virt,gic-version=3,acpi=off -cpu neoverse-n1 -smp 2 -m 2048 -no-reboot \
		-display none -monitor none \
		-drive if=pflash,format=raw,readonly=on,file=/usr/share/AAVMF/AAVMF_CODE.fd \
		-drive if=pflash,format=raw,file="/tmp/vars-$1.fd" \
		-device virtio-rng-pci -netdev tap,id=n0,ifname=tap-box,script=no,downscript=no \
		-device virtio-net-pci,netdev=n0,mac="$mac",romfile= \
		-serial "file:$log/console-$1.log" &
	vm_pid=$!
	vm_start=$(date +%s)
}
alive() { [ -d "/proc/$vm_pid" ] && ! grep -q ') Z ' "/proc/$vm_pid/stat"; }
wait_ssh() { # until root logs in over ssh, at most 20 minutes
	local end=$(($(date +%s) + 1200))
	while [ "$(date +%s)" -lt "$end" ]; do
		alive || fail "QEMU ended before ssh came up"
		ssh_box -o ConnectTimeout=10 "root@$box" true 2> /dev/null && return 0
		sleep 5
	done
	fail "no ssh within 20 minutes"
}
wait_exit() { # <seconds>: until QEMU ends (the guest reset), prints the time it ended
	for _ in $(seq 1 "$1"); do
		alive || { date +%s; return 0; }
		sleep 1
	done
	return 1
}
check_rescue() { # <run> <the firmware's boot entry, as efibootmgr names it>
	local cmdline
	cmdline=$(rssh cat /proc/cmdline)
	[ "$cmdline" = "$(ukify_cmdline "$out/test/fjarr-net-rescue-test.efi")" ] ||
		fail "$1: the kernel did not get the UKI's command line: $cmdline"
	rssh 'echo; hostname; uname -r; cat /proc/uptime; lsmod; ip -4 addr; mount; efibootmgr -v; logread | tail -n 20
		sgdisk --version; mke2fs -V; fdisk --version; nvme version; zstd -V; lsblk --version
		mkfs.fat --help 2>&1 | head -n 1; ls -l /usr/sbin/reboot-recovery
		ls /dev/watchdog* 2> /dev/null || echo "no watchdog device"; true' > "$log/rescue-$1.txt" 2>&1 ||
		fail "$1: commands over ssh"
	grep -qx fjarr-net-rescue "$log/rescue-$1.txt" || fail "$1: hostname"
	grep -q '^mke2fs 1\.' "$log/rescue-$1.txt" || fail "$1: mke2fs is not e2fsprogs'"
	grep -q '^fdisk from util-linux' "$log/rescue-$1.txt" || fail "$1: fdisk is not util-linux's"
	for t in 'GPT fdisk (sgdisk) version' 'nvme version' 'zstd command line interface' 'lsblk from util-linux' \
		'Usage: mkfs.fat' '/usr/sbin/reboot-recovery'; do
		grep -qF "$t" "$log/rescue-$1.txt" || fail "$1: no '$t' from the rescue"
	done
	grep -q "^BootCurrent:" "$log/rescue-$1.txt" || fail "$1: efibootmgr cannot read the firmware variables"
	current=$(sed -n 's/^BootCurrent: //p' "$log/rescue-$1.txt")
	grep -q "^Boot$current\* $2" "$log/rescue-$1.txt" || fail "$1: booted Boot$current, not $2"
	for m in r8168 nvme pcie_tegra194 pwm_fan; do
		grep -q "^$m " "$log/rescue-$1.txt" || fail "$1: module $m not loaded"
	done
	pass "$1: booted by '$2', root over ssh with the lab host key, the UKI's command line, modules, tools, efivars"
}
ukify_cmdline() { python3 -I -c 'import pefile,sys
pe = pefile.PE(sys.argv[1], fast_load=True)
print(next(s.get_data()[:s.Misc_VirtualSize] for s in pe.sections if s.Name.rstrip(b"\0") == b".cmdline").decode().strip())' "$1"; }

deadman=$(ukify_cmdline "$out/test/fjarr-net-rescue-test.efi" | grep -o 'fjarr\.deadman=[0-9]*' | cut -d= -f2)
[ -n "$deadman" ] || fail "the test UKI sets no fjarr.deadman"

note "2. PXEv4 boot (HTTP blocked)"
block "tcp dport 8471"
vm pxe
wait_ssh
note "pxe: ssh after $(($(date +%s) - vm_start)) s"
rssh touch /run/fjarr-lab-ok
check_rescue pxe "UEFI PXEv4"
grep -q "sent /srv/rescue/tftp/${mac//:/-}/$name to $box" "$log/minipc-dnsmasq.log" || fail "pxe: dnsmasq logged no TFTP transfer to the box"
rssh touch /run/fjarr-lab-ok
t0=$(date +%s)
sleep 70
t1=$(date +%s) # the last touch happens between t1 and t1b
rssh touch /run/fjarr-lab-ok
t1b=$(date +%s)
te=$(wait_exit $((deadman + 120))) || fail "pxe: the dead-man did not fire within $((deadman + 120)) s of the last touch"
grep -q "no operator contact for $deadman s: entering RCM" "$log/console-pxe.log" || fail "pxe: no dead-man message on the console"
grep -q "Restarting system with command 'forced-recovery'" "$log/console-pxe.log" || fail "pxe: no RESTART2 forced-recovery on the console"
# Not before the limit after the last touch; at most two of its 10-second polls after it, plus
# slack for an emulated guest on a busy host.
[ $((te - t1)) -ge $((deadman - 2)) ] && [ $((te - t1b)) -le $((deadman + 45)) ] ||
	fail "pxe: the dead-man fired $((te - t1)) s after the last touch (limit $deadman s)"
pass "pxe: the dead-man entered RCM $((te - t1)) s after the last touch ($((te - t0)) s after the one before): reboot(RESTART2, forced-recovery)"

note "3. HTTPv4 boot (TFTP and port 4011 blocked)"
block "udp dport { 69, 4011 }"
vm http
wait_ssh
note "http: ssh after $(($(date +%s) - vm_start)) s"
rssh touch /run/fjarr-lab-ok
check_rescue http "UEFI HTTPv4"
grep -q "served /$name to $box ($mac)" "$log/minipc-http.log" || fail "http: the HTTP server logged no transfer to the box"
ssh_box -o ConnectTimeout=30 "root@$box" reboot-recovery > /dev/null 2>&1 || true # one try: it resets
wait_exit 60 > /dev/null || fail "http: reboot-recovery did not reset the guest"
grep -q "Restarting system with command 'forced-recovery'" "$log/console-http.log" || fail "http: no RESTART2 forced-recovery on the console"
pass "http: reboot-recovery restarts with the command forced-recovery"

# Both ways open, so the firmware's first entry, PXEv4, takes it. (Run 8, 2026-10-17, booted the lab
# UKI by HTTP alone with the host at load 30-40 from a Yocto build: AAVMF reset the connection at 48%
# of the download, "Unexpected network error". The firmware's HTTP client is emulated here; HTTPv4
# itself passed in 3.)
note "4. boot of the lab UKI (PXEv4 and HTTPv4 open)"
block ""
serve "$out/$name"
vm lab
# (-q and the filter: OpenSSH 10's ssh-keyscan prints the server's banner as a comment on stdout.)
for _ in $(seq 1 180); do
	in_ns minipc ssh-keyscan -q -T 5 -t ed25519 "$box" 2> /dev/null | grep -v '^#' | cut -d' ' -f2,3 > /tmp/hostkey &&
		[ -s /tmp/hostkey ] && break
	sleep 5
done
[ -s /tmp/hostkey ] || fail "lab: sshd did not answer within 15 minutes"
[ "$(cat /tmp/hostkey)" = "$(cut -d' ' -f2,3 "$out/known_hosts")" ] ||
	fail "lab: the host key is not the one in known_hosts: $(cat /tmp/hostkey)"
if rssh true 2> "$log/lab-login.txt"; then fail "lab: the test key logged in"; fi
grep -q "Permission denied" "$log/lab-login.txt" || fail "lab: login failed for another reason: $(cat "$log/lab-login.txt")"
via=$(grep -aoE '>>Start (PXE|HTTP Boot) over IPv[46]' "$log/console-lab.log" | tail -n 1 | cut -c3-)
pass "lab UKI: up after $(($(date +%s) - vm_start)) s (${via:-boot path not on the console}) with the lab host key; the test key is refused"
kill "$vm_pid"
note "all passed"
