#!/bin/bash
# Test host-setup.sh for real without touching a real host: in a throwaway Ubuntu 26.04 container
# (the mini-PC's release), with stand-ins for what a container lacks: systemctl, udevadm,
# systemd-tmpfiles, a running NFS server's showmount, and ufw. Package installs and exportfs are
# real. Checks the dry run changes nothing; the nightly window, a held lab lock and a rule that
# differs from NVIDIA's are refused; setup installs, writes and records; a second run changes
# nothing and a second tree only adds exports; --undo returns the container to its first state,
# packages included, and keeps what was there before setup (a package, a rule).
# spec: docs/32-fleet-ota.md#jetson-test-image
#
#   test-host-setup.sh [<Linux_for_Tegra>]   needs docker, and network for apt; prints PASS/FAIL lines
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
if [ "${1:-}" != --inside ]; then
	l4t=$(realpath "${1:-$(dirname "$here")/out/flash/l4t-36.4.4/Linux_for_Tegra}")
	exec docker run --rm --sysctl net.ipv6.conf.all.disable_ipv6=0 \
		-v "$here:/flash:ro" -v "$l4t/tools/kernel_flash/host_udev:/rules:ro" \
		ubuntu:26.04@sha256:c098726dd9b9733f3e885f8de0ef375206a311b0a317eadc40d347b1115fb944 \
		bash /flash/test-host-setup.sh --inside
fi

# --- inside the container -------------------------------------------------------------------------
fails=0
pass() { echo "PASS $*"; }
fail() { echo "FAIL $*"; fails=$((fails + 1)); }
check() { # check <name> <command...>; a failure shows the command's last lines
	local name=$1 out
	shift
	if out=$("$@" 2>&1); then pass "$name"; else
		fail "$name"
		tail -5 <<<"$out" | sed 's/^/    /'
	fi
}
setup=/flash/host-setup.sh

# What the mini-PC has and a bare container lacks: sysctl (procps), the rules and tmpfiles.d
# directories, and an ordinary user.
apt-get update -qq >/dev/null
apt-get install -y -qq --no-install-recommends procps >/dev/null 2>&1
mkdir -p /etc/udev/rules.d /etc/tmpfiles.d
useradd -m robot

# Stand-ins, first on PATH. Each logs its calls.
fakes=/opt/fakes
mkdir -p $fakes
cat >$fakes/systemctl <<'EOF'
#!/bin/bash
echo "systemctl $*" >>/tmp/calls
case $1 in
is-active) [ -e /tmp/nfs-active ] ;;
start) touch /tmp/nfs-active ;;
stop) rm -f /tmp/nfs-active ;;
esac
EOF
cat >$fakes/udevadm <<'EOF'
#!/bin/bash
echo "udevadm $*" >>/tmp/calls
EOF
cat >$fakes/systemd-tmpfiles <<'EOF'
#!/bin/bash
# The d and f lines of the given file, as systemd-tmpfiles --create makes them.
echo "systemd-tmpfiles $*" >>/tmp/calls
while read -r type path mode _; do
	case $type in
	d) install -d -m "$mode" "$path" ;;
	f) [ -e "$path" ] || install -m "$mode" /dev/null "$path" ;;
	esac
done < <(grep -E '^[df] ' "${*: -1}")
EOF
cat >$fakes/showmount <<'EOF'
#!/bin/bash
# What the running server would list: the exports exportfs has loaded.
echo "Export list for localhost:"
awk '{print $1, $2}' /var/lib/nfs/etab 2>/dev/null | sed 's/(.*//'
EOF
cat >$fakes/ufw <<'EOF'
#!/bin/bash
# Prints its rules after the status line, as ufw does.
echo "ufw $*" >>/tmp/calls
case $1 in
status)
	if [ -e /tmp/ufw-on ]; then
		printf '%s\n' "Status: active" "" "To                         Action      From" \
			"--                         ------      ----" "22/tcp                     ALLOW       Anywhere"
	else
		echo "Status: inactive"
	fi
	;;
