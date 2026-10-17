#!/usr/bin/env bash
# probe-host.sh: a read-only look at the lab's mini-PC as the box's flashing host (slice 8.0, B2).
# spec: docs/32-fleet-ota.md#jetson-test-image
#
#   probe-host.sh
#
# One ssh session runs a script as root on the mini-PC (sudo -n). It changes nothing: it starts no
# server, sends nothing on the LAN (it listens 60 s for DHCP and PXE traffic; nmap's DHCP discover
# runs only when nmap is installed), and reads GNOME's automount settings as the desktop's user
# without touching the session. It does not touch the box, its plug or RCM, so it takes no lab
# lock; it still keeps out of the nightly window (L1). The raw dump stays in .lab/probe/;
# results/<date>-probe-host.txt gets a digest and the redacted dump.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=lib/lab.sh
. "$here/lib/lab.sh"

lab_init probe-host 600 nolock

host_script() {
  cat <<'EOF'
sec() { printf '\n===== %s\n' "$*"; }
run() { printf '\n$ %s\n' "$*"; timeout 90 "$@" 2>&1; rc=$?; [ "$rc" -eq 0 ] || printf '[exit %s]\n' "$rc"; }
pkgs() { printf '\n$ dpkg-query %s\n' "$*"; dpkg-query -W -f '${Package} ${Version} ${db:Status-Status}\n' "$@" 2>&1; }
speed() { cat "/sys/bus/usb/devices/$1/speed" 2>/dev/null || echo '?'; }
state() { for u in "$@"; do printf 'unit %s: %s\n' "$u" "$(systemctl is-active "$u" 2>&1)"; done; }

sec "identity"
run date -u
run uptime
run sh -c '. /etc/os-release; echo "$PRETTY_NAME"'
run uname -r -m
run timedatectl show -p Timezone -p NTPSynchronized -p TimeUSec

sec "USB: the box's port (NVIDIA known issue 4695663: initrd flashing fails on ports of 10 Gbps or more)"
run lsusb -d 0955:
run lsusb -t
for d in /sys/bus/usb/devices/*; do
  [ -r "$d/idVendor" ] && [ "$(cat "$d/idVendor")" = 0955 ] || continue
  bus=$(cat "$d/busnum"); path=$(cat "$d/devpath"); hop=${path%%.*}
  port=/sys/bus/usb/devices/$bus-0:1.0/usb$bus-port$hop
  printf '\nbox: %s, 0955:%s, link %s Mbit/s, USB %s, bus %s, device path %s\n' "${d##*/}" \
    "$(cat "$d/idProduct")" "$(cat "$d/speed")" "$(tr -d ' ' <"$d/version")" "$bus" "$path"
  case $path in *.*) echo "box: behind a hub, on root port $hop" ;; esac
  printf 'box: root hub usb%s, %s Mbit/s\n' "$bus" "$(speed "usb$bus")"
  if [ -e "$port/peer" ]; then
    peer=$(basename "$(readlink -f "$port/peer")"); pbus=${peer#usb}; pbus=${pbus%%-port*}
    printf 'box: the same port'"'"'s SuperSpeed side is %s, on root hub usb%s, %s Mbit/s\n' "$peer" "$pbus" "$(speed "usb$pbus")"
  else
    echo "box: no SuperSpeed peer recorded for usb$bus-port$hop"
  fi
  ctl=$(basename "$(readlink -f "/sys/bus/usb/devices/usb$bus/..")")
  run lspci -nn -s "$ctl"
done
echo; echo "every root hub:"
for h in /sys/bus/usb/devices/usb*; do
  printf '%s: %s Mbit/s, USB %s, controller %s, authorized_default %s\n' "${h##*/}" "$(speed "${h##*/}")" \
    "$(tr -d ' ' <"$h/version")" "$(basename "$(readlink -f "$h/..")")" "$(cat "$h/authorized_default")"
done
if [ -e /run/fjarr-zedbox-usb.restore ]; then iso="in place, $(wc -l </run/fjarr-zedbox-usb.restore) hub setting(s) saved"; else iso=none; fi
echo "the lab's USB isolation of the box (lib/lab.sh): $iso"

sec "kernel: USB networking (RNDIS, CDC Ethernet) and 32-bit x86 emulation"
run sh -c 'modinfo rndis_host cdc_ether | grep -E "^(filename|name|description|license|depends|intree|vermagic):"'
run sh -c 'lsmod | grep -E "^(rndis_host|cdc_ether|cdc_ncm|cdc_acm|usbnet) "'
run sh -c 'grep -E "^(# )?CONFIG_IA32_EMULATION" "/boot/config-$(uname -r)"'
run cat /proc/cmdline
echo "(NVIDIA's flashing tools tegrabct_v2 and tegrahost_v2 are 32-bit x86 executables)"

sec "network: links, IPv6"
run sysctl net.ipv6.conf.all.disable_ipv6 net.ipv6.conf.default.disable_ipv6
run ip -br link
run ip -4 -br addr
run ip -6 -o addr show
run ip route
run ip -6 route

sec "firewall"
run ufw status verbose

sec "Docker"
run sh -c 'command -v docker || echo "docker: no such command"'
state docker.service docker.socket containerd.service
pkgs docker.io docker-ce docker-ce-cli containerd containerd.io

sec "NFS"
pkgs nfs-kernel-server nfs-common
state nfs-server.service
run sh -c 'cat /etc/exports 2>&1'

sec "dnsmasq, and DHCP, DNS, TFTP and PXE listeners on this host"
pkgs dnsmasq dnsmasq-base
state dnsmasq.service
run ps -C dnsmasq -o pid=,user=,args=
run ss -H -lunp '( sport = :53 or sport = :67 or sport = :69 or sport = :547 or sport = :4011 )'
run ip -br link show type bridge
run sh -c 'snap list lxd 2>&1 | tail -n +2 || true'

dev=$(ip route show default | awk '{ for (i = 1; i < NF; i++) if ($i == "dev") { print $(i + 1); exit } }')
sec "the LAN's DHCP server, from this host's own lease on ${dev:-?}"
[ -n "$dev" ] && run nmcli -f DHCP4 device show "$dev"

sec "other DHCP or PXE servers on the LAN: 60 s of listening on ${dev:-?}, nothing sent"
if [ -n "$dev" ] && command -v tcpdump >/dev/null; then
  timeout 60 tcpdump -p -n -l -vv -i "$dev" -c 500 'udp and (port 67 or port 68 or port 4011)' 2>/dev/null |
    awk '/BOOTP\/DHCP, Request/ { req++ } /BOOTP\/DHCP, Reply/ { rep++ }
      /Server-ID/ { srv[$NF]++ } /PXEClient/ { pxe++ } /BF Option|Boot-?[Ff]ile| file "/ { bf++ }
      /TFTP/ { tftp++ } / port 4011|\.4011/ { p4011++ }
      END { printf "DHCP requests seen: %d, replies seen: %d\n", req, rep
            for (k in srv) printf "DHCP server (option 54) %s: %d replies\n", k, srv[k]
            printf "PXEClient vendor class: %d, boot file: %d, TFTP server option: %d, port 4011 packets: %d\n", pxe, bf, tftp, p4011 }'
else
  echo "no listen: tcpdump is not installed or there is no default route"
fi
if command -v nmap >/dev/null && [ -n "$dev" ]; then
  echo; echo "nmap's DHCP discover (one DISCOVER from a fixed fake MAC, no lease taken):"
  timeout 60 nmap --script broadcast-dhcp-discover -e "$dev" 2>&1 | grep -E 'Server Identifier|IP Offered|Router|TFTP|Bootfile|Domain Name Server' || true
else
  echo "nmap is not installed: no DHCP discover sent"
fi

sec "NVIDIA's host setup for flashing (phase C0): present?"
run sh -c 'ls -la /etc/udev/rules.d/ | grep -iE "l4t|nv" || echo "no NVIDIA udev rules"'
run sh -c 'ls -la /run/nvidia_initrd_flash 2>&1'

sec "free disk"
run df -h -x tmpfs -x devtmpfs -x squashfs -x overlay -x efivarfs

sec "GNOME automount (read only, as the desktop's user)"
desk=
for s in $(loginctl list-sessions --no-legend 2>/dev/null | awk '{ print $1 }'); do
  info=$(loginctl show-session "$s" -p Name -p Seat -p Class 2>/dev/null)
  case $info in *Seat=seat0*) ;; *) continue ;; esac
  case $info in *Class=user*) desk=$(printf '%s\n' "$info" | sed -n 's/^Name=//p'); break ;; esac
