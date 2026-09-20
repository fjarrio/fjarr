---
title: Dependencies
description: Every dependency with version, purpose, license, and whether it ships.
---

Policy ([ADR-0011](adr/0011-license-open-core.md)): shipped artifacts may
depend on **permissive or LGPL (dynamically linked)** components only — no
GPL. Dev-only tools are unrestricted. **Adding a dependency requires adding
its row here in the same PR.** The devcontainer Dockerfile mirrors these
groups with comments.

## Agent (`libfjarr`) — ships on robots

| Dependency | Version (Ubuntu 26.04 LTS, ADR-0022; re-verified in slice 2.9) | Purpose | License | Ships |
|---|---|---|---|---|
| GStreamer core + base/good/bad plugins | 1.28.2 | pipelines, RTP, WebRTC | LGPL-2.1 (dynamic) | yes |
| `gstreamer1.0-nice` (libnice) | 0.1.23 | ICE for webrtcbin | LGPL-2.1/MPL | yes |
| `gstreamer1.0-pipewire` | 1.6.2 | Wayland capture (`pipewiresrc`) | MIT | yes |
| libva + intel-media-driver (iHD) | 2.22 / 26.1 | VA-API H.264 encode | MIT | yes (driver from distro) |
| libx11 / libxtst / libxfixes / libxrandr / libxi | 26.04 | X11 backend | MIT/X11 | yes — in `fjarr-desktop-x11` only ([ADR-0021](adr/0021-desktop-backends-as-runtime-modules.md)) |
| libei | 1.5.0 | Wayland input injection | MIT | yes — in `fjarr-desktop-wayland` only |
| libpipewire (+ `gstreamer1.0-pipewire`) | 1.6.2 | portal capture | MIT | yes — in `fjarr-desktop-wayland` only |
| libevdev | 1.13.6 | uinput helper | MIT-ish (X11) | yes (in `fjarr-inputd`) |
| libdbus / sd-bus | 26.04 | portal negotiation | AFL-2.1/GPL dual → use LGPL path; verify at M2 | yes — in `fjarr-desktop-wayland` only |
| nlohmann-json | 3.11 | envelopes, config | MIT | yes |
| openh264 (`openh264enc`, plugins-bad) | 2.4 | the explicit `encoder = "software"` path (CI, no-GPU dev, portable robots) — never a silent fallback (docs/23) | BSD-2 | yes (optional) |
| libsoup-3 (+ glib-networking) | 3.6 | WS/HTTP signaling client (ADR-0017) and the introspection server (docs/24) | LGPL-2.1 (dynamic) | yes |
| toml++ (`tomlplusplus`) | 3.4 (header-only) | `fjarr.toml` config (docs/23) | MIT | yes |
| nlohmann json-schema-validator | 2.3 (no Ubuntu package: pinned via CMake `FetchContent`, built into `libfjarr`) | capability config validation against `config_schema` | MIT | yes |
| libsystemd (`sd_notify`) | 26.04 | READY/WATCHDOG supervision — required by the packaged agent (ADR-0019 addendum) | LGPL-2.1 (dynamic) | yes |
| GoogleTest | 1.17 | C++ unit/loop tests | BSD-3 | dev-only |
| Vendor camera SDKs (ZED, RealSense, Jetson multimedia, GigE vendors…) | per vendor | **never a `libfjarr` dependency** — each ships as its own GStreamer plugin package `fjarr-gst-<vendor>` per architecture ([ADR-0020](adr/0020-vendor-sources-as-gstreamer-plugins.md)) | per vendor (checked per package) | optional, separate packages |
| **Forbidden**: `gstreamer1.0-plugins-ugly` (x264enc) | — | — | GPL | **never** (doctor-enforced) |

## Signaling (`fjarr-signaling` / `fjarr-server`) — ships as sidecar/Cloud

