#!/usr/bin/env python3
"""Tonight's lab-desktop-prepare, for lab-desktop-verify
(docs/15-testing-strategy.md#unattended-access-test-the-industrial-gate).

    tonights_prepare.py <owner/repo>     prints "<run id> <state> <when>"; needs GH_TOKEN

The newest prepare run on main, and what verify makes of it:
  ran       it succeeded, and its job on the machine finished less than 12 hours ago: report the
            machine's verdict for it
  waiting   it has not finished (queued, running, or behind verify in the lab-desktop group): not yet;
            or, with <when> "not-created-yet", tonight's has not been created: the newest ended before
            tonight's slot (the prepare cron), which passed less than 6 hours ago
  stale     it ended 12 hours ago or more: no prepare has run tonight
  none      there is no prepare run at all
  <other>   it ended otherwise tonight: failure, cancelled, ...
<when> is when it ended (UTC), or its status while it has not.
The standard library only: the lab machine has no gh.
"""
import datetime
import json
import os
import sys
import urllib.request

FRESH = datetime.timedelta(hours=12)
# lab-desktop-prepare.yml's cron, "15 22 * * *" UTC (tools/ci/test.sh checks they agree), and how long
# after it a missing run is still GitHub being late: it has started that cron up to 4 h 46 min late.
SLOT = datetime.time(22, 15, tzinfo=datetime.timezone.utc)
LATE = datetime.timedelta(hours=6)


def _slot_before(now):
    """Tonight's prepare slot: the newest SLOT at or before `now`."""
    slot = datetime.datetime.combine(now.date(), SLOT)
    return slot if slot <= now else slot - datetime.timedelta(days=1)


def _time(s):
    return datetime.datetime.fromisoformat(s.replace("Z", "+00:00"))


def judge(run, jobs, now):
    """(state, when) for the newest prepare run and its jobs, as at `now`."""
    if run is None:
        return "none", "-"
    if run["status"] != "completed":
        return "waiting", run["status"]
    # When it ran, not when GitHub created it: a prepare that had waited 21.7 h for the machine ran
    # on it 2 min before verify asked, and verify called it stale (2026-10-16). Its job on the machine
    # (the fjarr-lab label) dates it; a run that never got that far is dated by its end.
    on_machine = [_time(j["completed_at"]) for j in jobs
                  if "fjarr-lab" in j.get("labels", []) and j.get("completed_at")]
    ended = max(on_machine) if on_machine else _time(run["updated_at"])
    when = ended.strftime("%Y-%m-%dT%H:%M:%SZ")
    # GitHub starts the two crons with their own lateness, so verify can run before tonight's prepare
    # even exists: the newest is then last night's. Not yet, while that is still lateness.
    slot = _slot_before(now)
    if ended < slot and now - slot < LATE:
        return "waiting", "not-created-yet"
    if now - ended >= FRESH:
        return "stale", when
    if run["conclusion"] != "success":
        return run["conclusion"] or "unknown", when
    return "ran", when


def main():
    repo = sys.argv[1]

    def get(path):
        req = urllib.request.Request(
            f"https://api.github.com/repos/{repo}/{path}",
            headers={"Authorization": "Bearer " + os.environ["GH_TOKEN"], "Accept": "application/vnd.github+json"})
        return json.load(urllib.request.urlopen(req, timeout=20))

    runs = get("actions/workflows/lab-desktop-prepare.yml/runs?branch=main&per_page=1")["workflow_runs"]
    run = runs[0] if runs else None
    jobs = get(f"actions/runs/{run['id']}/jobs?per_page=100")["jobs"] if run and run["status"] == "completed" else []
    state, when = judge(run, jobs, datetime.datetime.now(datetime.timezone.utc))
    print(run["id"] if run else "none", state, when)


if __name__ == "__main__":
    main()
