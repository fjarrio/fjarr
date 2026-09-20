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
| `coturn` | `turn` | TURN relay in `use-auth-secret` mode (ephemeral creds only) |
| `browser` | `lab` | headless Chromium with CDP on <http://localhost:9222> and fake media devices — the [browser lab](25-browser-lab.md) |

`make demo-up` = `docker compose --profile demo up -d` — the full customer
topology. The `dev` and `robot-sim` containers share `/tmp/.X11-unix` via the
`x11sock` volume, which is why `DISPLAY=:99` works inside `dev`.

## The doctor {#doctor}

`make doctor` → `.devcontainer/doctor.sh`. Checks, and what a failure means:

| Check | On failure |
|---|---|
| toolchains (cmake/cargo/node/…) | image build incomplete — rebuild `dev` |
| GStreamer elements (`webrtcbin`, `vah264enc`, `ximagesrc`, …) | missing plugin package — see Dockerfile groups |
| `x264enc` **absent** | if present: GPL plugin leaked in — ADR-0011 violation, fix the image |
| `/dev/dri` accessible | `RENDER_GID` in `.env` ≠ host render group; fix + `docker compose build dev` |
| `vainfo` encode entrypoints + `vah264enc` smoke pipeline | iHD driver problem — see GPU troubleshooting |
| `DISPLAY=:99` + `ximagesrc` capture | robot-sim not up, or stale X socket — `docker compose restart robot-sim` |
| `libei`/`pipewire`/`libevdev` pkg-config | dev packages missing from image |
| `/dev/uinput` | WARN by default; opt in via the uinput override |
| `webrtcsink` | WARN until the ADR-0007 spike layer exists |

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

## GPU / VA-API troubleshooting

- `vainfo` errors inside `dev` → check `LIBVA_DRIVER_NAME=iHD` (set in the
  image) and that `/dev/dri` is mapped (compose `devices:`).
- Permission denied on `renderD128` → `.env` `RENDER_GID` must equal
  `stat -c %g /dev/dri/renderD128`; rebuild `dev` after changing.
- No `/dev/dri` at all (CI, cloud VM) → doctor degrades to WARN; software
  encoders cover dev work but never performance claims.
- Machine-specific note (Meteor Lake NUCs and newer): only the iHD driver
  supports encode; `mesa-va-drivers` is present for completeness.

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

`make agent-test-tsan` fails with "incompatible memory layout … unable to
disable ASLR" unless the host lowers ASLR entropy: `sudo sysctl -w
vm.mmap_rnd_bits=28` (Docker's default seccomp profile blocks the
`personality` call `setarch -R` would use inside the container). TSan is a
trend in slice 3b, a gate from 3c (docs/15).

## Pipeline introspection

With the demo profile up, `make introspect` opens the agent's live pipeline
viewer (`http://localhost:7381/`, [docs/24](24-pipeline-introspection.md));
`curl localhost:7381/pipelines` lists pipelines and
`curl localhost:7381/pipelines/<id>.txt` prints a one-screen summary — the
first thing to look at when media misbehaves, and what `/verify` and the
e2e tests assert against.

## Make targets

`doctor` · `agent-configure/build/test` · `agent-test-asan/tsan` ·
`agent-raii-gate` · `opsim` / `opsim-all` (the docs/23 operator simulator
against the demo robot) ·
`agent-leaks` · `agent-memcheck` · `agent-heaptrack` (docs/15 memory
safety) · `signaling-run/test/clippy` ·
`web-dev/build/lint` · `sim-up` · `demo-up/down` · `stack-up` ·
`website-dev/build` · `docs-lint` · `docs-links` · `fmt` · `lint`.
`.vscode/tasks.json` wraps the same targets — terminal and IDE never diverge.