| Dependency | Version | Purpose | License | Ships |
|---|---|---|---|---|
| Rust toolchain | 1.89.0 (pinned) | build | MIT/Apache-2.0 | build-only |
| tokio | 1.x | async runtime | MIT | yes |
| axum (+ `ws`) | 0.8 | HTTP + WebSocket | MIT | yes |
| tracing / tracing-subscriber | 0.1/0.3 | structured logs | MIT | yes |
| serde / serde_json | 1.x | envelopes | MIT/Apache-2.0 | yes |
| futures-util | 0.3 | WS stream/sink combinators | MIT/Apache-2.0 | yes |
| jsonwebtoken | 9 | session-grant verification (HS256 at M1) | MIT | yes |
| hmac + sha1 + sha2 | 0.12/0.10 | TURN ephemeral creds (SHA1, coturn format), webhook signatures (SHA256) | MIT/Apache-2.0 | yes |
| base64 | 0.22 | TURN credential encoding | MIT/Apache-2.0 | yes |
| subtle | 2 | constant-time token comparison | BSD-3 | yes |
| uuid (v7) | 1.x | event/session ids | MIT/Apache-2.0 | yes |
| reqwest (rustls) | 0.12 | webhook delivery | MIT/Apache-2.0 | yes |
| tokio-tungstenite | 0.24 | e2e test WS client | MIT | dev-only |

## Web (`@fjarr/core`, `@fjarr/react`) — ships to customer bundles

| Dependency | Version | Purpose | License | Ships |
|---|---|---|---|---|
| TypeScript | ^5.9 | build | Apache-2.0 | build-only |
| React (peer dep) | ≥ 19 | `@fjarr/react` only | MIT | peer |
| (M2+) xterm.js | — | terminal view | MIT | yes |
| `@fjarr/core` runtime deps | **none** (design goal) | keep the core dependency-free | — | — |
| vitest | ^5 | unit tests (core, react) | MIT | dev-only |
| happy-dom | ^20 | DOM for `@fjarr/react` hook tests | MIT | dev-only |
| @testing-library/react | ^16 | hook/component tests | MIT | dev-only |
| d3-graphviz + @hpcc-js/wasm | ^5 / ^2 | DOT rendering in `<PipelineGraph>` and the on-robot viewer (docs/24) | BSD-3 / Apache-2.0 | yes (`@fjarr/react` optional entry; viewer bundled with the agent) |
| `@playwright/test` (+ pinned Chromium image) | 1.63.0 (pinned) | browser lab harness and e2e (docs/25; rows below) | Apache-2.0 | dev-only |
| Vite + `@vitejs/plugin-react` | ^8 / ^5 | serves the lab page (`web/e2e/app`) | MIT | dev-only |

## Demos, website, tooling — never shipped

| Dependency | Version | Purpose |
|---|---|---|
| Vite | ^8 | demo-dashboard dev/build |
| Astro + Starlight | ^7 / ^0.42 | website + docs rendering |
| pnpm (via corepack) | 10.x | JS workspace |
| markdownlint-cli2, lychee | latest | docs gates |
| ajv | ^8 | protocol schema conformance gate (`make protocol-check`) |
| CMake/Ninja/ccache, clang-21 suite | 26.04 | C++ build/lint |
| valgrind, heaptrack | 3.26 / 1.5 | nightly memcheck and allocation profiling of the agent (docs/15 memory safety; GPL tools, dev-only, never linked) |
| `@playwright/test` (`@fjarr/e2e`, never published) | 1.63.0 (pinned; the `browser` image tag follows it) | browser lab harness and `fjarr-lab` (Apache-2.0) |
| `mcr.microsoft.com/playwright:v1.63.0-noble` (container) | 1.63.0 | the lab's headless Chromium with CDP (docs/25) |
| `docker-cli`, `docker-compose-v2` (dev image) | 26.04 | docker-outside-of-docker: the lab drives the compose stack from `dev` (Apache-2.0) |
| `iproute2` (`tc`, dev image) | 26.04 | netem media-path profiles inside the robot container (GPL-2.0 tool, dev-only, never linked) |
| coturn (container) | 4.6 | dev/self-host TURN (BSD-3) |
| Xvfb/openbox/x11vnc/noVNC (robot-sim) | 26.04 | fake robot desktop |
| Docker + Compose v2 | ≥ 24 | the environment itself |

## Version discipline

Toolchains are pinned (`rust-toolchain.toml`, `packageManager` field,
Dockerfile base images by tag). Distro libraries float within Ubuntu 26.04
LTS. `Cargo.lock` and `pnpm-lock.yaml` are committed. Upgrades are ordinary
PRs with a dependencies-row diff; base-image bumps get a changelog note.
