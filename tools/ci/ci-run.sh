#!/usr/bin/env bash
# Finds the ci.yml run whose packages a workflow reuses (docs/30-continuous-integration.md#artifacts-built-once).
# Prints "<run-id> <sha>".
#
#   ci-run.sh --sha <sha> [--wait <minutes>]   that commit's push run; waits while it is still
#                                              running; fails if it is red or holds no packages
#   ci-run.sh --latest-main                    the newest green push run on main holding debs-amd64
# Needs GH_TOKEN and GITHUB_REPOSITORY.
set -euo pipefail
repo=${GITHUB_REPOSITORY:?}
api() { gh api -H "Accept: application/vnd.github+json" "$@"; }

has_debs() { # run-id arch...: every arch's debs artifact is there and not expired
  local run=$1; shift
  local names; names=$(api "repos/$repo/actions/runs/$run/artifacts?per_page=100" --jq '.artifacts[] | select(.expired|not) | .name')
  for a in "$@"; do printf '%s\n' "$names" | grep -qx "debs-$a" || return 1; done
}

case "${1:-}" in
  --sha)
    sha=${2:?}; wait_min=0
    [ "${3:-}" = --wait ] && wait_min=${4:?}
    deadline=$(( $(date +%s) + wait_min * 60 ))
    while :; do
      run=$(api "repos/$repo/actions/workflows/ci.yml/runs?head_sha=$sha&event=push&per_page=1" --jq '.workflow_runs[0] | "\(.id) \(.status) \(.conclusion)"')
      if [ -n "$run" ] && [ "$run" != "null null null" ]; then
        read -r id status conclusion <<<"$run"
        if [ "$status" = completed ]; then
          [ "$conclusion" = success ] || { echo "ci-run: CI for $sha is $conclusion (run $id): nothing to release" >&2; exit 1; }
          has_debs "$id" amd64 arm64 || { echo "ci-run: CI run $id for $sha holds no packages (a docs-only commit, or older than their 14 days): tag the commit that set the version, or a later code commit" >&2; exit 1; }
          echo "$id $sha"; exit 0
        fi
        echo "ci-run: CI for $sha is $status (run $id); waiting" >&2
      else
        echo "ci-run: no CI run for $sha yet; waiting" >&2
      fi
      [ "$(date +%s)" -lt "$deadline" ] || { echo "ci-run: gave up waiting for CI on $sha" >&2; exit 1; }
      sleep 30
    done ;;
  --latest-main)
    for row in $(api "repos/$repo/actions/workflows/ci.yml/runs?branch=main&event=push&status=success&per_page=30" --jq '.workflow_runs[] | "\(.id):\(.head_sha)"'); do
      if has_debs "${row%%:*}" amd64; then echo "${row%%:*} ${row#*:}"; exit 0; fi
    done
    echo "ci-run: no green main run with packages among the last 30" >&2; exit 1 ;;
  *) sed -n '2,10p' "$0"; exit 2 ;;
esac
