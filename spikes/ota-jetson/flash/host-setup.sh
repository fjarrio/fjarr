#!/bin/bash
# Prepare the lab's mini-PC to flash, back up and restore the ZED Box Mini with NVIDIA's tools in
# the fjarr-l4t-flash container (phase C), and undo it again:
#   - nfs-kernel-server: the box's flash initrd mounts the L4T tree from this host over NFS, on the
#     USB link's IPv6 network fc00:1:1::/48. The container runs no NFS daemons (host-nfs-only)
#   - exports of each tree's rootfs, tools/kernel_flash/images and tools/backup_restore, read-write
#     with NVIDIA's options, to fc00:1:1::/48 only, at the paths the container sees them under
#   - NVIDIA's udev rules: 10-l4t-usb-msd.rules (udisks, and so GNOME, leave the box's USB disks
#     alone) and 99-l4t-host.rules (NetworkManager leaves the flash link alone)
#   - /run/nvidia_initrd_flash/docker_host_network, made at every boot by tmpfiles.d: with it,
#     NVIDIA's script in the container expects this host's NFS server
#   - with ufw active: NFS (TCP 2049) from fc00:1:1::/48
# spec: docs/32-fleet-ota.md#jetson-test-image (flashing from the mini-PC; Q6: every change undoable)
#
#   host-setup.sh [--dry-run] --l4t <Linux_for_Tegra> [--l4t <another tree>]...
#   host-setup.sh [--dry-run] --undo
#
# Every change is recorded in /var/lib/fjarr-flash-host/state, and --undo reverts exactly those,
# keeping what was there before. A second run adds trees and changes nothing else. Refuses before
# any change: 23:55-06:30 Europe/Stockholm (the nightly window and the 30 min fjarr-lab gives a job
# still running at its end, as lib/lab.sh's LAB_WINDOW_END_M), while another lab script holds
# /run/lock/fjarr-zedbox.lock, with IPv6 off, or where a file it would write exists and is not its
# own. --dry-run changes nothing, needs no root, and shows each command and file it would write.
set -euo pipefail

state_dir=/var/lib/fjarr-flash-host
state=$state_dir/state
packages_before=$state_dir/packages-before
lock=/run/lock/fjarr-zedbox.lock
exports_file=/etc/exports.d/fjarr-l4t-flash.exports
tmpfiles_conf=/etc/tmpfiles.d/fjarr-l4t-flash.conf
marker_dir=/run/nvidia_initrd_flash
marker=$marker_dir/docker_host_network
rules_dir=/etc/udev/rules.d
net=fc00:1:1::/48
# NVIDIA's PERMISSION_STR (tools/kernel_flash/l4t_kernel_flash_vars.func).
opts=rw,nohide,insecure,no_subtree_check,async,no_root_squash
# NVIDIA's two rules, the same in L4T 36.4.4 and 36.5.2.
declare -A rule_sha=(
	[10-l4t-usb-msd.rules]=56ef9816cb9b54009992f70046efbeedd99c83d4cb6cb5dda909838a1af60bbb
	[99-l4t-host.rules]=6132a03dcea7e030cf51b4eb6f84d2e6d337947c46f133f9ae0e227b941932f5
)

usage() {
	echo "usage: host-setup.sh [--dry-run] --l4t <Linux_for_Tegra> [--l4t <another tree>]..." >&2
	echo "       host-setup.sh [--dry-run] --undo" >&2
	exit 2
}
say() { echo "host-setup: $*"; }
die() { echo "host-setup: $*" >&2; exit 1; }

dry=0 undo=0 trees=()
while [ $# -gt 0 ]; do
	case $1 in
	--dry-run) dry=1 ;;
	--undo) undo=1 ;;
	--l4t) [ $# -ge 2 ] || usage; trees+=("$2"); shift ;;
	*) usage ;;
	esac
	shift
