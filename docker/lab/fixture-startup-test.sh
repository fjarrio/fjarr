#!/usr/bin/env bash
# docker/lab/fixture-startup-test.sh — the desktop fixtures' startup contract, under fault
# (docs/15#the-desktop-test-lab): a fixture whose WirePlumber loses the start race recovers and
# says so, and a fixture with no session manager fails its start loudly instead of declaring itself
# ready with no default output (six CI jobs from 2026-10-03 to 10-14 ran on such a fixture).
#
#   fixture-startup-test.sh gnome|x11
#
# Each case starts the fixture's image with FIXTURE_FAULT, a test-only knob of its session script:
#   wireplumber-first   WirePlumber starts a second before PipeWire, so its first connect fails
#                       (status 69). The fixture must log that exit, restart WirePlumber and become
#                       ready, with WirePlumber running and a default output. Then, as under
#                       Restart=on-failure, a WirePlumber killed (SIGKILL) must come back with the
#                       default output, and one stopped (SIGTERM) must stay stopped.
#                       The suites' dump of the fixture's internals must work on it, and leave it
#                       healthy.
#   pipewire-late       The usual order, but PipeWire starts a second late. Only the wait for
#                       PipeWire keeps WirePlumber from losing the race: the fixture must become
#                       ready with no WirePlumber exit at all. Then, as under systemd's start limit,
#                       a WirePlumber killed 5 times within 10 s must be given up, and say so.
#   no-session-manager  WirePlumber never starts. The fixture must exit 1 without becoming ready,
#                       saying "no default output", that WirePlumber is not running, and the tails
#                       of WirePlumber's and PipeWire's logs.
#   no-pipewire         PipeWire never starts. The fixture must exit 1 without becoming ready,
#                       saying that PipeWire never answered.
# One PASS/FAIL line per check; exit status 1 on any failure. Plain `docker run` with no compose
# project and no bind mount, so it never touches the fixtures the suites use, and its containers'
# names carry the run's pid, so two runs on one host do not remove each other's. Each case's container
# is removed once its output is captured, so the next case does not start beside a running session. FIXTURE_IMAGE tests
# another build of the image (a fault planted in its script, to see a check fail). Host-run (CLAUDE.md).
#
# spec: docs/15-testing-strategy.md#the-desktop-test-lab
set -euo pipefail

kind=${1:?usage: fixture-startup-test.sh gnome|x11}
case "$kind" in
gnome)
  image=${FIXTURE_IMAGE:-fjarr-desktop-fixture} prefix=fixture logs=/run/desktop
  start=("$image") # its own entrypoint; the session belongs to the image's user, desktop
  as=()
  dump=(fixture-session dump) # as the image's user, as the suites run it
  ;;
x11)
  image=${FIXTURE_IMAGE:-fjarr-robot-sim} prefix=robot-sim logs=/run/fixture
  # The kiosk fixture insists on fjarr-x11-session from a built tree (make agent-build). A stand-in
  # plays it here: the contract under test is the kiosk's PipeWire and WirePlumber, not the program
  # that hands the sound to an agent, and the stand-in lets this run where the agent is not built.
  session=/workspace/build/release/agent/fjarr-x11-session
  start=(--entrypoint bash -e SIM_FIXTURE=1 "$image" -c "install -d ${session%/*} &&
    printf '#!/bin/sh\necho \"stand-in session program: granted nothing\"\nexec sleep infinity\n' >$session &&
    chmod 0755 $session && exec start.sh")
  as=(-u kiosk -e XDG_RUNTIME_DIR=/run/kiosk)
  dump=(start.sh dump) # as root, as the suites run it
  ;;
*)
  echo "usage: fixture-startup-test.sh gnome|x11" >&2
  exit 2
  ;;
esac
docker image inspect "$image" >/dev/null 2>&1 || { echo "fixture-startup-test: no $image image (make desktop-fixture-test builds it)"; exit 1; }

fails=0
check() { # name ok detail
  if [ "$2" = 1 ]; then echo "PASS $kind $1: $3"; else echo "FAIL $kind $1: $3"; fails=$((fails + 1)); fi
}
names=()
trap '[ ${#names[@]} -eq 0 ] || docker rm -f "${names[@]}" >/dev/null 2>&1 || true' EXIT

# Starts the fixture with a fault, as fjarr-fixture-startup-<kind>-<fault>-<pid>.
launch() {
  local name="fjarr-fixture-startup-$kind-$1-$$"
  docker rm -f "$name" >/dev/null 2>&1 || true
  names+=("$name")
  docker run -d --name "$name" -e FIXTURE_FAULT="$1" "${start[@]}" >/dev/null
}

