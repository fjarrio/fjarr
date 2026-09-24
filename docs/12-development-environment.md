---
title: Development Environment
description: Devcontainer, compose services, the doctor, and troubleshooting.
---

Everything runs in containers; the host needs only Docker + VS Code. The
environment is **self-verifying**: `make doctor` is the authority on whether
your setup can do what the specs assume.

## Quickstart

```bash
git clone <repo> fjarr && cd fjarr
cp .env.example .env            # then check RENDER_GID matches your host:
stat -c %g /dev/dri/renderD128  # → RENDER_GID in .env
# recommended, once per host: let ThreadSanitizer run inside containers (below)
sudo sysctl -w vm.mmap_rnd_bits=28 && echo 'vm.mmap_rnd_bits=28' | sudo tee /etc/sysctl.d/60-tsan.conf
code .                          # "Reopen in Container"
# post-create runs pnpm install, cargo fetch, cmake configure, doctor
make doctor
```

Or without VS Code: `docker compose up -d dev robot-sim`, then
`docker compose exec dev bash`.

## Services (docker-compose.yml)

| Service | Profile | Purpose |
|---|---|---|
| `dev` | default | the toolbox: Ubuntu 26.04 / C++ (clang 21) / GStreamer 1.28 + Rust + Node 22 (see the Dockerfile's commented package groups; [ADR-0022](adr/0022-baseline-ubuntu-2604-gstreamer-128.md)) |
| `robot-sim` | default | Xvfb fake robot desktop at `:99` (openbox + moving apps); **watch it at <http://localhost:6080>** (noVNC) |
| `fjarr-server` | `demo`, `stack` | the production sidecar image, built from `signaling/` only |
| `demo-backend` | `demo` | the TS "customer backend" beside the sidecar — mints real HS256 grants with `FJARR_GRANT_HS256_SECRET` |
| `demo-robot` | `demo` | the C++ "customer robot" capturing robot-sim |
| `demo-dashboard` | `demo` | Vite dev server on <http://localhost:5173> |
| `rtsp-sim` | `demo` | a network-camera stand-in: GStreamer's RTSP server on `rtsp://rtsp-sim:8554/pattern`, the demo robot's `fjarr.camera` `rtsp` track (slice 4) |
| `coturn` | `turn` | TURN relay in `use-auth-secret` mode (ephemeral creds only) |
| `browser` | `lab` | headless Chromium with CDP on <http://localhost:9222> and fake media devices — the [browser lab](25-browser-lab.md) |

`make demo-up` = `docker compose --profile demo up -d` — the full customer
topology. The `dev` and `robot-sim` containers share `/tmp/.X11-unix` via the
`x11sock` volume, which is why `DISPLAY=:99` works inside `dev`. The demo
dashboard's sidebar has a **role** picker (operator / developer): the demo
backend mints the grant for the role on the next connect, and only the
developer role's grant carries `fjarr.introspect`, so its *Diagnostics*
panel shows the robot's live pipeline graphs while an operator sees
"not available for this role" ([docs/24](24-pipeline-introspection.md)).
The demo backend and the dashboard run from the mounted sources: after
pulling a change to either, `docker compose --profile demo restart
demo-backend demo-dashboard` (and `demo-robot` after `make agent-build`).

## The doctor {#doctor}

`make doctor` → `.devcontainer/doctor.sh`. Checks, and what a failure means:

| Check | On failure |
|---|---|
| toolchains (cmake/cargo/node/…) | image build incomplete — rebuild `dev` |
| GStreamer elements (`webrtcbin`, `vah264enc`, `ximagesrc`, …) | missing plugin package — see Dockerfile groups |
| `x264enc` **absent** | if present: GPL plugin leaked in — ADR-0011 violation, fix the image |
| `/dev/dri` accessible | `RENDER_GID` in `.env` ≠ host render group; fix + `docker compose build dev` |
| `vainfo` encode entrypoints + `vah264enc` smoke pipeline | iHD driver problem — see GPU troubleshooting. A **warning** instead when `FJARR_MEDIA_ENCODER=software`: a machine with no Intel GPU (an NVIDIA or AMD runner) has deliberately chosen the CPU path, which is not a broken setup |
| `DISPLAY=:99` + `ximagesrc` capture | robot-sim not up, or stale X socket — `docker compose restart robot-sim` |
| `libei`/`pipewire`/`libevdev` pkg-config | dev packages missing from image |
| `/dev/uinput` | WARN by default; opt in via the uinput override |

## Opt-in overrides (conscious privilege grants)

**Host desktop capture** — develop against your real screen:

```bash
xhost +local:   # host, once per session
docker compose -f docker-compose.yml -f docker-compose.host-x11.yml up -d dev
# inside dev: DISPLAY is your host display now
```

Caveat: grants the container your entire screen + input access. Revert with
`xhost -local:` and plain `docker compose up -d dev`.

**uinput experiments**:

```bash
sudo modprobe uinput
docker compose -f docker-compose.yml -f docker-compose.uinput.yml up -d dev
```

Caveat: the container can synthesize input on the **host** kernel.

**Host webcam for `fjarr.camera`** (slice 4) — the demo robot's `webcam`
track reads `/dev/v4l/by-id/<name>`; pass the host's video devices in:

```bash
ls -l /dev/v4l/by-id/          # which node the …-video-index0 name points at
FJARR_WEBCAM_DEVICE=/dev/video2 FJARR_DEMO_WEBCAM=usb-Acme_Cam-video-index0 \
  docker compose -f docker-compose.yml -f docker-compose.camera.yml --profile demo up -d demo-robot
```

Without the override the track is `unavailable` (reason in
`curl localhost:7381/sources`) and simply absent from the manifest — the
docs/26 missing-device behaviour, exercised by CI. The demo reads
`/dev/video0` by default; set `FJARR_DEMO_WEBCAM` to a `/dev/v4l/by-id`
name (the stable form real config uses) or another node. A container's
`devices:` are static, so unplug/replug is verified by the unit tests and
on bare metal, not through this override. `fjarr-agent --probe-source
'{type = "v4l2", device = "/dev/video0"}'` validates a camera before it
goes into `fjarr.toml`.

## GPU / VA-API troubleshooting

- `vainfo` errors inside `dev` → check `LIBVA_DRIVER_NAME=iHD` (set in the
  image) and that `/dev/dri` is mapped (compose `devices:`).
- Permission denied on `renderD128` → `.env` `RENDER_GID` must equal
  `stat -c %g /dev/dri/renderD128`; rebuild `dev` after changing.
- No `/dev/dri` at all (CI, cloud VM) → doctor degrades to WARN; software
  encoders cover dev work but never performance claims.
- Machine-specific note (Meteor Lake NUCs and newer): only the iHD driver
  supports encode; `mesa-va-drivers` is present for completeness.
- **The iGPU stuck at its floor clock** (VA-API sessions at 10–15 fps while
  `gst-launch` alone manages 30, the software encoder streams 30, and the
  robot's CPU is idle): the package power limit throttled the GPU and the
  clock stayed at its minimum afterwards. Read
  `/sys/class/drm/card*/gt/gt0/rps_act_freq_mhz` under load and
  `throttle_reason_pl1`; on a laptop check the power source and profile.
  A day of heavy builds and sanitizer runs did this once, helped along by
  the sim's `glxgears` rendering unthrottled through llvmpipe on ~3.5 cores
  (removed since). The software encoder path — what CI measures — is
  unaffected; VA-API numbers taken in that state are not performance data.

## TURN sanity check

```bash
docker compose --profile turn up -d coturn
# mint an ephemeral credential by hand (docs/10#turn):
u="$(($(date +%s)+600)):manual-test"
p=$(printf %s "$u" | openssl dgst -sha1 -hmac "$TURN_SECRET" -binary | base64)
docker run --rm --network host --entrypoint turnutils_uclient \
  coturn/coturn:4.6-alpine -u "$u" -w "$p" -p 3478 -e 127.0.0.1 -n 2 127.0.0.1
# Valid cred ⇒ allocation succeeds, then "403 (Forbidden IP)" on channel
# bind — that's coturn's default loopback-peer block, i.e. auth WORKED.
# A wrong cred fails earlier with "Cannot complete Allocation".
```

## Caches & volumes

Named volumes keep rebuilds fast: `cargo-registry`, `pnpm-store`, `ccache`,
`x11sock`. `docker compose down -v` wipes them (first build after that is
slow again).

## Browser lab

`make lab-up` starts the `browser` service (headless Chromium, CDP at
<http://127.0.0.1:9222/json> from the host) beside `fjarr-server`;
`make e2e` runs the Playwright/CDP suites from `dev` (`make e2e-loopback`
needs no server); `pnpm fjarr-lab <command>` drives the same browser ad
hoc (`open`, `net lossy`, `wire --follow`, `profile cpu 10`,
`memory --cycles 20`…). Artifacts land in `web/e2e/out/`. The `dev` image
carries the docker CLI with the host socket mounted so the lab can apply
netem, pause or restart stack containers from inside — set `DOCKER_GID`
in `.env` to the group owning `/var/run/docker.sock`. Details and the
environment variables: [docs/25](25-browser-lab.md#implementation-notes-slice-3a).

## ThreadSanitizer in the container

TSan keeps its shadow memory at fixed addresses; with the 32 bits of ASLR
entropy recent kernels default to, the program's own mappings can land on
them and TSan refuses to start ("incompatible memory layout … unable to
disable ASLR"). Its usual self-repair — re-exec with
`personality(ADDR_NO_RANDOMIZE)` — is blocked by Docker's default seccomp
profile, and so is `setarch -R`. The fix is a host kernel setting, which
every container inherits (the recommended one-time step in the quickstart):

```bash
sudo sysctl -w vm.mmap_rnd_bits=28                                # now
echo 'vm.mmap_rnd_bits=28' | sudo tee /etc/sysctl.d/60-tsan.conf   # after reboots
```

28 bits of randomisation instead of 32 is the sanitizer maintainers' own
recommendation and immaterial on a development machine. The doctor probes
this functionally (it compiles and runs a one-line TSan program) and WARNs
when `make agent-test-tsan` would not run. TSan is a gate (docs/15): CI's
`cpp` job runs inside a container and cannot set the sysctl, so the gate
runs in the runner-level `e2e` job, which sets it first. `agent/tests/tsan.supp`
lists the modules TSan cannot see into (GLib, GStreamer, libnice, libsoup
are not instrumented, so their locks are invisible and every hand-off through
their queues would otherwise be reported); the RAII kit pairs its own
hand-offs (posts, sources, promises, thread-pool jobs) with an acquire/release
so a race with both ends in fjarr code is always reported.

## Nightly CI and the self-hosted GPU runner

`.github/workflows/nightly.yml` (docs/15) runs the 200-cycle soak, valgrind,
heaptrack, the `netem-*` scenarios and the latency harness every night and
on demand. Its `runs-on` is the repository variable `FJARR_NIGHTLY_RUNNER`,
falling back to GitHub's `ubuntu-24.04`.

> **This repository is public, so a self-hosted runner may only serve
> workflows that untrusted code cannot reach.** Anyone can open a pull
> request, and a workflow that runs on it executes their code on your
> machine. `nightly.yml` is safe by construction: it triggers on `schedule`
> and `workflow_dispatch`, which only ever run from the default branch and
> which a fork cannot trigger. **`ci.yml` must stay on hosted runners** —
> it runs on `push` and `pull_request`. Also set Settings → Actions →
> *Fork pull request workflows* to require approval for all outside
> collaborators, and prefer a machine that is not someone's daily desktop.

Setting one up:

1. **On the machine**: Ubuntu 24.04 or later, Docker Engine with Compose v2,
   the runner's user in the `docker` group (and `render`/`video` when
   `/dev/dri` exists), `vm.mmap_rnd_bits=28` in `/etc/sysctl.d/` (TSan,
   above) or passwordless sudo for the job's `sysctl`, `make`, and enough
   disk for the dev image plus build trees.
2. GitHub → Settings → Actions → Runners → *New self-hosted runner*;
   install it as a service and give it a label you will point at.
3. GitHub → Settings → Secrets and variables → Actions → *Variables*:
   `FJARR_NIGHTLY_RUNNER = <label>`. Unset it to fall back to hosted.

**A render node is not an Intel one.** Our hardware encode path is VA-API
([ADR-0022](adr/0022-baseline-ubuntu-2604-gstreamer-128.md); NVENC is
[open question #2](18-open-questions.md)), and an NVIDIA card presents
`/dev/dri/renderD128` while offering no VA-API H.264 encoder. The nightly
therefore **probes the pipeline** rather than trusting the device node, and
sets `media.encoder` to `software` when the probe fails — the explicit,
never-silent fallback of [docs/23](23-agent-core-architecture.md). The
doctor treats a failed VA-API smoke as a warning when the encoder has been
chosen as `software`, and as a failure otherwise. So a big AMD or NVIDIA
box is a fine runner; it just will not exercise the VA-API path.

For the [latency harness](15-testing-strategy.md#latency-harness) the same
runner takes `FJARR_LATENCY_LABEL` (the hardware, for the CSV) and
`FJARR_LATENCY_STRICT=1` (hold the run to the docs/16 budgets). Turn strict
on only for a machine nobody is using: the budgets assume a quiet host, and
the harness refuses a run that could not decode the source rate anyway.

## Pipeline introspection

With the demo profile up, `make introspect` (no arguments) opens the
agent's live pipeline viewer at `http://localhost:7381/`
([docs/24](24-pipeline-introspection.md#the-viewer)) and prints the token
to paste. The endpoint is loopback-only inside the robot container, so
the demo profile binds it on the container's interface with the dev token
`FJARR_INTROSPECT_TOKEN` (`.env.example`; the Makefile reads `.env` for
it) and publishes it on the host's loopback only; the viewer asks for the
token once per tab. `make introspect PIPELINE=<id> [FORMAT=json|dot]`
prints one pipeline, `make introspect SUMMARIES=1` every pipeline's
summary, and from the host
`curl -H "Authorization: Bearer $FJARR_INTROSPECT_TOKEN" localhost:7381/pipelines`
is the raw API — the first thing to look at when media misbehaves, and
what `/verify` and the e2e tests assert against.

### Running the demo robot from the agent image

The demo robot runs in the dev image with the workspace mounted, so an
agent change is `make agent-build` and a container restart, never an
image rebuild. The product artifact is the `fjarr-agent` image
(`docker/agent/Dockerfile`, repo-root context: a node stage builds the
viewer, a build stage the library and daemon, the runtime stage is Ubuntu
26.04 with the runtime GStreamer and VA-API packages, no compilers, a
non-root user, the viewer under `/usr/share/fjarr/viewer`, a healthcheck
on the endpoint, and a build that fails if `x264enc` is present — the
doctor's rule applied to the artifact; `.dockerignore` keeps `build/`,
`node_modules/` and `inspiration/` out of the context). A `demo` stage
layers the demo robot binary on the runtime image, the topology an
embedding customer uses. To run the demo from it:

```bash
docker compose -f docker-compose.yml -f docker-compose.image.yml --profile demo up -d --build demo-robot
```

CI runs the lab smoke against this variant on every push and discards
the image; publishing to GHCR arrives with M2.5's release process
([docs/26](26-robot-install-and-drivers.md#distribution-channels)).

## Make targets

`doctor` · `agent-configure/build/test` · `agent-test-asan/tsan` ·
`agent-raii-gate` · `agent-leaks-selftest` · `opsim` / `opsim-all` /
`opsim-soak` (`OPSIM_CYCLES`) / `opsim-netem` (`NETEM_PROFILE`) (the
docs/23 operator simulator against the demo robot) ·
`agent-leaks` (`SCENARIO=`, recreates the robot with GStreamer's leaks
tracer and prints what stayed alive) · `agent-memcheck` · `agent-heaptrack`
(docs/15 memory safety, nightly) · `introspect` (`PIPELINE=`, `FORMAT=`:
the endpoint from the terminal, docs/24) · `signaling-run/test/clippy` ·
`web-dev/build/lint` · `sim-up` · `demo-up/down` · `stack-up` ·
`website-dev/build` · `docs-lint` · `docs-links` · `fmt` · `lint`.
`.vscode/tasks.json` wraps the same targets — terminal and IDE never diverge.
