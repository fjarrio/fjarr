#!/bin/sh
# The fjarr-agent image's entrypoint (docs/26#containerized-robots, docs/27#lifecycle).
#
# Started as root — the reference compose file's tunnel setup — it does what an apt device's
# fjarr-net.service does at every boot: `fjarr-setup net up` creates fjarr0 owned by `fjarr` (a
# no-op unless fjarr.net is enabled). Then it drops to `fjarr` with no capabilities at all before
# the agent runs, so the agent never holds CAP_NET_ADMIN (docs/27, rule 1). The setup tool itself
# (`setup`, `net`, `drivers`) stays root: it writes /etc/fjarr and creates the device.
# Started as `fjarr` (the image's default user) it runs the agent directly.
set -eu

if [ "$(id -u)" != 0 ]; then
    exec fjarr-agent "$@"
fi

case "${1:-}" in
setup | net | drivers) exec fjarr-agent "$@" ;;
esac

if [ -f /etc/fjarr/fjarr.toml ]; then
    /usr/lib/fjarr/fjarr-setup net up
fi

# fjarr's own groups (video, render from the package) and the ones compose added with group_add —
# the host's render GID, which differs per host (docs/26). `--init-groups` alone would drop those.
groups=$({ id -G fjarr; id -G; } | tr ' ' '\n' | grep -vx 0 | sort -un | paste -sd, -)
exec setpriv --reuid=fjarr --regid=fjarr --groups="$groups" --inh-caps=-all --bounding-set=-all -- fjarr-agent "$@"
