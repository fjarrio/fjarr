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
# A real Ubuntu has systemd-tmpfiles, so the package's tmpfiles step runs at install time; without it
# that step is skipped and a /run/fjarr the package failed to create went unnoticed (0.1.2, found on
# the mini-PC). The standalone build is the one a container without systemd can have.
apt-get install -y -qq systemd-standalone-tmpfiles >/dev/null 2>&1 || fail "could not install systemd-standalone-tmpfiles"
cp /debs/fjarr-agent_*.deb /debs/fjarr-setup_*.deb /debs/fjarr-tools_*.deb /debs/fjarr-desktop-session_*.deb /debs/fjarr-desktop-wayland_*.deb /debs/fjarr-desktop-x11_*.deb /tmp/ && apt-get install -y -qq /tmp/*.deb >/tmp/apt.log 2>&1 || { tail -20 /tmp/apt.log; fail "apt could not install the packages"; }
ok "apt installed: $(ls /tmp/*.deb | xargs -n1 basename | tr '\n' ' ')"
# The package creates /run/fjarr for the agent at install (tmpfiles), which needs the fjarr user to
# exist first (sysusers) — debhelper orders the two the other way round unless the postinst does it.
[ "$(stat -c '%U %a' /run/fjarr 2>/dev/null)" = "fjarr 755" ] || fail "/run/fjarr was not created at install, owned by fjarr: $(stat -c '%U %a' /run/fjarr 2>&1)"
ok "/run/fjarr created at install, owned by fjarr (sysusers before tmpfiles)"

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
fjarr-agent net --help 2>&1 | grep -q '^Usage: fjarr-setup net' || fail "fjarr-agent does not hand 'net' to fjarr-setup"
fjarr-agent display --help 2>&1 | grep -q '^Usage: fjarr-setup display' || fail "fjarr-agent does not hand 'display' to fjarr-setup"
# Root here, no config yet: the tool must refuse and name the fix, not "set up" a device that is not configured.
fjarr-agent net setup --yes --ros no 2>&1 | grep -q 'does not exist.*fjarr-agent setup' || fail "net setup without a config did not name setup as the fix"
ok "fjarr-agent hands setup/net/drivers/display to fjarr-setup"
[ ! -e /etc/fjarr/fjarr.toml ] || fail "the package shipped /etc/fjarr/fjarr.toml; setup writes it"; ok "no config shipped (setup writes it)"

# fjarr-desktop-wayland and fjarr-desktop-session (docs/26#packages): install, switch nothing on.
# setup desktop does that.
[ -f /usr/lib/fjarr/desktop/libfjarr-desktop-mutter.so ] || fail "no backend module E in /usr/lib/fjarr/desktop"
# No session bus here, so it exits 1 at once ("no session bus"); 127 would be a library it cannot load.
rc=0; /usr/lib/fjarr/fjarr-desktop-session >/tmp/helper.log 2>&1 </dev/null || rc=$?
grep -q 'no session bus' /tmp/helper.log || fail "fjarr-desktop-session does not run (exit $rc): $(cat /tmp/helper.log)"
getent group fjarr-desktop >/dev/null || fail "no fjarr-desktop group (sysusers)"
id -nG fjarr | tr ' ' '\n' | grep -qx fjarr-desktop || fail "the agent's account is not in fjarr-desktop: $(id -nG fjarr)"
[ -f /usr/lib/systemd/user/fjarr-desktop-session.service ] || fail "no user unit for the session helper"
[ -z "$(find /etc/systemd/user -name 'fjarr-desktop-session.service' 2>/dev/null)" ] || fail "the helper's user unit is enabled for every user; setup desktop enables it for one account"
[ -f /usr/lib/systemd/system/fjarr-desktop-watchdog.timer ] || fail "no watchdog timer"
[ ! -e /etc/systemd/system/timers.target.wants/fjarr-desktop-watchdog.timer ] || fail "the watchdog is enabled by the package; setup desktop enables it"
[ -L /usr/bin/fjarr-setup ] && [ "$(readlink -f /usr/bin/fjarr-setup)" = /usr/lib/fjarr/fjarr-setup ] || fail "no fjarr-setup command (fjarr-setup package)"
ok "fjarr-desktop-wayland + fjarr-desktop-session: module, helper, group with fjarr in it, user unit and watchdog installed, nothing enabled"

# fjarr-desktop-x11 (docs/26#packages): the module and the session program, its autostart entry inert.
[ -f /usr/lib/fjarr/desktop/libfjarr-desktop-x11.so ] || fail "no backend module A in /usr/lib/fjarr/desktop"
# No X server here, so it exits 1 at once ("cannot open the display"); 127 would be a library it cannot load.
rc=0; DISPLAY=:9 /usr/lib/fjarr/fjarr-x11-session >/tmp/x11s.log 2>&1 </dev/null || rc=$?
grep -q 'cannot open the display' /tmp/x11s.log || fail "fjarr-x11-session does not run (exit $rc): $(cat /tmp/x11s.log)"
[ -f /usr/share/fjarr/fjarr-x11-session.desktop ] || fail "no autostart entry for fjarr-x11-session under /usr/share/fjarr"
[ ! -e /etc/xdg/autostart/fjarr-x11-session.desktop ] || fail "the kiosk session's autostart entry is enabled by the package; setup desktop links it"
ok "fjarr-desktop-x11: module, session program and its inert autostart entry installed, nothing enabled"

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

# The driver catalog and `drivers` (docs/26#fjarr-agent-drivers): shipped, and the built-in entries only.
[ -f /usr/share/fjarr/catalog.toml ] || fail "no driver catalog installed"
fjarr-agent drivers list >/tmp/drivers.log 2>&1 || { cat /tmp/drivers.log; fail "drivers list failed"; }
for d in test v4l2 rtsp; do grep -q "^  $d  *built in" /tmp/drivers.log || { cat /tmp/drivers.log; fail "drivers list lacks the built-in $d"; }; done
fjarr-agent drivers list --json | grep -q '"status": "built-in"' || fail "drivers list --json is not JSON with statuses"
# gstreamer1.0-plugins-base-apps is a dependency: detection has its tool on every robot. No camera here.
fjarr-agent drivers detect | grep -q 'no video sources found' || fail "drivers detect did not run (gst-device-monitor-1.0 missing?)"
fjarr-agent drivers install v4l2 | grep -q 'built into fjarr-agent' || fail "drivers install v4l2 did not say it is built in"
fjarr-agent drivers install nosuch >/dev/null 2>&1 && fail "drivers install of an unknown entry succeeded"
ok "catalog shipped; drivers list/detect/install answer for the built-in entries"

# `setup` (docs/26#fjarr-agent-setup), scripted. Against a server that is not there it fails before
# writing anything and names the fix; with --offline it writes the config the way the agent reads it.
if fjarr-agent setup --yes --server ws://127.0.0.1:9/ws --device-id test-device --token t0k --cameras none --net no >/tmp/setup.log 2>&1; then
    cat /tmp/setup.log; fail "setup against no server succeeded"
fi
grep -q 'not reachable' /tmp/setup.log && grep -q -- '--offline' /tmp/setup.log || { cat /tmp/setup.log; fail "setup against no server did not name the fix"; }
[ ! -e /etc/fjarr/fjarr.toml ] || fail "setup wrote the config although it failed"
ok "setup against no server fails cleanly, names --offline, writes nothing"
FJARR_DEVICE_TOKEN=t0k fjarr-agent setup --yes --offline --server ws://127.0.0.1:9/ws --device-id test-device --cameras none --net no >/tmp/setup.log 2>&1 || { cat /tmp/setup.log; fail "setup --offline failed"; }
[ "$(stat -c '%a %U:%G' /etc/fjarr/fjarr.toml)" = "640 root:fjarr" ] || fail "config is not 0640 root:fjarr: $(stat -c '%a %U:%G' /etc/fjarr/fjarr.toml)"
grep -q '^robot_id = "test-device"' /etc/fjarr/fjarr.toml || fail "config lacks robot_id"
grep -q '^server_url = "ws://127.0.0.1:9/ws"' /etc/fjarr/fjarr.toml || fail "config lacks server_url"
grep -q '^dev_token = "t0k"' /etc/fjarr/fjarr.toml || fail "config lacks the token from FJARR_DEVICE_TOKEN"
# No GPU here: setup must have chosen the software encoder, or the agent would refuse to start (docs/23).
grep -q '^encoder = "software"' /etc/fjarr/fjarr.toml || { cat /etc/fjarr/fjarr.toml; fail "setup did not choose the software encoder without VA-API"; }
grep -q 'systemd is not running' /tmp/setup.log || { cat /tmp/setup.log; fail "setup did not say the agent was not started (no systemd here)"; }
grep -q '^check: OK' /tmp/setup.log || { cat /tmp/setup.log; fail "setup did not end with a passing --check"; }
grep -q '"path": "/etc/fjarr/fjarr.toml"' /var/lib/fjarr/setup-changes.json || fail "setup did not record the config for --undo"
[ "$(stat -c '%a %U' /var/lib/fjarr)" = "700 fjarr" ] || fail "/var/lib/fjarr is not 0700 fjarr"
ok "setup --offline wrote the config (0640 root:fjarr, keys the agent reads, software encoder), recorded it, --check passed"
# The agent reads what setup wrote: the doctor from the file alone, no environment.
fjarr-agent --config /etc/fjarr/fjarr.toml --check >/tmp/check.log 2>&1 || { cat /tmp/check.log; fail "--check fails on setup's config"; }
grep -q '^profile core' /tmp/check.log || { cat /tmp/check.log; fail "--check printed no profile rows"; }
ok "after setup, --check passes with every profile row ok"
# Without sudo, --check cannot see inside /etc/fjarr: it must say so, not call the config missing
# (the mini-PC, 2026-09-30: the operator went looking for a config that was there).
setpriv --reuid=65534 --regid=65534 --clear-groups fjarr-agent --check >/tmp/check-user.log 2>&1 || true
if grep -q 'fjarr.toml .*MISSING' /tmp/check-user.log || ! grep -q 'permission denied.*sudo fjarr-agent --check' /tmp/check-user.log; then
    cat /tmp/check-user.log; fail "--check as an ordinary user called the config missing instead of saying it cannot see it"
fi
ok "--check without root says it cannot see the config, and to check as root"
# setup desktop (docs/26#fjarr-agent-setup-desktop), as far as a container goes: no GDM or systemd
# here, so the test gives it Ubuntu's own custom.conf and checks every file, account and key it
# writes, then that --undo desktop puts each back.
mkdir -p /etc/gdm3
printf '# GDM configuration storage\n\n[daemon]\n#WaylandEnable=false\n#  AutomaticLoginEnable = true\n#  AutomaticLogin = user1\n\n[security]\n' >/etc/gdm3/custom.conf
cp /etc/gdm3/custom.conf /tmp/custom.conf.orig
fjarr-agent setup desktop --yes --account desktop --reboot no >/tmp/desktop.log 2>&1 || { cat /tmp/desktop.log; fail "setup desktop failed"; }
getent passwd desktop >/dev/null || fail "setup desktop did not create the account"
passwd -S desktop | awk '{print $2}' | grep -q '^L' || fail "the desktop account's password is not locked: $(passwd -S desktop)"
id -nG desktop | tr ' ' '\n' | grep -qx fjarr-desktop || fail "desktop is not in fjarr-desktop"
grep -q '^AutomaticLoginEnable=true$' /etc/gdm3/custom.conf && grep -q '^AutomaticLogin=desktop$' /etc/gdm3/custom.conf || { cat /etc/gdm3/custom.conf; fail "no automatic login for desktop in GDM"; }
grep -q '^\[security\]' /etc/gdm3/custom.conf || fail "setup desktop dropped the rest of custom.conf"
grep -q '^lock-enabled=false' /etc/dconf/db/local.d/00-fjarr-desktop || fail "no screen-lock setting"
grep -q '^system-db:local' /etc/dconf/profile/user || fail "the dconf profile does not read the local database"
link=/home/desktop/.config/systemd/user/graphical-session.target.wants/fjarr-desktop-session.service
[ "$(readlink "$link")" = /usr/lib/systemd/user/fjarr-desktop-session.service ] || fail "the helper's user unit is not enabled for desktop: $(ls -la "$link" 2>&1)"
[ "$(stat -c %U /home/desktop/.config/systemd)" = desktop ] || fail "setup made ~desktop/.config/systemd owned by $(stat -c %U /home/desktop/.config/systemd), not desktop"
[ -z "$(find /etc/systemd/user -name 'fjarr-desktop-session.service' 2>/dev/null)" ] || fail "the helper got enabled for every account"
grep -A3 '^\[capabilities."fjarr.desktop".helper\]' /etc/fjarr/fjarr.toml | grep -q '^user = "desktop"' || { cat /etc/fjarr/fjarr.toml; fail "the agent's config does not name the desktop account"; }
grep -q '^check: OK' /tmp/desktop.log || { cat /tmp/desktop.log; fail "setup desktop did not end with a passing --check"; }
ok "setup desktop: account (locked, in fjarr-desktop), GDM auto-login, no screen lock, helper for that account only, agent config; --check passed"
fjarr-agent setup --undo desktop >/tmp/undo-desktop.log 2>&1 || { cat /tmp/undo-desktop.log; fail "setup --undo desktop failed"; }
! getent passwd desktop >/dev/null || fail "--undo desktop left the account it created"
cmp -s /etc/gdm3/custom.conf /tmp/custom.conf.orig || { diff /tmp/custom.conf.orig /etc/gdm3/custom.conf; fail "--undo desktop did not restore custom.conf"; }
[ ! -e /etc/dconf/db/local.d/00-fjarr-desktop ] && [ ! -e /etc/dconf/profile/user ] || fail "--undo desktop left the dconf files"
! grep -q 'fjarr.desktop' /etc/fjarr/fjarr.toml || { cat /etc/fjarr/fjarr.toml; fail "--undo desktop left the desktop keys in the config"; }
grep -q '^robot_id = "test-device"' /etc/fjarr/fjarr.toml || fail "--undo desktop touched setup's own keys"
ok "setup --undo desktop removed the account, restored custom.conf, removed the dconf files and the config keys, kept the rest"
# setup desktop --x11 (docs/26#fjarr-agent-setup-desktop): the kiosk session's autostart entry and
# the agent's backend; the display manager and its automatic login stay the operator's.
fjarr-agent setup desktop --yes --x11 --display :7 >/tmp/x11.log 2>&1 || { cat /tmp/x11.log; fail "setup desktop --x11 failed"; }
[ "$(readlink /etc/xdg/autostart/fjarr-x11-session.desktop)" = /usr/share/fjarr/fjarr-x11-session.desktop ] || fail "setup desktop --x11 did not link the kiosk session's autostart entry"
grep -q '^backend = "x11"' /etc/fjarr/fjarr.toml && grep -q '^display = ":7"' /etc/fjarr/fjarr.toml || { cat /etc/fjarr/fjarr.toml; fail "the agent's config does not name backend A on :7"; }
cmp -s /etc/gdm3/custom.conf /tmp/custom.conf.orig || fail "setup desktop --x11 touched the display manager"
grep -q '^check: OK' /tmp/x11.log || { cat /tmp/x11.log; fail "setup desktop --x11 did not end with a passing --check"; }
fjarr-agent setup --undo desktop >/tmp/undo-x11.log 2>&1 || { cat /tmp/undo-x11.log; fail "setup --undo desktop (x11) failed"; }
[ ! -e /etc/xdg/autostart/fjarr-x11-session.desktop ] || fail "--undo desktop left the autostart entry"
! grep -q 'fjarr.desktop' /etc/fjarr/fjarr.toml || { cat /etc/fjarr/fjarr.toml; fail "--undo desktop left the x11 keys in the config"; }
ok "setup desktop --x11: autostart entry linked, backend A on the display, display manager untouched, --check passed; --undo desktop removed both"
fjarr-agent setup --undo >/tmp/undo.log 2>&1 || { cat /tmp/undo.log; fail "setup --undo failed"; }
[ ! -e /etc/fjarr/fjarr.toml ] || fail "--undo left the config in place"
grep -q '"changes": \[\]' /var/lib/fjarr/setup-changes.json || fail "--undo left changes recorded: $(cat /var/lib/fjarr/setup-changes.json)"
FJARR_MEDIA_ENCODER=software fjarr-agent --config "$cfg" --check 2>&1 | grep -q 'MISSING.*sudo fjarr-agent setup' || fail "after --undo, --check does not name setup again"
ok "setup --undo removed the config and the record; --check names setup again"

for e in x264enc avdec_h264; do gst-inspect-1.0 "$e" >/dev/null 2>&1 && fail "$e came with the packages (ADR-0011)"; done
gpl=$(dpkg-query -W -f '${Package} ' 'libx264-*' 'libxvidcore*' 2>/dev/null || true)
[ -z "$gpl" ] || fail "GPL codec libraries came with the packages: $gpl"
ok "no GPL codec in reach of the agent (ADR-0011)"
echo "deb-install-test: all promises kept"