esac
EOF
cat >$fakes/date <<'EOF'
#!/bin/bash
# The time of day a test asks for, else the real date.
if [ "$*" = "+%H%M" ] && [ -e /tmp/hhmm ]; then cat /tmp/hhmm; else exec /usr/bin/date "$@"; fi
EOF
chmod +x $fakes/*
export PATH=$fakes:$PATH
echo 1200 >/tmp/hhmm # every run happens at noon, unless a check says otherwise

# Two fake trees holding what host-setup.sh checks, with NVIDIA's real rules.
tree() {
	mkdir -p "$1/tools/kernel_flash/host_udev" "$1/tools/backup_restore" "$1/rootfs"
	touch "$1/flash.sh" "$1/tools/kernel_flash/l4t_initrd_flash.sh" "$1/tools/backup_restore/l4t_backup_restore.sh"
	cp /rules/*.rules "$1/tools/kernel_flash/host_udev/"
}
t1=/srv/l4t-a/Linux_for_Tegra t2=/srv/l4t-b/Linux_for_Tegra
tree $t1
tree $t2

snapshot() { # packages, every path outside apt's and dpkg's own records, and what /etc holds
	dpkg-query -W -f='${Package} ${Version} ${Status}\n' | sort
	find /etc /run /var/lib /usr/local -xdev \( -path /var/lib/apt -o -path /var/lib/dpkg -o \
		-path /run/lock/fjarr-zedbox.lock -o -path /var/lib/systemd \) -prune -o -print 2>/dev/null | sort
	find /etc -xdev -type f -exec sha256sum {} + | sort -k2
}
# What apt itself leaves after installing and purging: the service users nfs-common and rpcbind
# add stay (Debian keeps system users), adduser's /run/adduser (gone at reboot), and ldconfig's
# rewritten cache.
residue='/etc/(passwd|group|shadow|gshadow)-?$|/etc/ld\.so\.cache$|/run/adduser$'
changes() { # changes <before> <after>: what differs, apt's residue left out
	diff "$1" "$2" | grep -E '^[<>]' | grep -vE "$residue" || true
}
snapshot >/tmp/s0

# 1. A dry run as an ordinary user changes nothing.
check "dry run works without root" su robot -c "$setup --dry-run --l4t $t1"
snapshot >/tmp/s1
check "dry run changes nothing" cmp /tmp/s0 /tmp/s1

# 2. Refusals, each changing nothing.
refuses() { # refuses <message part> <args...>: host-setup.sh fails, saying that
	local want=$1 out
	shift
	! out=$($setup "$@" 2>&1) && grep -q "$want" <<<"$out"
}
echo 0300 >/tmp/hhmm
check "refuses inside the nightly window" refuses "nightly lab window" --l4t $t1
echo 2357 >/tmp/hhmm
check "refuses just before it" refuses "nightly lab window" --l4t $t1
echo 0615 >/tmp/hhmm
check "refuses in fjarr-lab's grace after it" refuses "nightly lab window" --l4t $t1
echo 1200 >/tmp/hhmm
flock /run/lock/fjarr-zedbox.lock sleep 3 & # another lab script, for three seconds
sleep 0.5
check "refuses while the lab lock is held" refuses "holds /run/lock/fjarr-zedbox.lock" --l4t $t1
wait
rm /run/lock/fjarr-zedbox.lock # setup makes it again below
echo '# not NVIDIA' >/etc/udev/rules.d/99-l4t-host.rules
echo 0630 >/tmp/hhmm # from 06:30 the window lets it through, to the next refusal
check "refuses a rule that differs from NVIDIA's, at 06:30" refuses "differs from NVIDIA's" --l4t $t1
echo 1200 >/tmp/hhmm
rm /etc/udev/rules.d/99-l4t-host.rules
check "no state after refusals" test ! -e /var/lib/fjarr-flash-host/state
snapshot >/tmp/s2
diff /tmp/s0 /tmp/s2 | grep -E '^[<>]' >/tmp/d2 || true
if [ -s /tmp/d2 ]; then fail "refusals change nothing:"; sed 's/^/    /' /tmp/d2; else pass "refusals change nothing"; fi

# 3. Setup.
check "setup runs" $setup --l4t $t1
check "lab lock file made, mode 0644" test "$(stat -c %a /run/lock/fjarr-zedbox.lock)" = 644
check "nfs-kernel-server installed" dpkg-query -W nfs-kernel-server
check "rules are NVIDIA's" cmp /rules/10-l4t-usb-msd.rules /etc/udev/rules.d/10-l4t-usb-msd.rules
check "marker made by tmpfiles.d" test -e /run/nvidia_initrd_flash/docker_host_network
exports() { awk -F'\t' '$2 ~ /^fc00:1:1::\/48\(/' /var/lib/nfs/etab | wc -l; } # etab: path TAB client(options)
check "three exports loaded, to fc00:1:1::/48 only" test "$(exports)" = 3
check "exports read-write, root not squashed" grep -q "$t1/rootfs.*(rw,.*no_root_squash" /var/lib/nfs/etab
check "packages recorded" grep -qx "package nfs-kernel-server" /var/lib/fjarr-flash-host/state
check "nfs-server started and recorded" grep -qx "started nfs-server" /var/lib/fjarr-flash-host/state
cp /var/lib/fjarr-flash-host/state /tmp/state1
snapshot >/tmp/s3

# 4. A second run changes nothing; a second tree only adds exports; ufw gets its rule.
check "second run" $setup --l4t $t1
check "second run records nothing new" cmp /tmp/state1 /var/lib/fjarr-flash-host/state
snapshot >/tmp/s4
check "second run changes no file" cmp /tmp/s3 /tmp/s4
touch /tmp/ufw-on
check "second tree" $setup --l4t $t2
check "both trees exported" test "$(exports)" = 6
check "ufw rule added for NFS from fc00:1:1::/48" grep -q "ufw allow proto tcp from fc00:1:1::/48 to any port 2049" /tmp/calls

# 5. Undo: a dry run first, then for real.
cp -a /var/lib/fjarr-flash-host/state /tmp/state2
check "dry-run undo" $setup --dry-run --undo
check "dry-run undo changes nothing" cmp /tmp/state2 /var/lib/fjarr-flash-host/state
check "undo runs" $setup --undo
check "ufw rule deleted" grep -q "ufw delete allow proto tcp from fc00:1:1::/48 to any port 2049" /tmp/calls
check "nfs-server stopped" test ! -e /tmp/nfs-active
snapshot >/tmp/s5
changes /tmp/s0 /tmp/s5 >/tmp/d5
if [ -s /tmp/d5 ]; then fail "undo returns to the first state:"; sed 's/^/    /' /tmp/d5; else pass "undo returns to the first state"; fi
echo "     apt's residue: $(diff /tmp/s0 /tmp/s5 | grep -E '^[<>]' | grep -oE "$residue" | sort -u | xargs)"
check "a second undo has nothing to do" bash -c "$setup --undo | grep -q 'nothing to undo'"

# 6. An install interrupted after apt but before the record: undo waits, a rerun records it.
mkdir -p /var/lib/fjarr-flash-host
dpkg-query -W -f='${Package} ${Status}\n' | awk '/ok installed/{print $1}' | sort >/var/lib/fjarr-flash-host/packages-before
apt-get install -y -qq --no-install-recommends nfs-kernel-server >/dev/null 2>&1
check "undo refuses while an install is unrecorded" bash -c "! $setup --undo"
check "a rerun records the interrupted install" $setup --l4t $t1
check "its packages recorded" grep -qx "package nfs-kernel-server" /var/lib/fjarr-flash-host/state
check "undo after the rerun" $setup --undo
check "the interrupted install purged" bash -c "! dpkg-query -W -f='\${Status}' nfs-kernel-server 2>/dev/null | grep -q 'ok installed'"

# 7. What was there before setup stays after undo: a package and an identical rule.
apt-get install -y -qq --no-install-recommends nfs-kernel-server >/dev/null 2>&1
cp /rules/10-l4t-usb-msd.rules /etc/udev/rules.d/
snapshot >/tmp/s6
check "setup over existing package and rule" $setup --l4t $t1
check "existing package not recorded" bash -c "! grep -q '^package ' /var/lib/fjarr-flash-host/state"
check "undo" $setup --undo
check "existing package kept" dpkg-query -W nfs-kernel-server
check "existing rule kept" test -e /etc/udev/rules.d/10-l4t-usb-msd.rules
snapshot >/tmp/s7
# The NFS server that stays keeps the lock files its first exportfs made.
changes /tmp/s6 /tmp/s7 | grep -vxE '> /var/lib/nfs/(\.etab\.lock|export-lock)' >/tmp/d7 || true
if [ -s /tmp/d7 ]; then fail "undo keeps the earlier state:"; sed 's/^/    /' /tmp/d7; else pass "undo keeps the earlier state"; fi
echo "     kept by the NFS server that stays: $(changes /tmp/s6 /tmp/s7 | grep -oE '/var/lib/nfs/.*' | xargs)"

echo "$fails failure(s)"
[ "$fails" = 0 ]
