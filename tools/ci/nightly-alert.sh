#!/usr/bin/env bash
# A red night reaches someone (docs/30-continuous-integration.md#nightly-alert). Judges one night: the
# scheduled lab-desktop-prepare and lab-desktop-verify of the evening before (verify by its last
# attempt) and the Nightly of that date; without --night, the newest Nightly that is due, so the
# 05:30 check judges the night before's Nightly again, should its 12:30 check have been dropped. Keeps one issue labelled nightly-red open while a night is red,
# and closes it with a comment on the first green night.
#
#   nightly-alert.sh [--night YYYY-MM-DD] [--now YYYY-MM-DDTHH:MM:SSZ] [--dry-run]
#     --night    the night (default: the newest whose lab window has closed, at 05:30 UTC, and the
#                newest Nightly past its due time, 12:00 UTC)
#     --now      judge as a check would have then, from the runs' timestamps (default: now)
#     --dry-run  print the verdict and what it would do to the issue; change nothing
# Needs GH_TOKEN (actions: read, issues: write) and GITHUB_REPOSITORY. With FJARR_ALERT_FIXTURES=<dir>
# the API's answers come from files there and changes go to <dir>/writes.log, and FJARR_ALERT_CLOCK sets
# the time of a live check (tools/ci/test.sh).
set -euo pipefail
shopt -s inherit_errexit # a failed API call inside $(...) must stop the check, not drop a row
repo=${GITHUB_REPOSITORY:?}
root=$(cd "$(dirname "$0")/../.." && pwd)

night="" now_arg="" dry=0
while [ $# -gt 0 ]; do
  case "$1" in
    --night) night=${2:?--night needs a date}; shift 2 ;;
    --now) now_arg=${2:?--now needs a time}; shift 2 ;;
    --dry-run) dry=1; shift ;;
    *) sed -n '8,12p' "$0"; exit 2 ;;
  esac
done

api() { # GET, a path under the repository
  if [ -n "${FJARR_ALERT_FIXTURES:-}" ]; then
    local f=${1%%\?*}
    f="$FJARR_ALERT_FIXTURES/${f//\//_}.json"
    if [ -f "$f" ]; then cat "$f"; else return 1; fi
  else
    gh api -H "Accept: application/vnd.github+json" "repos/$repo/$1"
  fi
}
change() { # <method> <path> <gh api fields...>: nothing on a dry run
  [ "$dry" -eq 0 ] || return 0
  if [ -n "${FJARR_ALERT_FIXTURES:-}" ]; then echo "$1 $2" >> "$FJARR_ALERT_FIXTURES/writes.log"; return 0; fi
  gh api -X "$1" -H "Accept: application/vnd.github+json" "repos/$repo/$2" "${@:3}" >/dev/null
}
say() { if [ "$dry" -eq 1 ]; then echo "nightly-alert: would $*"; else echo "nightly-alert: $*"; fi; }
epoch() { jq -rn --arg t "$1" '$t | fromdateiso8601'; }
iso() { jq -rn --argjson s "$1" '$s | todate'; }

# The lab window is 00:00-06:00 in Stockholm, and a running job has 30 min more (docs/12): over by
# 05:30 UTC in summer and in winter. GitHub has started the 02:17 UTC Nightly up to 7 h late (docs/15).
LAB_DUE=$((5 * 3600 + 30 * 60))
NIGHTLY_DUE=$((12 * 3600))
if [ -n "$now_arg" ]; then now=$(epoch "$now_arg"); elif [ -n "${FJARR_ALERT_CLOCK:-}" ]; then now=$(epoch "$FJARR_ALERT_CLOCK"); else now=$(date -u +%s); fi
nightly_night=$night
if [ -z "$night" ]; then
  night=$(jq -rn --argjson s "$((now - LAB_DUE))" '$s | strftime("%Y-%m-%d")')
  nightly_night=$(jq -rn --argjson s "$((now - NIGHTLY_DUE))" '$s | strftime("%Y-%m-%d")')
fi

