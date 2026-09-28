#!/bin/sh
# Installs Fjarr's .debs on a clean Ubuntu 26.04 (as apt would on a robot, Recommends included)
# and checks what docs/26#packages promises. Exits non-zero naming the first broken promise.
set -eu
export DEBIAN_FRONTEND=noninteractive
fail() { echo "FAIL: $*"; exit 1; }
ok() { echo "ok   $*"; }
apt-get update -qq >/dev/null
cp /debs/*.deb /tmp/ && apt-get install -y -qq /tmp/*.deb >/tmp/apt.log 2>&1 || { tail -20 /tmp/apt.log; fail "apt could not install the packages"; }
ok "apt installed: $(ls /debs/*.deb | xargs -n1 basename | tr '\n' ' ')"

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
[ ! -e /etc/fjarr/fjarr.toml ] || fail "the package shipped /etc/fjarr/fjarr.toml; setup writes it"; ok "no config shipped (setup writes it)"

[ -f /usr/share/fjarr/viewer/index.html ] || fail "the viewer is missing"; ok "viewer installed"
getcap /usr/bin/fjarr-connect | grep -q 'cap_net_admin=p' || fail "fjarr-connect lacks cap_net_admin: $(getcap /usr/bin/fjarr-connect)"
ok "fjarr-connect holds cap_net_admin"

cfg=/usr/share/fjarr/fjarr.toml.example
[ -f "$cfg" ] || fail "no example config"
FJARR_MEDIA_ENCODER=software fjarr-agent --config "$cfg" --check >/tmp/check.log 2>&1 || { cat /tmp/check.log; fail "--check fails on the example config"; }
ok "--check passes on the example config (software encoder: no GPU here)"

apt-get install -y -qq gstreamer1.0-tools >/dev/null 2>&1   # the test's own tool, after the packages
for e in x264enc avdec_h264; do gst-inspect-1.0 "$e" >/dev/null 2>&1 && fail "$e came with the packages (ADR-0011)"; done
gpl=$(dpkg-query -W -f '${Package} ' 'libx264-*' 'libxvidcore*' 2>/dev/null || true)
[ -z "$gpl" ] || fail "GPL codec libraries came with the packages: $gpl"
ok "no GPL codec in reach of the agent (ADR-0011)"
echo "deb-install-test: all promises kept"