done
echo "the desktop's user: ${desk:-none on seat0}"
if [ -n "$desk" ]; then
  uid=$(id -u "$desk")
  for k in automount automount-open autorun-never; do
    printf 'org.gnome.desktop.media-handling %s: %s\n' "$k" \
      "$(sudo -u "$desk" -H env XDG_RUNTIME_DIR="/run/user/$uid" gsettings get org.gnome.desktop.media-handling "$k" 2>&1)"
  done
fi

sec "the lab lock"
run sh -c 'ls -la /run/lock/fjarr-zedbox.lock 2>&1'
run sh -c 'lslocks -o PID,COMMAND,TYPE,MODE,PATH | grep -E "PATH|fjarr-zedbox" || true'

sec "tools"
for t in lsusb flock lslocks jq curl rsync zstd sha256sum sgdisk tcpdump nmap arp-scan fping python3 docker dnsmasq exportfs; do
  printf '%s: %s\n' "$t" "$(command -v "$t" || echo -)"
done
exit 0
EOF
}

digest() {
  grep -aE \
    -e '^(PRETTY|Ubuntu)|^Timezone=|^NTPSynchronized=' \
    -e '^box: ' \
    -e '^(usb[0-9]+): [0-9?]+ Mbit/s' -e '^the lab.s USB isolation' \
    -e '^filename: ' -e '^(# )?CONFIG_IA32_EMULATION' \
    -e '^net\.ipv6\.conf\.(all|default)\.disable_ipv6 =' \
    -e '^Status: ' \
    -e '^(docker|nfs|dnsmasq|containerd)[a-z.-]* [^ ]+ (installed|not-installed|config-files)' \
    -e '^unit [^ ]+: ' \
    -e '^DHCP4\.OPTION\[[0-9]+\]: *(dhcp_server_identifier|next_server|filename|routers) ' \
    -e '^(DHCP requests seen|DHCP server \(option 54\)|PXEClient vendor class|nmap|no listen)' \
    -e '^/dev/' \
    -e '^the desktop.s user|^org\.gnome\.desktop\.media-handling' \
    "$1" || true
}

