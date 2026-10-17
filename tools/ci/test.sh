#!/usr/bin/env bash
# Tests for tools/ci (docs/30-continuous-integration.md): the change classifier, the image hash's inputs,
# tonight's prepare for the lab's verify, and the red-night alert against recorded API answers.
set -uo pipefail
cd "$(dirname "$0")/../.." || exit 1
export PYTHONDONTWRITEBYTECODE=1 # no __pycache__ left in the tree to be committed
fails=0
check() { # name expected actual
  if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: expected '$2', got '$3'"; fails=$((fails + 1)); fi
}
classify() { printf '%s\n' "$@" | bash tools/ci/changes.sh --classify; }

check "docs only" code=false "$(classify docs/30-continuous-integration.md docs/adr/0001-x.md)"
check "root markdown and .claude" code=false "$(classify CLAUDE.md README.md .claude/skills/x/SKILL.md)"
check "website" code=false "$(classify website/src/pages/index.astro)"
check "a spec and the agent" code=true "$(classify docs/08-protocol.md agent/src/core/x.cpp)"
check "markdown inside the tree is code" code=true "$(classify packaging/README.md)"
check "a workflow is code" code=true "$(classify .github/workflows/ci.yml)"
check "a root file other than markdown is code" code=true "$(classify Makefile)"
check "no files is code" code=true "$(classify)"
check "new branch is code" code=true "$(bash tools/ci/changes.sh 0000000000000000000000000000000000000000 HEAD 2>/dev/null)"
check "an unreadable range is code" code=true "$(bash tools/ci/changes.sh deadbeefdeadbeefdeadbeefdeadbeefdeadbeef HEAD 2>/dev/null)"

src() { python3 -c "import sys; sys.path.insert(0, 'tools/ci'); import image; print(' '.join(image.copy_sources(sys.stdin.read())))"; }
check "COPY sources" "a b" "$(printf 'FROM x\nCOPY a b /dst/\n' | src)"
check "flags are not sources" "s.sh" "$(printf 'COPY --chmod=0755 s.sh /usr/local/bin/s\n' | src)"
check "COPY --from reads a stage" "" "$(printf 'COPY --from=build /out /usr/bin/out\n' | src)"
check "continuation lines and ADD" "x y" "$(printf 'ADD x \\\n  y /dst/\n' | src)"
check "JSON form" "a" "$(printf 'COPY ["a", "/dst"]\n' | src)"
tag() { python3 tools/ci/image.py tag "$@"; }
t1=$(tag docker/robot-sim docker/robot-sim/Dockerfile)
check "the tag is stable" "$t1" "$(tag docker/robot-sim docker/robot-sim/Dockerfile)"
t2=$(tag docker/robot-sim docker/robot-sim/Dockerfile USER_UID=1001)
differs() { if [ "$1" != "$2" ]; then echo yes; else echo no; fi; }
check "build arguments change the tag" yes "$(differs "$t1" "$t2")"
t3=$(tag docker/robot-services docker/robot-services/Dockerfile)
check "COPY'd files change the tag" yes "$(differs "$t1" "$t3")"

# Tonight's prepare, as lab-desktop-verify judges it (docs/15#unattended-access-test-the-industrial-gate):
# by when it ran on the machine, not when GitHub created the run. Reads {now, run, jobs} on stdin.
prep() {
  python3 -c '
import json, sys
sys.path.insert(0, "tools/ci")
import tonights_prepare as t
d = json.load(sys.stdin)
print(*t.judge(d.get("run"), d.get("jobs", []), t._time(d["now"])))'
}
on_machine='"labels": ["self-hosted", "fjarr-lab", "desktop", "gnome"]'
hosted='"labels": ["ubuntu-24.04"]'
check "a prepare that waited 21.7 h for the machine and has just run is tonight's (2026-10-16)" "ran 2026-10-16T22:01:16Z" "$(prep <<EOF
{"now": "2026-10-16T22:02:55Z",
 "run": {"status": "completed", "conclusion": "success", "created_at": "2026-10-16T00:21:21Z", "updated_at": "2026-10-16T22:01:17Z"},
 "jobs": [{$hosted, "completed_at": "2026-10-16T00:36:55Z"}, {$on_machine, "completed_at": "2026-10-16T22:01:16Z"}]}
EOF
)"
check "an unfinished prepare is not yet, however long ago it was created" "waiting in_progress" "$(prep <<EOF
{"now": "2026-10-16T22:02:55Z",
 "run": {"status": "in_progress", "conclusion": null, "created_at": "2026-10-15T00:30:03Z", "updated_at": "2026-10-15T00:34:00Z"}}
