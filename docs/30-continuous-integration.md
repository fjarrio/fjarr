---
title: Continuous Integration
description: How Fjarr's CI is organised and kept fast — the job graph, which changes run what, CI images by content hash, caches as a budget, build-once artifacts reused by the nightly, the lab and the release — and the rules for adding to it.
---

CI is where [docs/13](13-development-workflow.md)'s definition of done is
enforced and where [docs/15](15-testing-strategy.md)'s tests run. This document
is about the machinery: how the workflows are laid out, what makes them fast,
and the rules that keep them fast as the project grows. **Read it before you add
a job, a cache or an image to `.github/workflows/`.**

## What we optimise for {#goals}

The repository is public, so GitHub's hosted minutes cost nothing. **The cost
is the wait**: a push is not done until CI is green ([work on main](13-development-workflow.md#branches-commits-prs)),
and CI runs 10–20 times a day. The budget is wall-clock time to a verdict:

| Push | Budget | Measured (2026-10-02) |
|---|---|---|
| Code (anything outside the docs) | **≤ 20 min** to the last job | 38 min before this design; see [the job graph](#the-job-graph) |
| Docs only | **≤ 3 min** | 38 min before this design |

Job-minutes matter only where they buy wall-clock time back: a job that runs in
parallel and finishes before the longest one is free.

## The principles {#principles}

1. **Every commit on `main` gets its own verdict.** `ci.yml` never cancels a
   superseded run on `main`: bisecting needs every commit's result, and a tag
   needs its commit's ([never tag on red](#releases-reuse-cis-packages)). Pull
   requests are different — only the newest push to a PR matters, so a new push
   cancels the PR's run in progress.
2. **Run what a change can affect, and nothing is assumed safe.** A push is
   classified by the paths it touches ([below](#which-changes-run-what)). The
   only class that skips work is *docs only*, and a path is *docs* only when it
   is on an explicit list; everything else is code. A new class needs a
   measurement showing it is worth the risk.
3. **Build an artifact once, and test that artifact.** The `.deb`s are built
   once per commit by the `packages` job. The agent image, the nightly lab and
   the release use those files, not a rebuild: what CI tested is what ships
   ([below](#artifacts-built-once)).
4. **CI images are named by their inputs.** Every image CI builds from a
   Dockerfile is tagged with a hash of what goes into it and pulled from
   `ghcr.io/fjarrio/fjarr-ci`; only a new hash builds ([below](#ci-images)).
5. **Caches are a budget.** A repository gets 10 GB of Actions cache, and an
   entry over the limit evicts another. Each cache keeps **one** entry, saved
   from `main` only ([below](#caches)).
6. **Parallel over long.** Split a job when the halves need different
   running stacks; keep steps together when they need the same stack, because
   starting a stack twice costs more than it saves.
7. **Measure before and after.** Every change to the layout states the times
   it was made for, from [`tools/ci/timings.sh`](#measuring), in its commit
   message and in the table above.

## The job graph {#the-job-graph}

`ci.yml`, on every push to `main` and every pull request. The first job,
`changes`, decides whether the rest run; `docs` always runs.

| Job | Runs | Needs | What |
|---|---|---|---|
| `changes` | always | — | Classifies the push ([below](#which-changes-run-what)) |
| `docs` | always | — | markdownlint, lychee, protocol fixtures, website build, mermaid |
| `web` | code | `changes` | `@fjarr/*` build, typecheck, unit tests; demos build |
| `rust` | code | `changes` | `cargo fmt`, clippy `-D warnings`, tests, platform check |
| `cpp` | code | `changes` | Release build and tests, RAII gate, ASan/UBSan/LSan (ccache) |
| `tsan` | code | `changes` | The unit and loop tests under ThreadSanitizer, in the dev image (ccache) |
| `server-image` | code | `changes` | `fjarr-server` builds standalone from `signaling/` |
| `desktop-fixture` | code | `changes` | Headless mutter: capture, EIS, the helper's handover; `fjarr-lab` tests |
| `packages` (amd64, arm64) | code | `changes` | The `.deb`s, installed on a clean Ubuntu, embedded out of tree; uploaded as `debs-<arch>` |
| `e2e` | code | `changes` | The browser lab on the dev-image stack: suites, opsim, the desktop end to end and in the browser, soak, log gate, tunnel |
| `agent-image` | code | `packages` | The runtime image **from `debs-amd64`**, the demo robot from it with the browser smoke suites, the reference compose file |
| `cache-prune` | code, on `main` | the caching jobs | Keeps one entry per cache ([below](#caches)) |

The critical path is `packages` → `agent-image` or `e2e`, whichever is longer;
everything else finishes inside it.

## Which changes run what {#which-changes-run-what}

`tools/ci/changes.sh` lists the files a push changed (the push's range, or the
pull request's diff) and prints `code=true` unless **every** file matches the
docs list:

- `docs/**` — the specs; the website build in `docs` covers them
- `*.md` at the repository root, `.claude/**`
- `website/**` — built by `docs`

Anything else, including a Markdown file elsewhere in the tree (it may be
packaged), is code. When the range cannot be read — a new branch, a force
push, more files than the API lists — the answer is `code=true`.

A docs-only commit's jobs show as *skipped*: its code is its parent's, and the
parent's verdict stands for it.

## Artifacts built once {#artifacts-built-once}

| Artifact | Built by | Used by |
|---|---|---|
| `debs-amd64`, `debs-arm64` | `ci.yml` `packages`, every code push (kept 14 days) | `agent-image` (same run); `lab-desktop-prepare` (the newest green `main` run that has them); `release.yml` (the tagged commit's run) |
| `lab-desktop-build` (`fjarr-server`, `fjarr-opsim`) | `lab-desktop-prepare`, nightly | the lab machine, the same night |

`tools/ci/ci-run.sh` finds the `ci.yml` run that holds a commit's packages, so
every consumer looks them up the same way.

### Releases reuse CI's packages {#releases-reuse-cis-packages}

`release.yml` does not build. For the tag's commit it waits for `ci.yml` to
finish, refuses unless that run is green and holds both architectures'
packages, checks they carry the tag's version, and publishes **those files**.
So a release cannot ship bytes CI never tested, and "never tag on red" is
enforced rather than remembered. Tag the commit that `make set-version`
created, or any later code commit: a docs-only commit has no packages of its
own, and the release refuses it with that message.

## CI images {#ci-images}

The images CI builds from a Dockerfile — `dev`, the deb builder, the desktop
fixture, `robot-sim`, `robot-services`, `fjarr-server`, the ROS lab — cost one
to three minutes each to build on a fresh runner and change rarely.
`tools/ci/image.sh` makes them a pull:

1. **The tag is a hash of the inputs**: the Dockerfile, the build arguments
   (for `dev` these include the runner's uid and groups), every tracked file
   its `COPY` and `ADD` lines name, the runner's architecture, and the ISO week.
   The week makes every image rebuild once a week, which picks up the base
   image's and the archive's updates without anyone remembering to.
2. It pulls `ghcr.io/fjarrio/fjarr-ci:<name>-<hash>`. If that exists, it is
   tagged with the local name compose and the Makefile expect (`fjarr-dev`,
   `fjarr-deb-builder`, …). If not, it builds, and pushes when the run may
   publish: a push to `main`, the schedule or a manual run. A pull request only
   pulls, so nothing a PR builds is ever served to `main`.
3. For compose services it writes an override that drops their `build:`
   section and adds it to `COMPOSE_FILE`, so a later `up --build` uses the
   image it pulled instead of rebuilding it.

A registry rather than the Actions cache, because the images total several
gigabytes and the cache's 10 GB is [already spoken for](#caches). Make the
`fjarr-ci` package public once, in its settings on GitHub, like the
repository: until then a fork's pull request cannot pull it and builds the
images itself, which is slower but still correct.

**Adding an image:** give it to `image.sh` in the jobs that use it. Inputs come
from the Dockerfile, so nothing is listed twice; the one rule is that **an
image must not read files its Dockerfile does not `COPY`** (a `RUN` that
fetches the repository's own files from elsewhere would not be hashed).

The deb builder is not a compose service: `make deb` builds it unless
`DEB_BUILDER=prebuilt`, which CI sets after `image.sh` has pulled it.

## Caches {#caches}

| Cache | Key | Size | Used by |
|---|---|---|---|
| ccache, the `cpp` job (release + ASan) | `ccache-cpp-<run>` | ~0.5 GB | `cpp` |
| ccache, the dev image's release build | `ccache-dev-release-<run>` | ~0.3 GB | `e2e` saves; `agent-image` and the nightly restore |
| ccache, the dev image's TSan build | `ccache-dev-tsan-<run>` | ~0.3 GB | `tsan` |
| Cargo (`Swatinem/rust-cache`) | Rust toolchain + lockfile | ~0.9 GB | `rust` |
| pnpm store (`setup-node`) | lockfile | ~0.2 GB | every job with Node |

The rules:

- **One entry per cache.** A ccache cache is saved under a new key each time
  (cache entries are immutable), restored by prefix, and after the run's saves
  the `cache-prune` job (`tools/ci/cache-prune.sh`) deletes the older entries
  with the same prefix.
  This replaced `hendrikmuhs/ccache-action`, which saved a 475 MB copy on
  every run and kept them: sixteen copies held 7.6 GB of the 10 GB on
  2026-10-02 and evicted everything else.
- **Saved from `main` only.** A pull request restores `main`'s caches and saves
  nothing; its entries would be scoped to the PR and evict `main`'s.
- **Know the total before adding one**: `gh api repos/fjarrio/fjarr/actions/cache/usage`
  and `gh cache list`. The table above should add up to well under 10 GB.

## The nightly, the lab and the release {#other-workflows}

- **`nightly.yml`** uses the same CI images and the dev ccache, so its builds
  are as cached as `e2e`'s.
- **`lab-desktop-prepare.yml`** installs the packages of the newest green
  `main` run of `ci.yml`, and checks out that run's commit so the scripts match
  the build; it builds only the lab binaries the packages do not contain.
- **`release.yml`** publishes the tagged commit's packages, as
  [above](#releases-reuse-cis-packages).

Lab machines run only `schedule` and `workflow_dispatch` from `main`, never
`ci.yml` ([ADR-0033](adr/0033-desktop-test-lab-and-shared-runners.md), [docs/12](12-development-environment.md#nightly-ci-and-the-self-hosted-gpu-runner)).

## Measuring {#measuring}

`tools/ci/timings.sh [run-id]` prints a run's jobs and every step over 20
seconds (the newest `ci.yml` run when no id is given). Use it before and after
a change to the layout, and update [the budget table](#goals).

## Adding to CI {#adding-to-ci}

1. **Which stack does the test need?** A test that needs the browser-lab stack
   belongs in `e2e`; one that needs only a build belongs in its tier's job; one
   that needs the packages belongs in `agent-image` or `packages`.
2. **Does it fit the budget?** If it pushes the critical path past
   [the budget](#goals), split a job (principle 6) or move the test to the
   nightly with a reason (docs/15 decides what is a per-push gate).
3. **A new cache or image** follows [the cache rules](#caches) and
   [the image rules](#ci-images).
4. **A new path class** (principle 2) needs a measurement and an entry
   [above](#which-changes-run-what).
5. **Update this document** in the same commit: the job graph, the tables, and
   the measured times.

## Next steps {#next-steps}

Recorded so they are built on rather than rediscovered:

- **ccache inside the package build.** `make deb` compiles the agent and the
  Rust tools from scratch in both `packages` jobs (5–6 min of each); a ccache
  and a Cargo cache mounted into the builder would cut that, at the cost of
  one more cache per architecture.
- **More path classes** once measured: web-only changes need not build the
  agent's packages; agent-only changes need not run the Rust job.
- **The `e2e` job's stack start** is mostly the agent build and the image
  pulls; a prebuilt agent from `cpp` handed over as an artifact would remove
  the build, if the dev image and the `cpp` container build identically.
