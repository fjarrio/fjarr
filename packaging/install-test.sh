#!/bin/sh
# Installs Fjarr's .debs on a clean Ubuntu 26.04 (as apt would on a robot, Recommends included)
# and checks what docs/26#packages promises. Exits non-zero naming the first broken promise.
set -eu
export DEBIAN_FRONTEND=noninteractive
fail() { echo "FAIL: $*"; exit 1; }
ok() { echo "ok   $*"; }
apt-get update -qq >/dev/null
# What a robot installs: the runtime packages. libfjarr-dev is for an embedder's build machine and has
# its own test (embed-test.sh); its GStreamer -dev dependencies bring FFmpeg's, which docs/14 records.
cp /debs/fjarr-agent_*.deb /debs/fjarr-tools_*.deb /tmp/ && apt-get install -y -qq /tmp/*.deb >/tmp/apt.log 2>&1 || { tail -20 /tmp/apt.log; fail "apt could not install the packages"; }
ok "apt installed: $(ls /tmp/*.deb | xargs -n1 basename | tr '\n' ' ')"

# --help exits 2 by design (usage); what matters is that it ran, so match its first line.
fjarr-agent --help 2>&1 | grep -q '^fjarr-agent \[' || fail "fjarr-agent does not run (a missing library?)"; ok "fjarr-agent runs"
# Deliberately in a container WITHOUT CAP_NET_ADMIN in its bounding set: with the capability
# permitted-only (+p), the binary must still run for everything that needs no privilege.
fjarr-connect --help >/dev/null 2>&1 || fail "fjarr-connect does not run (exit $?; 126 = the file capability blocks exec)"; ok "fjarr-connect runs without CAP_NET_ADMIN in the bounding set"

getent passwd fjarr >/dev/null || fail "no fjarr user (sysusers)"
for g in video render; do id -nG fjarr | tr ' ' '\n' | grep -qx "$g" || fail "fjarr is not in $g"; done
ok "user fjarr in: $(id -nG fjarr)"

unit=$(ls /usr/lib/systemd/system/fjarr-agent.service /lib/systemd/system/fjarr-agent.service 2>/dev/null | head -1)
[ -n "$unit" ] || fail "no fjarr-agent.service installed"
grep -q '^ConditionPathExists=/etc/fjarr/fjarr.toml' "$unit" || fail "the unit does not wait for /etc/fjarr/fjarr.toml"
[ -e /etc/systemd/system/multi-user.target.wants/fjarr-agent.service ] || fail "the service is not enabled"
ok "unit installed and enabled: $unit"
[ -f /usr/lib/tmpfiles.d/fjarr-agent.conf ] || fail "no tmpfiles entry for /run/fjarr"; ok "tmpfiles entry present"
# The tunnel's boot unit ships disabled: `net setup` enables it where fjarr.net is wanted (docs/26#packages).
netunit=$(ls /usr/lib/systemd/system/fjarr-net.service /lib/systemd/system/fjarr-net.service 2>/dev/null | head -1)
[ -n "$netunit" ] || fail "no fjarr-net.service installed"
grep -q '^Before=fjarr-agent.service' "$netunit" || fail "fjarr-net.service is not ordered before the agent"
[ ! -e /etc/systemd/system/multi-user.target.wants/fjarr-net.service ] || fail "fjarr-net.service is enabled by the package; net setup enables it"
ok "fjarr-net.service installed, not enabled"
# The setup tool, and the hand-off (docs/26#the-setup-tool): `fjarr-agent net …` is fjarr-setup's.
[ -x /usr/lib/fjarr/fjarr-setup ] || fail "no /usr/lib/fjarr/fjarr-setup"
fjarr-agent net --help 2>&1 | grep -q '^Usage: fjarr-setup net' || fail "fjarr-agent does not hand `net` to fjarr-setup"
# Root here, no config yet: the tool must refuse and name the fix, not "set up" a device that is not configured.
fjarr-agent net setup --yes --ros no 2>&1 | grep -q 'does not exist.*fjarr-agent setup' || fail "net setup without a config did not name setup as the fix"
ok "fjarr-agent hands setup/net/drivers to fjarr-setup"
[ ! -e /etc/fjarr/fjarr.toml ] || fail "the package shipped /etc/fjarr/fjarr.toml; setup writes it"; ok "no config shipped (setup writes it)"

[ -f /usr/share/fjarr/viewer/index.html ] || fail "the viewer is missing"; ok "viewer installed"
getcap /usr/bin/fjarr-connect | grep -q 'cap_net_admin=p' || fail "fjarr-connect lacks cap_net_admin: $(getcap /usr/bin/fjarr-connect)"
ok "fjarr-connect holds cap_net_admin"

cfg=/usr/share/fjarr/fjarr.toml.example
[ -f "$cfg" ] || fail "no example config"
[ -f /usr/share/fjarr/profile.toml ] || fail "no system profile installed"
# Fresh install, setup not run: the profile check must fail and name the fix (docs/26#the-system-profile).
if FJARR_MEDIA_ENCODER=software fjarr-agent --config "$cfg" --check >/tmp/check.log 2>&1; then
    cat /tmp/check.log; fail "--check passed before setup ran: the profile check is not checking"
fi
grep -q 'profile core    /etc/fjarr/fjarr.toml .*MISSING.*sudo fjarr-agent setup' /tmp/check.log || { cat /tmp/check.log; fail "--check did not name setup as the fix"; }
ok "before setup, --check fails and names the fix"
# What setup and the service's first start do (until setup exists): the config, and the state directory.
install -D -m 0644 "$cfg" /etc/fjarr/fjarr.toml
install -d -o fjarr -g fjarr -m 0700 /var/lib/fjarr
# And what boot does: /run is empty at every boot and systemd-tmpfiles recreates /run/fjarr. This
# container runs no systemd, so the step is done by hand here, the way the tmpfiles entry says.
install -d -o fjarr -g fjarr -m 0755 /run/fjarr
FJARR_MEDIA_ENCODER=software fjarr-agent --config /etc/fjarr/fjarr.toml --check >/tmp/check.log 2>&1 || { cat /tmp/check.log; fail "--check fails after setup's steps"; }
grep -q '^profile core' /tmp/check.log || { cat /tmp/check.log; fail "--check printed no profile rows"; }
ok "after setup's steps, --check passes with every profile row ok (software encoder: no GPU here)"

apt-get install -y -qq gstreamer1.0-tools >/dev/null 2>&1   # the test's own tool, after the packages
for e in x264enc avdec_h264; do gst-inspect-1.0 "$e" >/dev/null 2>&1 && fail "$e came with the packages (ADR-0011)"; done
gpl=$(dpkg-query -W -f '${Package} ' 'libx264-*' 'libxvidcore*' 2>/dev/null || true)
[ -z "$gpl" ] || fail "GPL codec libraries came with the packages: $gpl"
ok "no GPL codec in reach of the agent (ADR-0011)"
echo "deb-install-test: all promises kept"
