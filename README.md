# Fjarr

**Generic peer-to-peer connectivity for robot fleets** — live camera video,
remote desktop, sensor streaming, file transfer, and a remote terminal over
WebRTC, shipped as three embeddable libraries with a plugin architecture.

*Fjarr* is Swedish for "remote" (as in *fjärrkontroll* — remote control).

> **Status: M0 — documentation & environment.** Fjarr is being built
> documentation-first: the specs below are the product taking shape; the
> code is skeletons proving the toolchain and boundaries. See the
> [roadmap](docs/17-roadmap.md).

## What it will be

| Tier | You embed | Into |
|---|---|---|
| Robot | `libfjarr` (C++/GStreamer) or the `fjarr-agent` daemon | your robot's Ubuntu system |
| Backend | the `fjarr-server` sidecar (or Fjarr Cloud) via a small token/REST/webhook contract | your existing backend, any stack |
| Dashboard | `@fjarr/core` + `@fjarr/react` | your existing React dashboard |

Everything user-visible is a **capability plugin** — cameras, desktop,
files, terminal, telemetry now; fleet observability and SWUpdate-based
atomic OTA later; your own capabilities without forking
([extension model](docs/05-extension-model.md)).

## Vision & business direction

Open core (AGPL-3.0 + commercial licensing) with a managed twin: everything
you need runs self-hosted for free; **Fjarr Cloud** hosts the
signaling/TURN/fleet layer and the flagship fleet features (observability,
OTA campaigns), metered by robots, session-minutes, and relay bandwidth.
The full living business plan: [docs/03-product-strategy.md](docs/03-product-strategy.md).

## Quickstart (development)

```bash
cp .env.example .env     # check RENDER_GID: stat -c %g /dev/dri/renderD128
code .                   # → "Reopen in Container"
make doctor              # the environment proves itself
# fake robot desktop: http://localhost:6080
```

Details: [docs/12-development-environment.md](docs/12-development-environment.md).

## Documentation

Start at the **[documentation index](docs/README.md)** — reading order,
per-doc status, and the [ADR log](docs/adr/README.md). Highlights:
[Vision](docs/00-vision.md) · [Architecture](docs/02-architecture.md) ·
[Capabilities](docs/06-capabilities.md) · [Protocol](docs/08-protocol.md) ·
[Interfaces](docs/09-interfaces.md) · [Security](docs/10-security.md) ·
[Prior art](docs/11-prior-art.md).

## Repository layout

```text
agent/       libfjarr (C++) + fjarr-agent daemon
signaling/   fjarr-signaling crate + fjarr-server sidecar (Rust)
web/         @fjarr/core + @fjarr/react (TypeScript)
demos/       three separated "customer" apps — public APIs only
website/     fjarr.io: landing + docs site (Astro + Starlight)
docs/        the specs (normative) + ADRs
protocol/    machine-readable schemas (JSON Schema, OpenAPI)
docker/      robot-sim + production server images
```

## License

AGPL-3.0 (planned; formal license files land with the first public release —
[ADR-0011](docs/adr/0011-license-open-core.md)). Commercial licensing will be
available.