EOF
)"
check "a prepare waiting behind verify in the group is not yet" "waiting pending" "$(prep <<EOF
{"now": "2026-10-17T00:50:00Z",
 "run": {"status": "pending", "conclusion": null, "created_at": "2026-10-17T00:49:00Z", "updated_at": "2026-10-17T00:49:00Z"}}
EOF
)"
check "yesterday's prepare is stale once tonight's slot is 6 h past" "stale 2026-10-16T00:30:05Z" "$(prep <<EOF
{"now": "2026-10-17T04:16:00Z",
 "run": {"status": "completed", "conclusion": "success", "created_at": "2026-10-16T00:25:39Z", "updated_at": "2026-10-16T00:30:06Z"},
 "jobs": [{$on_machine, "completed_at": "2026-10-16T00:30:05Z"}]}
EOF
)"
check "a prepare that failed tonight says how" "failure 2026-10-12T01:25:16Z" "$(prep <<EOF
{"now": "2026-10-12T01:47:43Z",
 "run": {"status": "completed", "conclusion": "failure", "created_at": "2026-10-12T01:17:58Z", "updated_at": "2026-10-12T01:25:17Z"},
 "jobs": [{$hosted, "completed_at": "2026-10-12T01:24:50Z"}, {$on_machine, "completed_at": "2026-10-12T01:25:16Z"}]}
EOF
)"
check "a prepare cancelled while it waited for the machine is dated by its end" "cancelled 2026-10-16T00:34:00Z" "$(prep <<EOF
{"now": "2026-10-16T01:00:00Z",
 "run": {"status": "completed", "conclusion": "cancelled", "created_at": "2026-10-15T00:30:03Z", "updated_at": "2026-10-16T00:34:01Z"},
 "jobs": [{$hosted, "completed_at": "2026-10-15T00:33:59Z"}, {$on_machine, "runner_name": "", "completed_at": "2026-10-16T00:34:00Z"}]}
EOF
)"
check "a run with no job on the machine is dated by the run" "ran 2026-10-17T00:30:05Z" "$(prep <<EOF
{"now": "2026-10-17T00:46:40Z",
 "run": {"status": "completed", "conclusion": "success", "created_at": "2026-10-17T00:25:39Z", "updated_at": "2026-10-17T00:30:05Z"},
 "jobs": []}
EOF
)"
check "verify before tonight's prepare was created waits for it" "waiting not-created-yet" "$(prep <<EOF
{"now": "2026-10-17T22:46:00Z",
 "run": {"status": "completed", "conclusion": "success", "created_at": "2026-10-16T22:20:00Z", "updated_at": "2026-10-16T22:46:00Z"},
 "jobs": [{$on_machine, "completed_at": "2026-10-16T22:45:59Z"}]}
EOF
)"
check "... also when GitHub starts both after midnight" "waiting not-created-yet" "$(prep <<EOF
{"now": "2026-10-18T00:46:00Z",
 "run": {"status": "completed", "conclusion": "success", "created_at": "2026-10-17T00:25:39Z", "updated_at": "2026-10-17T00:30:06Z"},
 "jobs": [{$on_machine, "completed_at": "2026-10-17T00:30:05Z"}]}
EOF
)"
check "the slot it waits for is lab-desktop-prepare's cron" \
  "$(sed -n 's/^ *- cron: *"\([0-9]*\) \([0-9]*\) .*/\1 \2/p' .github/workflows/lab-desktop-prepare.yml)" \
  "$(python3 -c 'import sys; sys.path.insert(0, "tools/ci"); import tonights_prepare as t; print(t.SLOT.minute, t.SLOT.hour)')"
check "no prepare run at all" "none -" "$(prep <<<'{"now": "2026-10-17T00:46:40Z", "run": null}')"

