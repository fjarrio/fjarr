#!/bin/sh
# make deb-install-test, second half: the host of a robot whose agent runs in a container
# (docs/26#a-desktop-in-a-container). fjarr-desktop-session and fjarr-setup alone, no agent: they
# install, `fjarr-setup setup desktop --container` does the host's side and prints what the
# container needs, the watchdog finds the account without an agent configuration, and --undo
# puts it all back. No GDM or systemd in a container: what is checked is every file, account and
# key the setup writes.
set -eu

fail() { echo "FAIL: $*"; exit 1; }
ok() { echo "ok   $*"; }

apt-get update -qq >/dev/null 2>&1
apt-get install -y -qq systemd-standalone-tmpfiles >/dev/null 2>&1 || fail "could not install systemd-standalone-tmpfiles"
cp /debs/fjarr-setup_*.deb /debs/fjarr-desktop-session_*.deb /tmp/ && apt-get install -y -qq --no-install-recommends /tmp/*.deb >/tmp/apt.log 2>&1 || { tail -20 /tmp/apt.log; fail "apt could not install the host packages"; }
! dpkg -s fjarr-agent >/dev/null 2>&1 || fail "fjarr-desktop-session brought the agent with it; a container host has none"
ok "fjarr-desktop-session and fjarr-setup installed alone, no agent"

[ -x /usr/bin/fjarr-setup ] || fail "no fjarr-setup command"
rc=0; /usr/lib/fjarr/fjarr-desktop-session >/tmp/helper.log 2>&1 </dev/null || rc=$?
grep -q 'no session bus' /tmp/helper.log || fail "fjarr-desktop-session does not run (exit $rc): $(cat /tmp/helper.log)"
getent group fjarr-desktop >/dev/null || fail "no fjarr-desktop group"
[ -f /usr/lib/systemd/user/fjarr-desktop-session.service ] || fail "no user unit for the session helper"
[ -f /usr/lib/systemd/system/fjarr-desktop-watchdog.timer ] || fail "no watchdog timer"
ok "the helper, its user unit, the group and the watchdog are on the host"

# The host's side, as far as a container goes: Ubuntu's own custom.conf stands in for GDM.
mkdir -p /etc/gdm3
printf '# GDM configuration storage\n\n[daemon]\n#  AutomaticLoginEnable = true\n#  AutomaticLogin = user1\n\n[security]\n' >/etc/gdm3/custom.conf
cp /etc/gdm3/custom.conf /tmp/custom.conf.orig
fjarr-setup setup desktop --container --yes --account desktop --reboot no >/tmp/host.log 2>&1 || { cat /tmp/host.log; fail "setup desktop --container failed"; }
getent passwd desktop >/dev/null || fail "no desktop account"
id -nG desktop | tr ' ' '\n' | grep -qx fjarr-desktop || fail "desktop is not in fjarr-desktop"
grep -q '^AutomaticLogin=desktop$' /etc/gdm3/custom.conf || fail "no automatic login for desktop"
[ -L /home/desktop/.config/systemd/user/graphical-session.target.wants/fjarr-desktop-session.service ] || fail "the helper is not enabled for desktop"
grep -q '^d /run/fjarr 0755 10001 10001' /etc/tmpfiles.d/fjarr-container.conf || fail "no /run/fjarr for the container's agent: $(cat /etc/tmpfiles.d/fjarr-container.conf 2>&1)"
[ "$(stat -c '%u' /run/fjarr 2>/dev/null)" = 10001 ] || fail "/run/fjarr is not uid 10001's: $(stat -c '%u %a' /run/fjarr 2>&1)"
[ ! -e /etc/fjarr/fjarr.toml ] || fail "setup wrote an agent configuration on a host with no agent"
uid=$(id -u desktop); gid=$(getent group fjarr-desktop | cut -d: -f3)
grep -q -- "--helper-uid $uid --helper-gid $gid" /tmp/host.log || { cat /tmp/host.log; fail "the container's command (--helper-uid $uid --helper-gid $gid) was not printed"; }
ok "setup desktop --container: account, automatic login, helper, /run/fjarr for uid 10001, no agent config; printed --helper-uid $uid --helper-gid $gid"

# The watchdog names the account from GDM: there is no agent configuration to read it from.
fjarr-setup desktop watchdog >/tmp/watchdog.log 2>&1 || true
! grep -q 'no desktop account configured' /tmp/watchdog.log || fail "the watchdog found no account on a container host: $(cat /tmp/watchdog.log)"
ok "the watchdog finds the account in GDM's configuration"

fjarr-setup setup --undo desktop >/tmp/undo.log 2>&1 || { cat /tmp/undo.log; fail "setup --undo desktop failed"; }
! getent passwd desktop >/dev/null || fail "--undo left the account"
cmp -s /etc/gdm3/custom.conf /tmp/custom.conf.orig || fail "--undo did not restore custom.conf"
[ ! -e /etc/tmpfiles.d/fjarr-container.conf ] || fail "--undo left the tmpfiles drop-in"
ok "setup --undo desktop: account, custom.conf and the tmpfiles drop-in back as they were"
echo "deb-install-test (container host): all promises kept"