lab_log "probing the mini-PC $LAB_MINIPC (read only, about 2 min) ..."
stamp=$(TZ=$LAB_TZ date +%Y%m%dT%H%M%S)
raw=$LAB_DIR/probe/$stamp-host
mkdir -p "$raw"
lab_mpc_root "$LAB_SECRETS_SCRIPT" >"$raw/secrets.txt" || lab_die "could not collect the mini-PC's identifiers"
lab_mpc_root "$(host_script)" >"$raw/host.txt" 2>&1 || lab_die "the probe failed on the mini-PC (see $raw/host.txt)"
lab_log "raw dump: $raw/host.txt ($(wc -l <"$raw/host.txt") lines)"

lab_redact "$raw/secrets.txt" <"$raw/host.txt" >"$raw/host.redacted.txt"
out=$LAB_HOME/results/$(lab_today)-probe-host.txt
[[ -e $out ]] && out=${out%.txt}-$(TZ=$LAB_TZ date +%H%M).txt
{
  printf '# The lab mini-PC as the ZED Box Mini'"'"'s flashing host, read-only probe: %s %s\n' "$(TZ=$LAB_TZ date '+%Y-%m-%d %H:%M')" "$LAB_TZ"
  printf '# spec: docs/32-fleet-ota.md#jetson-test-image (phase B2). Written by probe-host.sh. MACs,\n'
  printf '# serial numbers, disk and partition GUIDs and machine IDs are replaced by <kind>.\n'
  printf '\n## Digest\n\n'
  digest "$raw/host.redacted.txt"
  printf '\n## The dump\n'
  cat "$raw/host.redacted.txt"
} >"$raw/result.txt"
lab_leak_check "$raw/result.txt" "$raw/secrets.txt" || lab_die "an identifier survived the redaction: nothing written to results/ (see $raw/result.txt)"
cp "$raw/result.txt" "$out"
lab_log "written: ${out#"$LAB_HOME"/}"
