#!/bin/sh
# One-time spike-machine setup for the ADR-0028 handover spike (run with sudo). Creates the agent's
# system account and the fjarr-desktop group from ADR-0028's decision, with the desktop account as
# the group's only member. Throwaway spike code.
set -eu
DESKTOP_USER=${1:-fjarr-spike}
getent group fjarr-desktop >/dev/null || groupadd --system fjarr-desktop
# The spike machine's login user is already called fjarr, so the agent's account is fjarr-agent here.
getent passwd fjarr-agent >/dev/null || \
  useradd --system --no-create-home --home-dir /nonexistent --shell /usr/sbin/nologin fjarr-agent
usermod -aG fjarr-desktop "$DESKTOP_USER"
install -d -m 0755 /opt/fjarr-spike/helper
install -m 0644 "$(dirname "$0")"/agent.py "$(dirname "$0")"/helper.py /opt/fjarr-spike/helper/
id fjarr-agent; getent group fjarr-desktop
