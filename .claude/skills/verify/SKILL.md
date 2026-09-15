---
name: verify
description: Run Fjarr's full definition-of-done verification (doctor, builds, tests, lints, docs gates) and report a pass/fail table. Use before declaring any task finished, before commits that will be pushed, and after environment changes.
---

# /verify — Fjarr definition-of-done runner

Run every gate the project defines (docs/13). All commands execute inside the
dev container. Report results as a compact PASS/FAIL table, then fix failures
— never report done with a red row, and never weaken a gate to get to green.

## Steps

1. Ensure services are up: `docker compose up -d dev robot-sim`.
2. Environment: `docker compose exec dev make doctor`
   (expected: 0 failures; WARNs for uinput and webrtcsink are by design).
3. C++: `docker compose exec dev make agent-build` then
   `docker compose exec dev make agent-test`.
4. Rust: `docker compose exec dev bash -c "cd signaling && cargo fmt --check"`,
   then `make signaling-clippy`, then `make signaling-test`.
5. Web: `docker compose exec dev make web-build` and `make web-lint`.
6. Docs: `docker compose exec dev make docs-lint` and `make docs-links`.
7. Website (only if docs/ or website/ changed):
   `docker compose exec dev make website-build`.
8. If protocol files changed (docs/08, protocol/): confirm all three
   implementations and any schemas were updated together.

## On failure

Fix the cause, don't bypass the gate. If a gate itself is wrong, that's a
spec change to docs/13 or the Makefile — propose it explicitly instead of
skipping. `cargo fmt` (not `--check`) and `make fmt` fix formatting failures.