slot() { # <workflow file> <night's midnight>: its cron slot for that night; an evening cron belongs to the night after it
  local m h day=$2
  read -r m h < <(sed -n 's/^ *- cron: *"\([0-9]\{1,2\}\) \([0-9]\{1,2\}\) \* \* \*".*/\1 \2/p' "$root/.github/workflows/$1" | head -n 1) || true
  if [ -z "${h:-}" ]; then echo "nightly-alert: no daily cron in .github/workflows/$1" >&2; exit 1; fi
  if [ "$((10#$h))" -ge 12 ]; then echo $((day - 86400 + 10#$h * 3600 + 10#$m * 60)); else echo $((day + 10#$h * 3600 + 10#$m * 60)); fi
}

# shellcheck disable=SC2016 # the $names are jq's
JQ_LIB='
def t: fromdateiso8601;
def by($now): select((.created_at | t) <= $now);
# The run that stands for the night: the newest scheduled one in the 24 h after its slot; then each
# attempt it dispatched, whose name ends "after run <id>)" (lab-desktop-verify reschedules itself).
def night_run($slot; $now):
  [.workflow_runs[] | by($now) | select(.event == "schedule" and (.created_at | t) >= $slot and (.created_at | t) < $slot + 86400)]
  | max_by(.created_at);
def last_attempt($runs; $now):
  if . == null then null
  else . as $r
    | ([$runs[] | by($now) | select((.display_title // "") | endswith("after run \($r.id))"))] | min_by(.created_at)) as $n
    | if $n == null then $r else ($n | last_attempt($runs; $now)) end
  end;
# A job as it stood at the check. One that never got a runner has an empty runner_name: its
# started_at is only when it was created.
def job_state($now):
  if .status == "completed" and (.completed_at // "") != "" and (.completed_at | t) <= $now then "done"
  elif (.runner_name // "") != "" and (.started_at // "") != "" and (.started_at | t) <= $now then "running"
  else "queued" end;
def failing:
  [.[] | select(.conclusion != null and .conclusion != "success" and .conclusion != "skipped")
   | if (.runner_name // "") == "" then "\(.name): \(.conclusion) before any runner took it"
     else (first(.steps[]? | select(.conclusion == "failure" or .conclusion == "cancelled" or .conclusion == "timed_out")) // null) as $s
       | if $s then "\(.name) › \($s.name)" else "\(.name): \(.conclusion)" end
     end]
  | if length == 0 then "no job started: cancelled while it waited in its concurrency group" else join("; ") end;
def unfinished($now):
  [.[] | by($now)] as $j
  | [$j[] | select(job_state($now) == "queued") | "\(.name) [\(.labels | join(", "))]"] as $q
  | if ($j | length) == 0 then "still waiting in its concurrency group: no job started"
    elif ($q | length) > 0 then "still queued, no runner took it: \($q | join("; "))"
    else "still running: \([$j[] | select(job_state($now) == "running") | .name] | join(", "))" end;
def lab_end: [.[] | select(.labels | index("fjarr-lab")) | .completed_at // empty] | max;
def verdict($run; $jobs; $now; $due; $lab):
  if $run == null then
    if $now < $due then {verdict: "pending", text: "not started yet"}
    else {verdict: "red", key: "missing", text: "missing: no scheduled run started (GitHub skipped it, or the workflow is disabled)"} end
  elif $run.status == "completed" and ($run.updated_at | t) <= $now then
    if $run.conclusion != "success" then {verdict: "red", key: $run.conclusion, text: "\($run.conclusion): \($jobs | failing)"}
    elif $lab and (($jobs | lab_end) // "") != "" and (($jobs | lab_end | t) > $due) then
      {verdict: "red", key: "late", text: "ran after its window had closed: its job on the machine finished \($jobs | lab_end)"}
    else {verdict: "green", text: "success"} end
  elif $now < $due then {verdict: "pending", text: "not finished yet"}
  else {verdict: "red", key: "unfinished", text: ($jobs | unfinished($now))} end;
'

judge() { # <workflow file> <name> <lab|nightly> <night>: one JSON line for the report
  local file=$1 name=$2 kind=$3 night=$4 s due end runs run jobs='{"jobs":[]}' day
  day=$(epoch "${night}T00:00:00Z")
  s=$(slot "$file" "$day")
  if [ "$kind" = lab ]; then due=$((day + LAB_DUE)); else due=$((day + NIGHTLY_DUE)); fi
  end=$((now < s + 3 * 86400 ? now : s + 3 * 86400)); [ "$end" -ge "$s" ] || end=$s
  runs=$(api "actions/workflows/$file/runs?branch=main&exclude_pull_requests=true&per_page=100&created=$(iso "$s")..$(iso "$end")")
  run=$(jq -c --argjson slot "$s" --argjson now "$now" "$JQ_LIB"' . as $all | night_run($slot; $now) | last_attempt([$all.workflow_runs[]]; $now)' <<<"$runs")
  if [ "$run" != null ]; then jobs=$(api "actions/runs/$(jq -r .id <<<"$run")/jobs?per_page=100"); fi
  jq -c -n --argjson run "$run" --argjson jobs "$jobs" --argjson now "$now" --argjson due "$due" \
    --arg file "${file%.yml}" --arg name "$name" --arg night "$night" --argjson lab "$([ "$kind" = lab ] && echo true || echo false)" \
    "$JQ_LIB"' {file: $file, name: $name, night: $night, run: $run} + verdict($run; [$jobs.jobs[]]; $now; $due; $lab)'
}

results=$(
  judge lab-desktop-prepare.yml "Lab desktop — prepare" lab "$night"
  judge lab-desktop-verify.yml "Lab desktop — verify" lab "$night"
  if [ "$nightly_night" = "$night" ]; then judge nightly.yml "Nightly" nightly "$night"
  else judge nightly.yml "Nightly (night of $nightly_night)" nightly "$nightly_night"; fi
)
results=$(jq -s -c . <<<"$results")

# The lab runner. Listing runners needs Administration: read, which GITHUB_TOKEN cannot be given; the
# lab jobs above tell whether the machine took them in its window. Offline is its state by day; in
# its window (22:00-05:30 UTC holds it in summer and in winter) it is trouble.
tod=$((now % 86400))
in_window=$([ "$tod" -ge $((22 * 3600)) ] || [ "$tod" -lt "$LAB_DUE" ] && echo true || echo false)
runner_red=""
if [ -n "$now_arg" ]; then
  runner="not checked: the runner list shows only the present"
elif list=$(api "actions/runners?per_page=100" 2>/dev/null); then
  runner=$(jq -r --argjson in_window "$in_window" '[.runners[] | select([.labels[].name] | index("fjarr-lab") and index("desktop"))]
    | if length == 0 then "none registered with the labels fjarr-lab and desktop"
      else map("\(.name) \(.status)\(if .busy then ", busy" else "" end)\(if .status != "offline" then ""
        elif $in_window then " in its window" else " (as it is outside its window)" end)") | join("; ") end' <<<"$list")
  case "$runner" in
    none*) runner_red="runner:none" ;;
    *"offline in its window"*) runner_red="runner:offline" ;;
  esac
else
  runner="not readable with this token (it needs Administration: read), so judged by the lab jobs above"
fi
if jq -e '[.[] | select(.file != "nightly" and (.text // "" | test("no runner took it|before any runner took it")))] | length > 0' <<<"$results" >/dev/null; then
  runner="$runner. The mini-PC took no lab job in its window: it was off, offline or reserved (\`fjarr-lab status\` on it)"
fi

# A run that never came is keyed by its night, so a second missing night still comments.
reds=$(jq -r --arg extra "$runner_red" '[(.[] | select(.verdict == "red") | "\(.file):\(.run.id // "none@\(.night)"):\(.key)"), ($extra | select(. != ""))] | join(",")' <<<"$results")
pending=$(jq -r '[.[] | select(.verdict == "pending") | .name] | join(", ")' <<<"$results")
if [ -n "$reds" ]; then overall=red; elif [ -n "$pending" ]; then overall="not judged yet"; else overall=green; fi
marker="<!-- nightly-alert night=$night red=$reds -->"
report=$(jq -r --arg night "$night" --arg overall "$overall" --arg now "$(iso "$now")" --arg runner "$runner" '
  "**Night of \($night): \($overall)** (checked \($now))\n",
  "| Workflow | Run | Verdict |", "|---|---|---|",
  (.[] | "| \(.name) | \(if .run then "[\(.run.id)](\(.run.html_url)) \(.run.created_at[11:16]) UTC\((.run.name // "") as $n | (.run.display_title // "") | ltrimstr($n))" else "—" end) | \(
    if .verdict == "red" then "**red**: \(.text)" elif .verdict == "pending" then "not yet: \(.text)" else .text end) |"),
  "\nLab runner: \($runner)."' <<<"$results")
echo "$report"
[ -z "${GITHUB_STEP_SUMMARY:-}" ] || echo "$report" >> "$GITHUB_STEP_SUMMARY"
echo

issues=$(api "issues?labels=nightly-red&state=open&per_page=100" | jq -c '[.[] | select(.pull_request == null) | {number, body}] | sort_by(.number)')
open=$(jq -r 'last | .number // empty' <<<"$issues")
case "$overall" in
  red)
    body="$marker
$report

Kept by \`nightly-alert\` (docs/30-continuous-integration.md#nightly-alert): it comments when the red runs change, and closes this issue on the first green night."
    if [ -z "$open" ]; then
      if ! api "labels/nightly-red" >/dev/null 2>&1; then
        say "create the label nightly-red"
        change POST labels -f name=nightly-red -f color=b60205 -f description="A nightly or lab run is red (tools/ci/nightly-alert.sh)"
      fi
      say "open an issue \"Nightly red since the night of $night\" labelled nightly-red"
      change POST issues -f title="Nightly red since the night of $night" -f body="$body" -f "labels[]=nightly-red"
    # The same red runs as last time, whichever night judged them (a Nightly is judged on two).
    elif [ "$(jq -r 'last | .body // ""' <<<"$issues" | command grep -aoF -- " red=$reds -->" || true)" = " red=$reds -->" ]; then
      say "update #$open, which already lists these red runs, without a comment"
      change PATCH "issues/$open" -f body="$body"
    else
      say "update #$open and comment on it: the red runs changed"
      change PATCH "issues/$open" -f body="$body"
      change POST "issues/$open/comments" -f body="$report"
    fi ;;
  green)
    if [ -z "$open" ]; then echo "nightly-alert: green, and no nightly-red issue is open"; fi
    for n in $(jq -r '.[].number' <<<"$issues"); do
      say "close #$n with a comment: the night of $night is green"
      change POST "issues/$n/comments" -f body="The night of $night is green: closing.

$report"
      change PATCH "issues/$n" -f state=closed -f state_reason=completed
    done ;;
  *) echo "nightly-alert: not judged yet ($pending): nothing to do before a later check" ;;
esac
