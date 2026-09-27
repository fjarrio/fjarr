---
title: "Slice 4.5f Review"
description: Retrospective review of discovery and login — the operator API, the FjarrCliLogin handoff, the CLI's login/list/picker, and a gate that logs in through the lab browser; with the spec gap the code flow exposed and the lease rule a viewer tripped over.
---

> Retrospective review of slice 4.5f per docs/13 and docs/20. `fjarr-connect
> login` through the demo dashboard, then a list, a pick by label and a connect
> with nobody typing a robot id, and the session in the audit log with
> `fjarr.net` among its capabilities — the docs/17 gate — runs in the lab in
> three seconds. The loopback shape is tested in halves and says so.

## What the harness found

| Found by | Defect | Fix |
|---|---|---|
| reading the spec before writing code | docs/27 promised the headless path — "prints a URL and a short code, you approve it wherever a browser exists, and the CLI polls" — and docs/09 listed two operator endpoints, neither of which could hold a pending code | three endpoints, spec'd first: create with a separate poll token, approve inside the signed-in app, poll once. Knowing what the human typed does not let anyone collect the credential |
| the first manual login | the printed login URL was on the API's host, not the dashboard's: `resolve_backend` derived the route from the API base whenever `--api` was given, and the demo serves the two from different containers | the route stays on the site the user named; a test covers the two-host case |
| the first component test run | two tests passed vacuously — `toBeDisabled` is a jest-dom matcher this package never had, and the failure was an "invalid Chai property", not an assertion | plain `.disabled` assertions |
| the gate's audit-log step, twice | first it read the log once and `session.ended` had not landed yet; then it polled but filtered every event by "fjarr.net", which `session.ended` never carries — it names the session, the reason and the duration, nothing else | correlation by `session_id` between the `started` that names the capabilities and its `ended`, polled |
| the arm before this slice | a viewer session takes the input lease whatever it claims, so a tunnel under another identity is refused as `capability-denied` | [#31](../18-open-questions.md); the lab runs both as one operator |

## Reviewer findings

| # | Severity | Finding | Resolution |
|---|---|---|---|
| 1 | medium | the loopback flow — the one a laptop with a browser gets — is not exercised end to end in the lab, because its callback is `127.0.0.1` on the machine running the CLI and the lab's browser is another container. Its listener has a Rust test that drives it over TCP, preflight and wrong-state included, and the component has six tests; the seam between them (the browser's `fetch` reaching the listener) is the untested part | recorded in docs/15 in those words. Running Chromium in `dev` would close it and is not worth a second browser image for one seam |
| 2 | medium | a login code can be approved by any signed-in user who types it, which is the device-flow phishing shape: get a victim to approve *your* code and you hold their credential. docs/09's mitigation is the one OAuth's device flow uses — the dashboard shows the code and asks the human to compare it with their terminal — plus the poll token being a separate secret and the `PUT` rate-limited | by design and now written down here; a customer who wants more binds approval to the requesting machine, which the operator API leaves to them |
| 3 | low | presence in `list` reads `unknown` after the demo backend restarts, because it derives `status` from webhooks it keeps in memory. A real backend persists them | the reference stays honest about being a reference; noted, not fixed |
| 4 | low | `list --ssh-config` derives the address in Rust, a second implementation of docs/27#addressing beside the agent's | pinned by the two lab vectors `fjarr-agent --net-address` produces; a change to either that misses the other fails the test |

## Measured

| Case | Result |
|---|---|
| The gate, `make tunnel-login` | login by code approved in the lab browser, `list`, `--ssh-config` with the agent's addresses, connect by the label "lab", `ssh` over the link, `session.started`/`ended` correlated in the audit log, logout refused thereafter — 3.0 s |
| Component tests | 6, in `@fjarr/react`; the loopback listener, 3 in Rust; the address vectors, config and login-URL resolution, 27 in the crate |

## Gate (docs/17 slice 4.5f)

Met for the code shape end to end, and for the loopback shape in halves, with docs/15 saying which is which.

## Deferred

- The loopback seam end to end (finding 1).
- Presence persisted in the demo backend (finding 3).
- [#31](../18-open-questions.md), the lease rule for a view-only session, belongs with 4.5f's users rather than its code.
