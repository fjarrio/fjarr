#!/bin/bash
# The ZED Box Mini's network rescue, served from the lab mini-PC: proxy DHCP that answers only the
# box's MAC, TFTP for UEFI PXEv4 and HTTP for UEFI HTTPv4 boot. The LAN router keeps handing out
# every address; this only tells the box's firmware where its rescue is (build.sh builds it).
#
#   sudo net-rescue.sh --mac MAC [--efi FILE] [--iface IF] [--http-port N]   install or update,
#                                                         enable and start
#   net-rescue.sh --status                                exit 0 when it is serving
#   sudo net-rescue.sh --undo                             remove everything it added
#   net-rescue.sh --print-config --mac MAC --iface IF --address A.B.C.D/N
#                                                         the dnsmasq configuration only
#   net-rescue.sh --stage DIR --mac MAC --iface IF --address A.B.C.D/N --efi FILE
#                                                         every file into DIR, no system change (test/)
#
# MAC is the box's Ethernet MAC (the lab keeps it in spikes/ota-jetson/.lab/rescue/box-mac).
# It installs dnsmasq-base if missing, two systemd units and /var/lib/fjarr-net-rescue, and opens
# the ports in ufw when ufw is active; --undo removes exactly that. It refuses to install
# 00:00-06:30 Europe/Stockholm (the nightly window and the 30 min fjarr-lab gives a job still
# running at its end, as lib/lab.sh's LAB_WINDOW_END_M); --undo refuses while a lab script holds the
# box lock, since writes rely on the rescue. --iface defaults to the default route's interface.
# spec: docs/32-fleet-ota.md#jetson-test-image ("Three rescue layers", item 2; Q6)
set -euo pipefail

box_mac=''
name=fjarr-net-rescue.efi
state=/var/lib/fjarr-net-rescue
units=(fjarr-net-rescue-dhcp.service fjarr-net-rescue-http.service)
unit_dir=/etc/systemd/system
lock=${FJARR_ZEDBOX_LOCK:-/run/lock/fjarr-zedbox.lock}
here=$(cd "$(dirname "$0")" && pwd)

die() {
	echo "net-rescue: $*" >&2
	exit 1
}
usage() {
	sed -n '6,15p' "$0" >&2
	exit 2
}

ip2int() {
	local a b c d
	IFS=. read -r a b c d <<< "$1"
	echo $(((a << 24) | (b << 16) | (c << 8) | d))
}
int2ip() { echo "$((($1 >> 24) & 255)).$((($1 >> 16) & 255)).$((($1 >> 8) & 255)).$(($1 & 255))"; }

default_iface() { ip -4 route show default | awk '{ for (i = 1; i < NF; i++) if ($i == "dev") { print $(i + 1); exit } }'; }
iface_address() { ip -4 -o addr show dev "$1" scope global | awk '{ print $4; exit }'; }

# The dnsmasq configuration: <iface> <address/prefix> <http port> <state dir>
config() {
	local iface=$1 addr=${2%/*} prefix=${2#*/} port=$3 dir=$4 mask user=nobody
	if ! [[ $addr =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ && $prefix =~ ^[0-9]+$ ]] || ((prefix < 8 || prefix > 30)); then
		die "bad address: $2"
	fi
	mask=$(((0xffffffff << (32 - prefix)) & 0xffffffff))
	getent passwd dnsmasq > /dev/null && user=dnsmasq
	cat << EOF
# The ZED Box Mini's network rescue, written by spikes/ota-jetson/rescue/net-rescue.sh.
# spec: docs/32-fleet-ota.md#jetson-test-image ("Three rescue layers", item 2)
# Proxy DHCP only (the LAN router assigns the addresses), for one MAC; no DNS.
port=0
interface=$iface
bind-dynamic
no-resolv
no-hosts
user=$user
pid-file=
leasefile-ro
log-dhcp
log-facility=-
dhcp-range=$(int2ip $(($(ip2int "$addr") & mask))),proxy,$(int2ip "$mask")
dhcp-mac=set:fjarr-box,$box_mac
dhcp-ignore=tag:!fjarr-box
# UEFI HTTP boot asks as HTTPClient; dnsmasq's proxy answers it like PXE and echoes the class.
dhcp-pxe-vendor=PXEClient,HTTPClient
# PXEv4 (client arch 11, arm64 UEFI): one service, so dnsmasq hands over the file name directly
# (on port 4011) and the firmware fetches it by TFTP.
pxe-service=tag:fjarr-box,ARM64_EFI,"Fjarr network rescue",$name
enable-tftp
tftp-root=$dir/tftp
# Files only under tftp/<the client's MAC>/: any other client finds nothing.
tftp-unique-root=mac
# HTTPv4 (client arch 19, arm64 UEFI HTTP): the URL itself.
dhcp-match=set:http-arm64,option:client-arch,19
dhcp-boot=tag:fjarr-box,tag:http-arm64,http://$addr:$port/$name
EOF
}