# nightly-alert.sh (docs/30#nightly-alert) against recorded API answers: the night of 2026-10-20, whose
# prepare and verify are scheduled 22:15 and 22:45 UTC on the 19th and its Nightly 02:17 UTC on the 20th.
has() { # name output text: the output contains the text
  if grep -qF -- "$3" <<<"$2"; then echo "ok   $1"; else echo "FAIL $1: no '$3' in"; printf '%s\n' "$2" | sed 's/^/       /'; fails=$((fails + 1)); fi
}
fx=$(mktemp -d); trap 'rm -rf "$fx"' EXIT
fx_run() { # id workflow event status conclusion created updated [title]
  jq -nc --argjson id "$1" --arg n "$2" --arg e "$3" --arg s "$4" --arg c "$5" --arg cr "$6" --arg up "$7" --arg t "${8:-$2}" \
    '{id: $id, name: $n, event: $e, status: $s, conclusion: (if $c == "" then null else $c end), created_at: $cr,
      updated_at: $up, display_title: $t, html_url: "https://github.com/o/r/actions/runs/\($id)"}'
}
fx_runs() { local f=$1; shift; printf '%s\n' "$@" | jq -s '{workflow_runs: .}' > "$fx/actions_workflows_${f}_runs.json"; }
fx_job() { # name labels status conclusion created started completed runner [failed step]
  jq -nc --arg n "$1" --arg l "$2" --arg s "$3" --arg c "$4" --arg cr "$5" --arg st "$6" --arg co "$7" --arg r "$8" --arg f "${9:-}" \
    '{name: $n, labels: ($l | split(",")), status: $s, conclusion: (if $c == "" then null else $c end), created_at: $cr,
      started_at: $st, completed_at: (if $co == "" then null else $co end), runner_name: $r,
      steps: (if $f == "" then [] else [{name: $f, conclusion: "failure"}] end)}'
}
fx_jobs() { local id=$1; shift; printf '%s\n' "$@" | jq -s '{jobs: .}' > "$fx/actions_runs_${id}_jobs.json"; }
lab_labels=self-hosted,fjarr-lab,desktop,gnome
green_night() { # every run green, and issue #7 open from an earlier red night
  rm -f "$fx"/*
  fx_runs lab-desktop-prepare.yml "$(fx_run 101 "Lab desktop — prepare" schedule completed success 2026-10-20T00:30:00Z 2026-10-20T00:36:00Z)"
  fx_jobs 101 "$(fx_job "packages" ubuntu-24.04 completed success 2026-10-20T00:30:01Z 2026-10-20T00:30:02Z 2026-10-20T00:34:00Z "GitHub Actions 1")" \
    "$(fx_job "install (mini-PC)" "$lab_labels" completed success 2026-10-20T00:34:00Z 2026-10-20T00:34:05Z 2026-10-20T00:35:50Z mini-pc)"
  fx_runs lab-desktop-verify.yml "$(fx_run 201 "Lab desktop — verify" schedule completed success 2026-10-20T00:50:00Z 2026-10-20T00:50:12Z)"
  fx_jobs 201 "$(fx_job "verdict (mini-PC)" "$lab_labels" completed success 2026-10-20T00:50:01Z 2026-10-20T00:50:02Z 2026-10-20T00:50:11Z mini-pc)"
  fx_runs nightly.yml "$(fx_run 301 Nightly schedule completed success 2026-10-20T07:00:00Z 2026-10-20T07:20:00Z)"
  fx_jobs 301 "$(fx_job "soak" gpu-desktop completed success 2026-10-20T07:00:01Z 2026-10-20T07:00:05Z 2026-10-20T07:19:59Z gpu-desktop)"
  echo '[{"number": 7, "body": "<!-- nightly-alert night=2026-10-19 red=lab-desktop-verify:200:failure -->"}]' > "$fx/issues.json"
  echo '{"name": "nightly-red"}' > "$fx/labels_nightly-red.json"
}
alert() { FJARR_ALERT_FIXTURES=$fx GITHUB_REPOSITORY=o/r bash tools/ci/nightly-alert.sh "$@" 2>&1; }

green_night
out=$(alert --dry-run --night 2026-10-20 --now 2026-10-20T12:30:00Z)
has "a green night is green" "$out" "Night of 2026-10-20: green"
has "a green night closes the open issue" "$out" "would close #7 with a comment"
alert --night 2026-10-20 --now 2026-10-20T12:30:00Z >/dev/null
check "closing comments, then closes" "POST issues/7/comments PATCH issues/7" "$(tr '\n' ' ' < "$fx/writes.log" | sed 's/ $//')"
has "a replayed check does not show today's runner" "$out" "Lab runner: not checked: the runner list shows only the present."
has "the runner list unreadable with GITHUB_TOKEN is said, not guessed" "$(alert --dry-run --night 2026-10-20)" "not readable with this token"
echo '{"runners": [{"name": "mini-pc", "status": "offline", "busy": false, "labels": [{"name": "self-hosted"}, {"name": "fjarr-lab"}, {"name": "desktop"}]}]}' > "$fx/actions_runners.json"
has "a readable runner list shows the lab runner" "$(FJARR_ALERT_CLOCK=2026-10-20T12:30:00Z alert --dry-run --night 2026-10-20)" "Lab runner: mini-pc offline (as it is outside its window)."
out=$(FJARR_ALERT_CLOCK=2026-10-20T23:00:00Z alert --dry-run --night 2026-10-20)
has "a lab runner offline in its window is said" "$out" "Lab runner: mini-pc offline in its window."
has "... and makes the night red" "$out" "**Night of 2026-10-20: red**"
echo '{"runners": [{"name": "gpu-desktop", "status": "online", "busy": false, "labels": [{"name": "gpu-desktop"}]}]}' > "$fx/actions_runners.json"
out=$(alert --dry-run --night 2026-10-20)
has "no runner with the lab's labels is said" "$out" "Lab runner: none registered with the labels fjarr-lab and desktop"
has "... and makes the night red" "$out" "**Night of 2026-10-20: red**"
rm "$fx/actions_runners.json"
check "the default night is the newest whose lab window has closed" "2026-10-19 2026-10-20" \
  "$(for t in 2026-10-20T05:00:00Z 2026-10-20T05:31:00Z; do alert --dry-run --now "$t" | sed -n 's/^\*\*Night of \([0-9-]*\).*/\1/p'; done | tr '\n' ' ' | sed 's/ $//')"

out=$(alert --dry-run --night 2026-10-20 --now 2026-10-20T05:30:00Z)
has "the morning check judges the lab and waits for the Nightly" "$out" "| Nightly | — | not yet: not started yet |"
has "a night not judged yet changes nothing" "$out" "nothing to do before a later check"
# Without --night the morning check judges the night before's Nightly: had its 12:30 check been
# dropped, no check would judge it.
fx_runs nightly.yml "$(fx_run 301 Nightly schedule completed failure 2026-10-20T07:00:00Z 2026-10-20T07:20:00Z)"
fx_jobs 301 "$(fx_job "soak" gpu-desktop completed failure 2026-10-20T07:00:01Z 2026-10-20T07:00:05Z 2026-10-20T07:19:59Z gpu-desktop "soak")"
out=$(alert --dry-run --now 2026-10-21T05:31:00Z)
has "the morning check judges the night before's Nightly" "$out" "| Nightly (night of 2026-10-20) | [301](https://github.com/o/r/actions/runs/301) 07:00 UTC | **red**: failure: soak › soak |"
has "... beside its own night's lab runs" "$out" "**Night of 2026-10-21: red**"
# ... and a red run it already reported does not comment again, though the night has moved on.
fx_runs lab-desktop-prepare.yml "$(fx_run 102 "Lab desktop — prepare" schedule completed success 2026-10-21T00:30:00Z 2026-10-21T00:36:00Z)"
fx_jobs 102 "$(fx_job "install (mini-PC)" "$lab_labels" completed success 2026-10-21T00:34:00Z 2026-10-21T00:34:05Z 2026-10-21T00:35:50Z mini-pc)"
fx_runs lab-desktop-verify.yml "$(fx_run 202 "Lab desktop — verify" schedule completed success 2026-10-21T00:50:00Z 2026-10-21T00:50:12Z)"
fx_jobs 202 "$(fx_job "verdict (mini-PC)" "$lab_labels" completed success 2026-10-21T00:50:01Z 2026-10-21T00:50:02Z 2026-10-21T00:50:11Z mini-pc)"
echo '[{"number": 7, "body": "<!-- nightly-alert night=2026-10-20 red=nightly:301:failure -->"}]' > "$fx/issues.json"
has "a red run judged again on the next night does not comment" "$(alert --dry-run --now 2026-10-21T05:31:00Z)" \
  "would update #7, which already lists these red runs, without a comment"
# A run that never came is keyed by its night: a second missing night is a new red run.
fx_runs nightly.yml
echo '[{"number": 7, "body": "<!-- nightly-alert night=2026-10-19 red=nightly:none@2026-10-19:missing -->"}]' > "$fx/issues.json"
has "a Nightly missing on a second night comments" "$(alert --dry-run --now 2026-10-21T05:31:00Z)" "would update #7 and comment on it"
green_night
fx_runs nightly.yml

fx_runs nightly.yml
out=$(alert --dry-run --night 2026-10-20 --now 2026-10-20T12:30:00Z)
has "a Nightly that never started is red after its due time" "$out" "**red**: missing: no scheduled run started"

green_night
fx_runs lab-desktop-verify.yml "$(fx_run 201 "Lab desktop — verify" schedule completed success 2026-10-20T00:50:00Z 2026-10-20T00:50:12Z)" \
  "$(fx_run 202 "Lab desktop — verify" workflow_dispatch completed failure 2026-10-20T00:50:11Z 2026-10-20T00:52:30Z "Lab desktop — verify (attempt 2, after run 201)")"
fx_jobs 202 "$(fx_job "verdict (mini-PC)" "$lab_labels" completed failure 2026-10-20T00:50:12Z 2026-10-20T00:52:00Z 2026-10-20T00:52:29Z mini-pc "the machine's verdict")"
out=$(alert --dry-run --night 2026-10-20 --now 2026-10-20T12:30:00Z)
has "a rescheduled verify (a success) is judged by its last attempt" "$out" "[202](https://github.com/o/r/actions/runs/202) 00:50 UTC (attempt 2, after run 201) | **red**: failure: verdict (mini-PC) › the machine's verdict"
has "new red runs comment on the open issue" "$out" "would update #7 and comment on it"
echo '[{"number": 7, "body": "<!-- nightly-alert night=2026-10-20 red=lab-desktop-verify:202:failure -->"}]' > "$fx/issues.json"
out=$(alert --dry-run --night 2026-10-20 --now 2026-10-20T12:30:00Z)
has "the same red runs again do not comment" "$out" "would update #7, which already lists these red runs, without a comment"
echo '[]' > "$fx/issues.json"; rm "$fx/labels_nightly-red.json"
out=$(alert --dry-run --night 2026-10-20 --now 2026-10-20T12:30:00Z)
has "a red night with no issue opens one, and its label" "$out" "would create the label nightly-red"
has "... titled by the night" "$out" "would open an issue \"Nightly red since the night of 2026-10-20\" labelled nightly-red"

green_night
fx_runs lab-desktop-prepare.yml "$(fx_run 101 "Lab desktop — prepare" schedule in_progress "" 2026-10-20T00:30:00Z 2026-10-20T00:34:00Z)"
fx_jobs 101 "$(fx_job "packages" ubuntu-24.04 completed success 2026-10-20T00:30:01Z 2026-10-20T00:30:02Z 2026-10-20T00:34:00Z "GitHub Actions 1")" \
  "$(fx_job "install (mini-PC)" "$lab_labels" queued "" 2026-10-20T00:34:00Z 2026-10-20T00:34:00Z "" "")"
fx_runs lab-desktop-verify.yml "$(fx_run 201 "Lab desktop — verify" schedule pending "" 2026-10-20T00:50:00Z 2026-10-20T00:50:00Z)"
fx_jobs 201
out=$(alert --dry-run --night 2026-10-20 --now 2026-10-20T05:30:00Z)
has "a lab job still queued at the check is red" "$out" "**red**: still queued, no runner took it: install (mini-PC) [self-hosted, fjarr-lab, desktop, gnome]"
has "... and so is a verify still waiting in the group" "$out" "**red**: still waiting in its concurrency group: no job started"
has "... which says the mini-PC was not there" "$out" "The mini-PC took no lab job in its window"
out=$(alert --dry-run --night 2026-10-20 --now 2026-10-20T04:00:00Z)
has "before the window has closed, the same is not yet" "$out" "| not yet: not finished yet |"

green_night
fx_runs lab-desktop-verify.yml "$(fx_run 201 "Lab desktop — verify" schedule completed cancelled 2026-10-20T00:50:00Z 2026-10-21T00:21:22Z)"
fx_jobs 201
fx_jobs 101 "$(fx_job "install (mini-PC)" "$lab_labels" completed success 2026-10-20T00:34:00Z 2026-10-20T22:00:05Z 2026-10-20T22:01:16Z mini-pc)"
out=$(alert --dry-run --night 2026-10-20 --now 2026-10-21T08:00:00Z)
has "a verify cancelled while it waited in the group is red" "$out" "**red**: cancelled: no job started: cancelled while it waited in its concurrency group"
has "a lab run that ran in a later window is red" "$out" "**red**: ran after its window had closed: its job on the machine finished 2026-10-20T22:01:16Z"

if [ "$fails" -ne 0 ]; then echo "tools/ci: $fails failed"; exit 1; fi
echo "tools/ci: all passed"
