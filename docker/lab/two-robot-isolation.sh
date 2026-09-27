#!/usr/bin/env bash
# Two robots attached to one operator interface cannot reach each other, in either direction.
#
# This is a docs/15 safety-class regression: it is never removed, because the property it checks is
# the whole reason the operator's interface carries /32 routes rather than a subnet. The isolation is
# structural — no code holds two links, and each link checks a packet against its own pair of
# addresses (docs/27#isolation) — and structural properties are exactly the ones that quietly stop
# holding.
#
# Run with the link already up, as `fjarr-connect <a> <b> -- docker/lab/two-robot-isolation.sh`.
# FJARR_ADDRS carries both addresses, in the order they were attached.
#
#   positive control  each robot's own service answers from the operator
#   negative          neither robot's service answers from the other robot
#
# The positive control is not decoration: without it, a test that finds nothing proves nothing — a
# broken link and a working isolation rule look identical from the far side (the lesson of slice
# 4.5d's first isolation test, which passed while measuring the lab's own bridge).
#
# spec: docs/27-network-tunnel.md#isolation · docs/15-testing-strategy.md
set -uo pipefail

: "${FJARR_ADDRS:?FJARR_ADDRS is not set — run this through fjarr-connect with two robots}"
read -r -a ADDRS <<<"$FJARR_ADDRS"
[ "${#ADDRS[@]}" -eq 2 ] || { echo "two-robot-isolation: need exactly two robots, got ${#ADDRS[@]}" >&2; exit 2; }
A=${ADDRS[0]}
B=${ADDRS[1]}
# The containers whose network namespaces hold those addresses, in the same order.
SVC_A=${FJARR_LAB_SVC_A:-demo-robot}
SVC_B=${FJARR_LAB_SVC_B:-demo-robot-2}
# Every robot serves the introspection endpoint on its own namespace (docs/24), which makes it the
# one service both ends are guaranteed to have.
PORT=${FJARR_LAB_PORT:-7381}
TOKEN=${FJARR_INTROSPECT_TOKEN:-dev-only-introspect-token}
TIMEOUT=${FJARR_LAB_TIMEOUT:-6}

cd "$(dirname "$0")/../.."
fails=0
pass() { echo "two-robot-isolation: PASS $*"; }
fail() { echo "two-robot-isolation: FAIL $*" >&2; fails=$((fails + 1)); }

# From the operator's own namespace, over the link.
from_operator() {
  curl -s -o /dev/null -m "$TIMEOUT" -w '%{http_code}' \
    -H "Authorization: Bearer $TOKEN" "http://$1:$PORT/stats" 2>/dev/null
}
# From inside a robot, which is where a packet aimed at the other robot would start.
from_robot() {
  docker compose exec -T "$1" sh -c \
    "curl -s -o /dev/null -m $TIMEOUT -w '%{http_code}' -H 'Authorization: Bearer $TOKEN' http://$2:$PORT/stats 2>/dev/null" 2>/dev/null
}

echo "two-robot-isolation: A=$A ($SVC_A)  B=$B ($SVC_B)"

for pair in "$A $SVC_A" "$B $SVC_B"; do
  set -- $pair
  code=$(from_operator "$1")
  if [ "$code" = "200" ]; then
    pass "the operator reaches $1 ($2) over its own link"
  else
    fail "the operator cannot reach $1 ($2) — got '${code:-nothing}', so this run proves nothing about isolation"
  fi
done

# The two negatives, made as hard as the lab can make them. A robot has no route to another robot's
# tunnel address, so the naive version of this check watches the packet leave down eth0 and calls the
# timeout isolation — which proves nothing at all, and is the same mistake slice 4.5d's first
# isolation test made. So the route is forced into the tunnel first, and verified to be there, before
# the attempt is believed.
#
# What refuses it, with the route forced, is the sending robot's **own** agent: its outbound rule
# requires a packet's destination to be the operator, and another robot's address is not
# (docs/27#isolation). The operator's mirror of that rule is the second line, and no honest agent can
# produce the packet that would exercise it live — that one is covered by unit tests in all three
# implementations (`NetPolicy.*`, `policy::tests`).
force_route() { docker compose exec -T -u root "$1" ip route replace "$2/32" dev "${FJARR_LAB_DEV:-fjarr0}" >/dev/null 2>&1; }
drop_forced_route() { docker compose exec -T -u root "$1" ip route del "$2/32" dev "${FJARR_LAB_DEV:-fjarr0}" >/dev/null 2>&1 || true; }
routed_via_tunnel() {
  docker compose exec -T "$1" sh -c "ip route get $2 2>/dev/null" 2>/dev/null | grep -q "dev ${FJARR_LAB_DEV:-fjarr0}"
}

for pair in "$SVC_A $B" "$SVC_B $A"; do
  set -- $pair
  svc=$1
  target=$2
  if routed_via_tunnel "$svc" "$target"; then
    fail "$svc already routes $target down the tunnel before this test forced it — the lab is not in the state this test assumes"
  fi
  force_route "$svc" "$target"
  if ! routed_via_tunnel "$svc" "$target"; then
    fail "could not make $svc route $target into the tunnel, so the attempt would not have been carried by it"
    drop_forced_route "$svc" "$target"
    continue
  fi
  code=$(from_robot "$svc" "$target")
  if [ "$code" = "200" ]; then
    fail "$svc reached $target through the tunnel — the links are joined, which is the one thing this interface must never do"
  else
    pass "$svc cannot reach $target even with the route forced into the tunnel (got '${code:-nothing}')"
  fi
  drop_forced_route "$svc" "$target"
done

if [ "$fails" -ne 0 ]; then
  echo "two-robot-isolation: $fails check(s) failed" >&2
  exit 1
fi
echo "two-robot-isolation: all checks passed — both robots reachable from the operator, neither reachable from the other even with the route forced into the tunnel"