# Waits, at most $2 s, until the container is healthy or has exited; prints which and when.
settle() {
  local state health t0=$SECONDS
  for _ in $(seq $(($2 * 2))); do
    state=$(docker inspect -f '{{.State.Status}}' "$1")
    health=$(docker inspect -f '{{if .State.Health}}{{.State.Health.Status}}{{end}}' "$1")
    if [ "$state" = exited ] || [ "$health" = healthy ]; then
      echo "${state/running/$health} after $((SECONDS - t0)) s"
      return
    fi
    sleep 0.5
  done
  echo "neither healthy nor exited after $2 s"
}

in_fixture() { docker exec "${as[@]}" "$1" "${@:2}"; }
# The fixture's output lines matching $1. Captured first: `logs | grep -q` fails under pipefail when
# grep exits early and logs gets SIGPIPE.
logged() { local out; out=$(docker logs "$c" 2>&1); grep -m1 "$1" <<<"$out"; }
# Runs "$@" until it succeeds, at most 10 s.
eventually() { for _ in $(seq 50); do "$@" && return 0; sleep 0.2; done; return 1; }

# 1. The start race, lost on purpose: recovered, and logged.
c=fjarr-fixture-startup-$kind-wireplumber-first-$$ before=$fails
launch wireplumber-first
verdict=$(settle "$c" 60)
out=$(docker logs "$c" 2>&1)
check ready "$([[ $verdict == healthy* ]] && echo 1 || echo 0)" "$verdict"
line=$(grep -m1 "^$prefix: wireplumber exited with status 69 " <<<"$out" || true)
check race-logged "$([ -n "$line" ] && echo 1 || echo 0)" "${line:-no WirePlumber exit with status 69 in the fixture output}"
wplog=$(docker exec "$c" cat "$logs/wireplumber.log" 2>&1 || true)
check race-in-wireplumber-log "$(grep -q "Failed to connect to PipeWire" <<<"$wplog" && grep -q "wireplumber exited with status 69 " <<<"$wplog" && echo 1 || echo 0)" \
  "$logs/wireplumber.log has WirePlumber's own error and the fixture's exit line"
check restarted "$(in_fixture "$c" pgrep -x wireplumber >/dev/null 2>&1 && echo 1 || echo 0)" "$(in_fixture "$c" pgrep -ax wireplumber 2>&1 || echo "not running")"
sink=$(in_fixture "$c" wpctl inspect @DEFAULT_AUDIO_SINK@ 2>/dev/null | grep -m1 'node.name' || true)
check default-output "$([ -n "$sink" ] && echo 1 || echo 0)" "${sink:-none}"
# The dump a failed suite prints: the running WirePlumber and the links, with the fixture left as it was.
dumped=$(timeout 60 docker exec "$c" "${dump[@]}" 2>&1 || true)
line=$(grep -m1 -E "^$prefix: wireplumber: [0-9]+ " <<<"$dumped" || true)
health=$(docker inspect -f '{{.State.Health.Status}}' "$c")
check dump "$([ -n "$line" ] && grep -q -- "^--- pw-link -l" <<<"$dumped" && grep -q -- "^--- wpctl status" <<<"$dumped" &&
  [ "$health" = healthy ] && echo 1 || echo 0)" "${line:-no running WirePlumber in the dump}; the fixture $health after it"
# Later on, as under Restart=on-failure: a crash is restarted, with the default output back, and a
# clean stop is not.
old=$(in_fixture "$c" pgrep -x wireplumber || true)
renewed() { local p; p=$(in_fixture "$c" pgrep -x wireplumber) && [ "$p" != "$old" ] && in_fixture "$c" wpctl inspect @DEFAULT_AUDIO_SINK@ >/dev/null 2>&1; }
in_fixture "$c" pkill -KILL -x wireplumber || true
line=$(eventually logged "^$prefix: wireplumber exited with status 137 " >/dev/null && eventually renewed &&
  logged "^$prefix: wireplumber exited with status 137 " || true)
check crash-restarted "$([ -n "$line" ] && echo 1 || echo 0)" "${line:-no new WirePlumber with a default output within 10 s of a SIGKILL}"
in_fixture "$c" pkill -TERM -x wireplumber || true
line=$(eventually logged "^$prefix: wireplumber exited with status 0 .*not restarted" >/dev/null && sleep 1 &&
  ! in_fixture "$c" pgrep -x wireplumber >/dev/null && logged "^$prefix: wireplumber exited with status 0 " || true)
