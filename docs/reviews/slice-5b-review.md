---
title: "Slice 5b Review"
description: Retrospective review of the delivery half of the demo wiring — the viewer served from introspect.viewer_dir, the demo's exposed endpoint with its token, the fjarr-agent image and the compose override; what the lab found, what was fixed, and what is deferred to M2.5.
---

> Retrospective review of slice 5b per docs/13 and docs/20, run before the
> slice was committed. The slice delivers what 5a built: the same
> `<PipelineGraph>` as a static app the agent serves from
> `introspect.viewer_dir`, the demo profile exposing the endpoint on the
> host's loopback with a dev token so `make introspect` opens it, and the
> `fjarr-agent` container image — built and smoke-tested in CI on every
> push, published only with M2.5's release process. One reviewer pass on
> the C++, the compose files and the Dockerfile, plus what the lab found.

## What the harness found

| Found by | Defect | Fix |
|---|---|---|
| lab `viewer` spec, first run | the served viewer listed the connected robot's session pipeline but drew nothing: the HTTP feed filled the pipeline list on refresh and never each pipeline's "latest", so a pipeline whose snapshots predate the SSE stream had no sequence to fetch a body for. The dashboard never showed it because the session feed's subscribe *replays* the latest | `setList()` seeds every pipeline's snapshot store from its row on both feeds; the contract suite reopens a feed on existing agent state and expects the latest and its body at once |
| endpoint unit test | none: the serving rules (index at `/`, hashed assets immutable, traversal and a symlink out of the directory refused, a missing file 404, the API shadowing a file named `pipelines`, the token gating the API and not the page, the text index naming the key without a directory) passed as written | — |
| first run of the image | `fjarr-agent` and `demo-robot` failed to start in the runtime image: Ubuntu 26.04 ships toml++ as a shared library (`libtomlplusplus3t64`), not header-only, and the runtime package list lacked it | the package is in the runtime stage; CI's image check now also runs `ldd` on the binary and fails on any "not found" |
| lab `introspect` spec against the image robot, one run | the valve was read from the first snapshot after streaming began, which can predate the valve opening (a state change lands first) | the read polls the newest snapshot, like the later toggle check |
| CI, two runs | the simulator died after all checks had passed: a segfault in `no-answer`'s teardown, then glibc's `tpp.c:83` assertion in `ice-restart`'s reconnect; ThreadSanitizer on the simulator shows OpenSSL races between webrtcbin's `nicesrc` and `queue` threads inside GStreamer's DTLS handshake (docs/23 notes) | upstream; `make opsim-all` retries a scenario once on a signal death, never on a failed check; the agent-side soak keeps watching the robot |
| lab `agent` spec, hot-plug, one run | the presentation gap budget (6 frames) was exceeded once with the viewer's Chromium tab open beside it; passed on the rerun | the known load-sensitive presentation check (docs/23 slice 3c notes); not a defect of this slice |

## Reviewer findings

| # | Severity | Finding | Resolution |
|---|---|---|---|
| 1 | high | gating `GET /` by the token would make the viewer unreachable from a browser, which navigates without a header | the endpoint decides static-or-API before the token check: the page and its assets are public and reveal nothing; every API route needs the token; the viewer keeps the token in session storage and sends it on each request (docs/24 amended) |
| 2 | high | every existing consumer of the endpoint inside the demo — the make targets' `curl`s, `fjarr-opsim --introspect`, the lab's `curl` fallback — would break the moment the demo set a token | the Makefile includes `.env` and sends the token from one `ROBOT_CURL` variable; `fjarr-opsim` takes `--introspect-token` and falls back to `FJARR_INTROSPECT_TOKEN` in its environment (set by compose in the robot's container); the lab defaults its token to the compose default and now reaches the endpoint over HTTP on the compose network instead of `curl` inside the container |
| 3 | medium | the compose override had to drop the dev image's workspace mount and bash wrapper, which a plain merge cannot express | `!reset` on `volumes` and `command` (Compose v2.24+); documented in docs/12 |
| 4 | medium | the Dockerfile's node stage with only the viewer's packages copied would fail `pnpm install --frozen-lockfile` (the lockfile names every workspace importer) | the other importers' manifests are copied as files, so the lockfile stays valid and only the viewer's dependency closure is installed |
| 5 | low | the CMake presets set `ccache` as the compiler launcher, which the image's build stage does not have | the stage configures CMake directly with `-DCMAKE_BUILD_TYPE=Release` |
| 6 | low | a `build/` tree of ~2 GB and `node_modules/` would enter the image build context from the repo root | `.dockerignore` |

## Gate (docs/23 slice 5b)

1. `make introspect` opens the viewer, which renders the demo robot's session graph — `tests/stack/viewer.spec.ts` enters the token in the lab browser, selects the connected robot's session pipeline and counts the SVG nodes d3-graphviz drew; the summary tab shows the pipeline's text.
2. The token: `/pipelines` refused without it and served with it, the page served without it (`viewer.spec.ts`, and `test_introspect.cpp` at unit level).
3. Serving tests: index at the root, hashed assets immutable, a plain file `no-cache`, traversal and a symlink out refused, a missing file 404, `POST` refused, the API shadowing a file of the same name, a robot without the directory answering the text index that names the key (`test_introspect.cpp`).
4. CI builds the image, runs the demo robot from it under the viewer, introspect and dashboard specs, and the runtime stage fails its own build if `x264enc` is present (`docker/agent/Dockerfile`, `.github/workflows/ci.yml`).
5. Nothing is published: the images are tagged locally in CI and discarded.

## Deferred

- Publishing to GHCR, arm64, the per-vendor layers and the `fjarr-agent` package that installs the viewer under `/usr/share/fjarr/viewer`: M2.5.
- The image's healthcheck and entrypoint assume environment configuration; a config file layout for the container arrives with the package.
- The viewer has no way to look at another robot than the one that served it beyond `?endpoint=` for a dev server; fleet-wide browsing is Fjarr Cloud's (M7).