# The unit files: <state dir> <http port> <dnsmasq path>
unit_dhcp() {
	cat << EOF
# Written by spikes/ota-jetson/rescue/net-rescue.sh; removed by its --undo.
[Unit]
Description=Fjarr network rescue: proxy DHCP and TFTP for the ZED Box Mini
Wants=network-online.target
After=network-online.target

[Service]
ExecStart=$3 --keep-in-foreground --conf-file=$1/dnsmasq.conf
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF
}
unit_http() {
	cat << EOF
# Written by spikes/ota-jetson/rescue/net-rescue.sh; removed by its --undo.
[Unit]
Description=Fjarr network rescue: UEFI HTTP boot for the ZED Box Mini
Wants=network-online.target
After=network-online.target

[Service]
ExecStart=/usr/bin/python3 -I $1/http-server.py --file $1/http/$name --mac $box_mac --port $2
DynamicUser=yes
ProtectSystem=strict
ProtectHome=yes
PrivateTmp=yes
NoNewPrivileges=yes
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF
}

# Every file the rescue serves from: <dir> <iface> <address/prefix> <http port> <efi> <dnsmasq>
stage() {
	local dir=$1 macdir=${box_mac//:/-}
	install -d -m 0755 "$dir" "$dir/tftp" "$dir/tftp/$macdir" "$dir/http"
	install -m 0644 "$5" "$dir/tftp/$macdir/$name"
	ln -f "$dir/tftp/$macdir/$name" "$dir/http/$name"
	install -m 0644 "$here/net-rescue-http.py" "$dir/http-server.py"
	config "$2" "$3" "$4" "$dir" > "$dir/dnsmasq.conf"
	unit_dhcp "$dir" "$4" "$6" > "$dir/${units[0]}"
	unit_http "$dir" "$4" > "$dir/${units[1]}"
	printf 'mac=%s\niface=%s\naddress=%s\nport=%s\nsha256=%s\n' "$box_mac" "$2" "$3" "$4" \
		"$(sha256sum < "$5" | cut -d' ' -f1)" > "$dir/params"
}

ufw_rules() { # <iface> <http port>
	printf '%s\n' "allow in on $1 to any port 67 proto udp" "allow in on $1 to any port 69 proto udp" \
		"allow in on $1 to any port 4011 proto udp" "allow in on $1 to any port $2 proto tcp"
}

status() {
	local ok=1 iface address port sha now
	[ -f "$state/params" ] || { echo "not installed"; return 1; }
	box_mac=$(sed -n 's/^mac=//p' "$state/params")
	iface=$(sed -n 's/^iface=//p' "$state/params")
	address=$(sed -n 's/^address=//p' "$state/params")
	port=$(sed -n 's/^port=//p' "$state/params")
	sha=$(sed -n 's/^sha256=//p' "$state/params")
	for u in "${units[@]}"; do
		if systemctl is-active --quiet "$u"; then echo "$u: active"; else echo "$u: NOT active"; ok=0; fi
	done
	now=$(iface_address "$iface" || true)
	if [ "$now" = "$address" ]; then echo "$iface: $address"; else
		echo "$iface: has '${now:-no address}', the configuration says $address (run net-rescue.sh again)"
		ok=0
	fi
	for p in 67 69 4011; do
		if ss -Hlnu "sport = :$p" | grep -q .; then echo "udp/$p: listening"; else
			echo "udp/$p: NOT listening"
			ok=0
		fi
	done
	if [ "$(sha256sum < "$state/http/$name" | cut -d' ' -f1)" != "$sha" ]; then
		echo "$name: changed since it was installed"
		ok=0
	fi
	if python3 -I - "$port" "$name" "$(stat -c %s "$state/http/$name")" << 'EOF'; then echo "http/$port: serves $name"; else ok=0; fi
import http.client, sys
c = http.client.HTTPConnection("127.0.0.1", int(sys.argv[1]), timeout=5)
c.request("HEAD", "/" + sys.argv[2])
r = c.getresponse()
ok = r.status == 200 and r.getheader("Content-Length") == sys.argv[3]
print(f"http/{sys.argv[1]}: HEAD gave {r.status}, {r.getheader('Content-Length')} bytes", file=sys.stderr)
sys.exit(0 if ok else 1)
EOF
	echo "serves $name (sha256 ${sha:0:16}...) to $box_mac by PXE (tftp) and HTTP (http://${address%/*}:$port/$name)"
	[ "$ok" = 1 ]
}

install_rescue() { # <efi> <iface> <http port>
	local efi=$1 iface=$2 port=$3 addr dnsmasq hm
	[ "$(id -u)" = 0 ] || die "run as root"
	hm=$(TZ=Europe/Stockholm date +%H%M)
	[ "$((10#$hm))" -ge 630 ] || die "not between 00:00 and 06:30 Europe/Stockholm: the nightly window and fjarr-lab's grace"
	[ -f "$efi" ] || die "no rescue image $efi (spikes/ota-jetson/rescue/build.sh builds it)"
	[ -n "$iface" ] || iface=$(default_iface)
	[ -n "$iface" ] || die "no default route: name the LAN interface with --iface"
	addr=$(iface_address "$iface")
	[ -n "$addr" ] || die "$iface has no IPv4 address"
	command -v python3 > /dev/null || die "python3 is missing"
	mkdir -p "$state"
	if ! dnsmasq=$(command -v dnsmasq); then
		apt-get update -qq
		apt-get install -y -qq --no-install-recommends dnsmasq-base
		echo dnsmasq-base >> "$state/installed-packages"
		dnsmasq=$(command -v dnsmasq)
	fi
	if ss -Hltn "sport = :$port" | grep -q . && ! systemctl is-active --quiet "${units[1]}"; then
		die "tcp/$port is taken by another program: choose --http-port"
	fi
	stage "$state" "$iface" "$addr" "$port" "$efi" "$dnsmasq"
	install -m 0644 "$state/${units[0]}" "$state/${units[1]}" "$unit_dir/"
	if command -v ufw > /dev/null && ufw status | grep -q '^Status: active'; then
		ufw_rules "$iface" "$port" > "$state/ufw-rules"
		while read -r r; do
			# shellcheck disable=SC2086 # a ufw rule is words
			ufw $r comment fjarr-net-rescue > /dev/null
		done < "$state/ufw-rules"
	fi
	systemctl daemon-reload
	systemctl enable --quiet "${units[@]}"
	systemctl restart "${units[@]}"
	sleep 2
	status
}

undo() {
	[ "$(id -u)" = 0 ] || die "run as root"
	if [ -e "$lock" ]; then
		exec 9< "$lock"
		flock -n 9 || die "a lab script holds $lock and may rely on the rescue: not removed"
	fi
	for u in "${units[@]}"; do
		systemctl disable --now "$u" > /dev/null 2>&1 || true
		rm -f "$unit_dir/$u"
	done
	systemctl daemon-reload
	if [ -f "$state/ufw-rules" ]; then
		while read -r r; do
			# shellcheck disable=SC2086 # a ufw rule is words
			ufw delete $r > /dev/null || true
		done < "$state/ufw-rules"
	fi
	if [ -f "$state/installed-packages" ]; then
		# shellcheck disable=SC2046 # one package name per line
		apt-get remove -y $(sort -u "$state/installed-packages")
	fi
	rm -rf "$state"
	echo "net-rescue: removed"
}

mode=install efi=$here/../out/rescue/36.4.4/$name iface='' address='' port=8471 dir=''
while [ $# -gt 0 ]; do
	case $1 in
	--efi) efi=${2:?}; shift 2 ;;
	--mac) box_mac=$(tr 'A-F' 'a-f' <<< "${2:?}"); shift 2 ;;
	--iface) iface=${2:?}; shift 2 ;;
	--address) address=${2:?}; shift 2 ;;
	--http-port) port=${2:?}; shift 2 ;;
	--status) mode=status; shift ;;
	--undo) mode=undo; shift ;;
	--print-config) mode=print; shift ;;
	--stage) mode=stage dir=${2:?}; shift 2 ;;
	*) usage ;;
	esac
done
[[ $port =~ ^[0-9]+$ ]] || die "bad port: $port"
case $mode in install | print | stage)
	# Lower case: dnsmasq names the per-client TFTP directory in lower case.
	[[ $box_mac =~ ^([0-9a-f]{2}:){5}[0-9a-f]{2}$ ]] || die "--mac needs the box's MAC, like 02:00:00:00:00:01"
	;;
esac
case $mode in
install) install_rescue "$efi" "$iface" "$port" ;;
status) status ;;
undo) undo ;;
print)
	[ -n "$iface" ] && [ -n "$address" ] || usage
	config "$iface" "$address" "$port" "$state"
	;;
stage)
	[ -n "$iface" ] && [ -n "$address" ] || usage
	[ -f "$efi" ] || die "no rescue image $efi"
	stage "$dir" "$iface" "$address" "$port" "$efi" "$(command -v dnsmasq || echo /usr/sbin/dnsmasq)"
	;;
esac
