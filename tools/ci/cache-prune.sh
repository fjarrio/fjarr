#!/usr/bin/env bash
# One entry per cache (docs/30-continuous-integration.md#caches): for each key prefix, keep the
# newest entry on main and delete the others. Run after the jobs that save, on pushes to main.
#
#   cache-prune.sh <prefix>...     needs GH_TOKEN with actions:write, and GITHUB_REPOSITORY
set -euo pipefail
repo=${GITHUB_REPOSITORY:?}
for prefix in "$@"; do
  ids=$(gh cache list -R "$repo" --key "$prefix" --ref refs/heads/main -L 100 --sort created_at --order desc --json id --jq '.[].id')
  keep=$(printf '%s\n' "$ids" | head -1)
  n=0
  for id in $(printf '%s\n' "$ids" | tail -n +2); do
    gh cache delete -R "$repo" "$id" && n=$((n + 1))
  done
  echo "cache-prune: $prefix — kept ${keep:-nothing}, deleted $n"
done
