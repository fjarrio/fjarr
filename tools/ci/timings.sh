#!/usr/bin/env bash
# A CI run's jobs and every step over 20 s (docs/30-continuous-integration.md#measuring).
#   timings.sh [run-id]     the newest ci.yml run when no id is given
set -euo pipefail
run=${1:-$(gh run list --workflow ci.yml -L 1 --json databaseId --jq '.[0].databaseId')}
gh api "repos/{owner}/{repo}/actions/runs/$run" --jq '"run \(.id) \(.head_sha[0:7]) \(.event) \(.conclusion // .status): \(((.updated_at|fromdate)-(.run_started_at|fromdate))/60*10|floor/10) min wall"'
gh api "repos/{owner}/{repo}/actions/runs/$run/jobs?per_page=100" --jq '
  .jobs[] | select(.started_at != null and .completed_at != null) |
  "\(((.completed_at|fromdate)-(.started_at|fromdate))/60*10|floor/10) min  \(.name)  [\(.conclusion)]",
  (.steps[] | select(.started_at != null and .completed_at != null) | select(((.completed_at|fromdate)-(.started_at|fromdate)) > 20) |
   "      \((.completed_at|fromdate)-(.started_at|fromdate))s  \(.name)")'
