#!/usr/bin/env bash
# docker/lab/netem.sh — apply or clear a docs/25 media-path network profile on the
# demo robot's interface (the `tc netem` half of a NETWORK_PROFILE; the browser
# half is CDP and lives in web/e2e). fjarr-opsim cannot run tc itself (the dev
# user has no CAP_NET_ADMIN), so `make opsim-netem` wraps the scenario with this.
#
# spec: docs/25-browser-lab.md#the-harness (profile table)
# spec: docs/23-agent-core-architecture.md#slices-3a-3b-3c-and-their-gates (netem-<profile>)
# The profile → netem-args table mirrors web/e2e/src/profiles.ts NETWORK_PROFILES[*].media.netem.
#
#   netem.sh apply <profile>   profile ∈ lan wifi-ok 4g lossy bad offline (lan = clear)
#   netem.sh clear             delete the root qdisc (no error when none is set)
#   netem.sh show              print the interface's qdisc
#
# Runs `tc` as root in the demo-robot service (compose gives it NET_ADMIN).
#
# NETEM_DEV is a space-separated device list (default eth0, the browser lab's
# media path). fjarr-opsim shares the robot's network namespace, so its media
# is routed over `lo` even when addressed to the eth0 address and a qdisc on
# eth0 never sees it: `make opsim-netem` passes NETEM_DEV="lo eth0". On `lo`
# the impairment goes on band 2 of a prio qdisc and a u32 filter keeps the
# introspection port (7381, docs/24) on band 1, so the scenario can still read
# /stats and /pipelines while the media is degraded.
set -euo pipefail

cd "$(dirname "$0")/../.."

SERVICE=${NETEM_SERVICE:-demo-robot}
DEVS=${NETEM_DEV:-eth0}

usage() {
    echo "usage: $0 apply <lan|wifi-ok|4g|lossy|bad|offline> | clear | show" >&2
    exit 2
}

netem_args() {
    case "$1" in
        lan) echo "" ;;
        wifi-ok) echo "delay 10ms 3ms" ;;
        4g) echo "delay 40ms 10ms rate 8mbit" ;;
        lossy) echo "loss 5% delay 30ms 15ms" ;;
        bad) echo "loss 15% delay 100ms 40ms rate 1.5mbit" ;;
        offline) echo "loss 100%" ;;
        *) return 1 ;;
    esac
}

INTROSPECT_PORT=${NETEM_INTROSPECT_PORT:-7381}

tc_in_robot() {
    docker compose exec --user root -T "$SERVICE" tc "$@"
}

# shellcheck disable=SC2086 # $2 is a tc word list on purpose
apply_dev() {
    local dev=$1 args=$2
    # The impairment sits on a prio band the introspection port is filtered out of, on every device:
    # opsim and the harness keep reading /stats through a bad link (docs/25).
    tc_in_robot qdisc replace dev "$dev" root handle 1: prio bands 3 priomap 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1
    tc_in_robot qdisc replace dev "$dev" parent 1:2 handle 20: netem $args
    tc_in_robot filter replace dev "$dev" parent 1: protocol ip prio 1 handle 800::1 u32 match ip dport "$INTROSPECT_PORT" 0xffff flowid 1:1
    tc_in_robot filter replace dev "$dev" parent 1: protocol ip prio 1 handle 800::2 u32 match ip sport "$INTROSPECT_PORT" 0xffff flowid 1:1
}

# Before deleting, print what the netem qdisc saw (packets through it and dropped
# by it): the proof that the impairment was in the path of the traffic under test.
clear_qdisc() {
    for dev in $DEVS; do
        tc_in_robot -s qdisc show dev "$dev" 2>/dev/null | awk -v dev="$dev" '
            /^qdisc netem/ { netem = 1; next }
            netem && /Sent/ { printf "netem: %s saw %s pkt, dropped %s\n", dev, $4, $7; netem = 0 }' | tr -d '(),' || true
        tc_in_robot qdisc del dev "$dev" root 2>/dev/null || true
    done
}

case "${1:-}" in
    apply)
        profile=${2:-}
        [ -n "$profile" ] || usage
        args=$(netem_args "$profile") || { echo "netem.sh: unknown profile '$profile'" >&2; usage; }
        if [ -z "$args" ]; then
            clear_qdisc
            echo "netem: $profile on $SERVICE ($DEVS): no qdisc"
        else
            for dev in $DEVS; do
                apply_dev "$dev" "$args"
            done
            echo "netem: $profile on $SERVICE ($DEVS): $args"
        fi
        ;;
    clear)
        clear_qdisc
        echo "netem: cleared on $SERVICE ($DEVS)"
        ;;
    show)
        for dev in $DEVS; do
            tc_in_robot qdisc show dev "$dev"
        done
        ;;
    *)
        usage
        ;;
esac