done
[ "$undo" = 1 ] && [ ${#trees[@]} -gt 0 ] && usage
[ "$undo" = 1 ] || [ ${#trees[@]} -gt 0 ] || usage

run() { # a command that changes the host, or what it would be
	if [ "$dry" = 1 ]; then echo "  would run: $*"; else "$@"; fi
}
put() { # put <file> <mode> < content: write a file, or show it
	local file=$1 mode=$2 content line
	content=$(cat)
	if [ "$dry" = 1 ]; then
		echo "  would write $file (mode $mode):"
		while IFS= read -r line; do echo "    | $line"; done <<<"$content"
		return
	fi
	printf '%s\n' "$content" >"$file.fjarr-new"
	chmod "$mode" "$file.fjarr-new"
	mv "$file.fjarr-new" "$file"
}
recorded() { [ -f "$state" ] && grep -qxF "$*" "$state"; }
record() { # note a change for --undo, once
	[ "$dry" = 0 ] || return 0
	recorded "$@" && return 0
	[ -d "$state_dir" ] || install -d -m 0755 "$state_dir"
	echo "$*" >>"$state"
}
is_installed() { # is_installed <package>
	[ "$(dpkg-query -W -f='${Status}' "$1" 2>/dev/null || true)" = "install ok installed" ]
}
ours() { # ours <file>: absent, or made by this script
	[ ! -e "$1" ] || recorded file "$1"
}
mkdirs() { # mkdirs <dir>...: create missing directories, recorded
	local d
	for d in "$@"; do
		[ -d "$d" ] && continue
		run install -d -m 0755 "$d"
		record dir "$d"
	done
}

# --- both modes: who, when, and the lab lock -------------------------------------------------------
[ "$dry" = 1 ] || [ "$(id -u)" = 0 ] || die "run as root (or with --dry-run)"
# A run takes about a minute, so none starts after 23:55 either.
now=$(TZ=Europe/Stockholm date +%H%M)
if [ $((10#$now)) -lt 630 ] || [ $((10#$now)) -ge 2355 ]; then
	[ "$dry" = 1 ] || die "not from 23:55 to 06:30 Europe/Stockholm: the nightly lab window and fjarr-lab's grace"
	say "note: a real run would refuse now, at or near the nightly window (00:00-06:30 Europe/Stockholm, with fjarr-lab's grace)"
fi
if [ "$dry" = 0 ]; then
	# Opened read-only, as every lab script opens it: fs.protected_regular refuses O_CREAT on another
	# user's file in /run/lock, even for root.
	[ -e "$lock" ] || (umask 022 && : >>"$lock")
	exec 9<"$lock"
	flock -n 9 || die "another lab script holds $lock"
else
	say "dry run: nothing is changed"
fi

# --- undo -----------------------------------------------------------------------------------------
if [ "$undo" = 1 ]; then
	[ ! -f "$packages_before" ] ||
		die "an earlier setup stopped while installing packages: run it again to record them, then --undo"
	[ -f "$state" ] || { say "nothing recorded in $state: nothing to undo"; exit 0; }
	packages=()
	while read -r kind value; do
		case $kind in
		package) packages+=("$value") ;;
		file)
			run rm -f "$value"
			case $value in
			"$exports_file") ! command -v exportfs >/dev/null || run exportfs -ra ;;
			"$rules_dir"/*) ! command -v udevadm >/dev/null || run udevadm control --reload-rules ;;
			esac
			;;
		dir) run rmdir --ignore-fail-on-non-empty "$value" ;;
		started) run systemctl stop "$value" ;;
		ufw) read -ra words <<<"$value" && run ufw delete allow "${words[@]}" ;;
		l4t) say "no longer exported: $value" ;;
		*) die "unknown line in $state: $kind $value" ;;
		esac
	done < <(tac "$state")
	# Purge what setup installed and is still here; refuse if apt would take anything else with it.
	mapfile -t packages < <(for p in "${packages[@]}"; do ! is_installed "$p" || echo "$p"; done)
	if [ ${#packages[@]} -gt 0 ]; then
		mapfile -t purged < <(apt-get -s purge "${packages[@]}" | awk '/^Purg /{print $2}' | sort)
		mapfile -t wanted < <(printf '%s\n' "${packages[@]}" | sort)
		[ "${purged[*]}" = "${wanted[*]}" ] ||
			die "apt would purge '${purged[*]}', not only what setup installed ('${wanted[*]}'); purge by hand"
		run apt-get -o DPkg::Lock::Timeout=120 purge -y "${packages[@]}"
		# exportfs's lock files outlive the package that owned their directory.
		if [[ " ${packages[*]} " == *" nfs-common "* ]]; then
			run rm -f /var/lib/nfs/.etab.lock /var/lib/nfs/export-lock
			[ ! -d /var/lib/nfs ] || run rmdir --ignore-fail-on-non-empty /var/lib/nfs
		fi
	fi
	run rm -f "$state"
	run rmdir --ignore-fail-on-non-empty "$state_dir"
	say "undone"
	exit 0
fi

# --- setup: everything that could refuse, before any change ----------------------------------------
# The trees, canonical: NFS compares real paths, and the container mounts each at the same path.
l4ts=()
for t in "${trees[@]}"; do
	r=$(realpath -e "$t") || die "no such directory: $t"
	[ "$r" = "$t" ] || die "give the canonical path $r, not $t (the container mounts it at the same path)"
	[[ $r =~ ^/[A-Za-z0-9._/+-]+$ ]] || die "unusual characters in $r; keep exported paths plain"
	for f in flash.sh tools/kernel_flash/l4t_initrd_flash.sh tools/backup_restore/l4t_backup_restore.sh rootfs; do
		[ -e "$r/$f" ] || die "$r is not a prepared Linux_for_Tegra tree (no $f)"
	done
	l4ts+=("$r")
done
# NVIDIA's initrd flash talks IPv6 on the USB link; it cannot work with IPv6 off.
[ "$(sysctl -n net.ipv6.conf.all.disable_ipv6)" = 0 ] ||
	die "IPv6 is disabled (net.ipv6.conf.all.disable_ipv6=1): NVIDIA's initrd flash needs it"
for name in "${!rule_sha[@]}"; do
	src=${l4ts[0]}/tools/kernel_flash/host_udev/$name
	echo "${rule_sha[$name]}  $src" | sha256sum -c --quiet - 2>/dev/null ||
		die "$src is not the NVIDIA rule this script knows"
	dst=$rules_dir/$name
	[ ! -e "$dst" ] || cmp -s "$src" "$dst" || die "$dst exists and differs from NVIDIA's; not replacing it"
done
for f in "$tmpfiles_conf" "$exports_file"; do ours "$f" || die "$f exists and was not made here"; done
command -v docker >/dev/null || say "warning: no docker here; the flash container needs it"

# --- setup: the changes ---------------------------------------------------------------------------
# 1. The NFS server. The package list from before the install stays on disk until what it added
# is recorded, so an interrupted install is still undone in full after a rerun.
installed() { dpkg-query -W -f='${Package} ${Status}\n' | awk '/ok installed/{print $1}' | sort; }
if is_installed nfs-kernel-server; then
	[ -f "$packages_before" ] || say "nfs-kernel-server already installed"
elif [ "$dry" = 1 ]; then
	say "would install nfs-kernel-server and: $(apt-get -s install --no-install-recommends nfs-kernel-server 2>/dev/null |
		awk '/^Inst /{print $2}' | grep -vx nfs-kernel-server | xargs)"
else
	[ -d "$state_dir" ] || install -d -m 0755 "$state_dir"
	[ -f "$packages_before" ] || installed >"$packages_before"
	apt-get -o DPkg::Lock::Timeout=120 update -qq
	apt-get -o DPkg::Lock::Timeout=120 install -y --no-install-recommends nfs-kernel-server
fi
if [ "$dry" = 0 ] && [ -f "$packages_before" ]; then
	while read -r p; do record package "$p"; done < <(comm -13 "$packages_before" <(installed))
	rm -f "$packages_before"
fi

# 2. NVIDIA's udev rules, copied as they are.
mkdirs "$rules_dir"
reload_udev=0
for name in "${!rule_sha[@]}"; do
	dst=$rules_dir/$name
	if [ -e "$dst" ]; then
		recorded file "$dst" || say "$dst already present; left as it is"
		continue
	fi
	run install -m 0644 "${l4ts[0]}/tools/kernel_flash/host_udev/$name" "$dst"
	record file "$dst"
	reload_udev=1
done
[ "$reload_udev" = 0 ] || ! command -v udevadm >/dev/null || run udevadm control --reload-rules

# 3. The marker, made at every boot.
mkdirs "$(dirname "$tmpfiles_conf")"
if [ ! -e "$tmpfiles_conf" ]; then
	put "$tmpfiles_conf" 0644 <<-EOF
		# NVIDIA's l4t_initrd_flash.sh, run in the fjarr-l4t-flash container with this host's network,
		# expects this host's NFS server while this marker exists. Made by flash/host-setup.sh,
		# removed by its --undo (docs/32-fleet-ota.md#jetson-test-image).
		d $marker_dir 0755 root root -
		f $marker 0644 root root -
	EOF
	record file "$tmpfiles_conf"
fi
[ -d "$marker_dir" ] || record dir "$marker_dir"
[ -e "$marker" ] || record file "$marker"
run systemd-tmpfiles --create "$tmpfiles_conf"

# 4. The exports: the trees recorded before, and these.
all=()
if [ -f "$state" ]; then mapfile -t all < <(awk '$1 == "l4t" {print $2}' "$state"); fi
for r in "${l4ts[@]}"; do
	[[ " ${all[*]} " == *" $r "* ]] || all+=("$r") # paths hold no spaces (checked above)
done
for r in "${all[@]}"; do
	# NVIDIA's tool makes images/ on its first run; it has to exist to be exported.
	[ -d "$r/tools/kernel_flash/images" ] || run install -d -m 0755 "$r/tools/kernel_flash/images"
done
mkdirs "$(dirname "$exports_file")"
{
	echo "# The ZED Box Mini's flash initrd mounts these over the USB link (fc00:1:1::/48), read-write"
	echo "# as NVIDIA's tools export them. Made by flash/host-setup.sh, removed by its --undo."
	for r in "${all[@]}"; do
		for d in rootfs tools/kernel_flash/images tools/backup_restore; do echo "$r/$d $net($opts)"; done
	done
} | put "$exports_file" 0644
record file "$exports_file"
for r in "${l4ts[@]}"; do record l4t "$r"; done
run exportfs -ra
if [ "$dry" = 1 ]; then
	say "would start nfs-server if it is not running"
elif ! systemctl is-active --quiet nfs-server; then
	systemctl start nfs-server
	record started nfs-server
fi

# 5. The firewall, only where ufw is on.
# ufw prints its rules after the status line: read it whole, not through grep -q in a pipeline.
ufw_status=$(! command -v ufw >/dev/null || ufw status 2>/dev/null || true)
nfs_rule="proto tcp from $net to any port 2049"
if [ "$dry" = 1 ] && command -v ufw >/dev/null && [ -z "$ufw_status" ]; then
	# ufw status needs root; a dry run as a user cannot tell whether the firewall is on.
	say "would run, if ufw is active (unreadable without root): ufw allow $nfs_rule comment fjarr-l4t-flash"
elif grep -qx 'Status: active' <<<"$ufw_status"; then
	if recorded ufw "$nfs_rule"; then
		say "ufw already allows NFS from $net"
	else
		read -ra words <<<"$nfs_rule"
		run ufw allow "${words[@]}" comment fjarr-l4t-flash
		record ufw "$nfs_rule"
	fi
fi

# --- check that it happened -----------------------------------------------------------------------
if [ "$dry" = 1 ]; then
	say "would check: showmount -e localhost lists every export, $marker exists, both rules are in place"
	exit 0
fi
shown=$(showmount -e localhost) || die "the NFS server does not answer (showmount -e localhost)"
for r in "${all[@]}"; do
	for d in rootfs tools/kernel_flash/images tools/backup_restore; do
		awk -v p="$r/$d" '$1 == p {f = 1} END {exit !f}' <<<"$shown" || die "not exported: $r/$d"
	done
done
[ -e "$marker" ] || die "$marker is missing"
for name in "${!rule_sha[@]}"; do [ -e "$rules_dir/$name" ] || die "$rules_dir/$name is missing"; done
say "ready: ${#all[@]} tree(s) exported to $net; undo with: $0 --undo"