check clean-stop-stays "$([ -n "$line" ] && echo 1 || echo 0)" "${line:-a SIGTERM was not logged as a clean exit, or WirePlumber came back}"
out=$(docker logs "$c" 2>&1)
[ $fails -eq "$before" ] || { echo "--- $c's output"; echo "$out"; }
docker rm -f "$c" >/dev/null 2>&1 || true

# 2. PipeWire slow to listen, in the usual order: the wait holds WirePlumber back, so it never exits.
c=fjarr-fixture-startup-$kind-pipewire-late-$$ before=$fails
launch pipewire-late
verdict=$(settle "$c" 60)
out=$(docker logs "$c" 2>&1)
check late-ready "$([[ $verdict == healthy* ]] && echo 1 || echo 0)" "$verdict"
line=$(grep -m1 "^$prefix: wireplumber exited" <<<"$out" || true)
check waited "$([ -z "$line" ] && in_fixture "$c" pgrep -x wireplumber >/dev/null 2>&1 && echo 1 || echo 0)" \
  "${line:-WirePlumber started once PipeWire answered, and never exited}"
# Ready, then a crash loop: five kills, each of a new WirePlumber, are given up on, not restarted forever.
last=""
fresh() { local p; p=$(in_fixture "$c" pgrep -x wireplumber) && [ "$p" != "$last" ] && last=$p; }
for _ in 1 2 3 4 5; do
  eventually fresh || break
  in_fixture "$c" pkill -KILL -x wireplumber || true
done
line=$(eventually logged "^$prefix: wireplumber: start limit hit" >/dev/null && sleep 1 &&
  ! in_fixture "$c" pgrep -x wireplumber >/dev/null && logged "^$prefix: wireplumber: start limit hit" || true)
check start-limit "$([ -n "$line" ] && echo 1 || echo 0)" "${line:-WirePlumber killed 5 times within 10 s was not given up on}"
out=$(docker logs "$c" 2>&1)
[ $fails -eq "$before" ] || { echo "--- $c's output"; echo "$out"; }
docker rm -f "$c" >/dev/null 2>&1 || true

# 3. No session manager at all: the fixture fails its start, loudly, and is never ready.
c=fjarr-fixture-startup-$kind-no-session-manager-$$ before=$fails
launch no-session-manager
verdict=$(settle "$c" 60)
out=$(docker logs "$c" 2>&1)
code=$(docker inspect -f '{{.State.ExitCode}}' "$c")
check fails-start "$([[ $verdict == exited* ]] && [ "$code" = 1 ] && echo 1 || echo 0)" "$verdict, exit status $code"
ready=$(grep -m1 "^$prefix: .* ready" <<<"$out" || true)
check never-ready "$([ -z "$ready" ] && echo 1 || echo 0)" "${ready:-no ready line}"
why=$(grep -E "^$prefix: (no default output|wireplumber: )" <<<"$out" | paste -sd ';' - || true)
check says-why "$(grep -q "^$prefix: no default output" <<<"$out" && grep -q "^$prefix: wireplumber: not running" <<<"$out" && echo 1 || echo 0)" \
  "${why:-no default-output message}"
check shows-logs "$(grep -q -- "--- $logs/wireplumber.log" <<<"$out" && grep -q -- "--- $logs/pipewire.log" <<<"$out" && echo 1 || echo 0)" \
  "the tails of $logs/wireplumber.log and $logs/pipewire.log in the fixture's output"
[ $fails -eq "$before" ] || { echo "--- $c's output"; echo "$out"; }
docker rm -f "$c" >/dev/null 2>&1 || true

# 4. No PipeWire: the fixture fails its start, saying so, and is never ready.
c=fjarr-fixture-startup-$kind-no-pipewire-$$ before=$fails
launch no-pipewire
verdict=$(settle "$c" 60)
out=$(docker logs "$c" 2>&1)
code=$(docker inspect -f '{{.State.ExitCode}}' "$c")
why=$(grep -m1 "^$prefix: .*PipeWire never answered" <<<"$out" || true)
check no-pipewire-fails "$([[ $verdict == exited* ]] && [ "$code" = 1 ] && [ -n "$why" ] && ! grep -q "^$prefix: .* ready" <<<"$out" && echo 1 || echo 0)" \
  "$verdict, exit status $code: ${why:-no 'PipeWire never answered'}"
[ $fails -eq "$before" ] || { echo "--- $c's output"; echo "$out"; }
docker rm -f "$c" >/dev/null 2>&1 || true

[ $fails -eq 0 ]
