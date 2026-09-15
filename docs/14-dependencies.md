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

| Dependency | Version (noble) | Purpose | License | Ships |
|---|---|---|---|---|
| GStreamer core + base/good/bad plugins | 1.24.2 | pipelines, RTP, WebRTC | LGPL-2.1 (dynamic) | yes |
| `gstreamer1.0-nice` (libnice) | 0.1.21 | ICE for webrtcbin | LGPL-2.1/MPL | yes |
| `gstreamer1.0-pipewire` | 1.0.5 | Wayland capture (`pipewiresrc`) | MIT | yes |
| libva + intel-media-driver (iHD) | 2.20 / 24.1 | VA-API H.264 encode | MIT | yes (driver from distro) |
| libx11 / libxtst / libxfixes / libxrandr / libxi | noble | X11 backend | MIT/X11 | yes |
| libei | 1.2.1 | Wayland input injection | MIT | yes |
| libpipewire | 1.0.5 | portal capture | MIT | yes |
| libevdev | 1.13.1 | uinput helper | MIT-ish (X11) | yes (in `fjarr-inputd`) |
| libdbus / sd-bus | noble | portal negotiation | AFL-2.1/GPL dual → use LGPL path; verify at M2 | yes |
| nlohmann-json | 3.11 | envelopes, config | MIT | yes |
| libsoup-3 (+ glib-networking) | 3.4 | WS/HTTP signaling client (ADR-0017) | LGPL-2.1 (dynamic) | yes |
| **Forbidden**: `gstreamer1.0-plugins-ugly` (x264enc) | — | — | GPL | **never** (doctor-enforced) |

## Signaling (`fjarr-signaling` / `fjarr-server`) — ships as sidecar/Cloud

| Dependency | Version | Purpose | License | Ships |
|---|---|---|---|---|
| Rust toolchain | 1.89.0 (pinned) | build | MIT/Apache-2.0 | build-only |
| tokio | 1.x | async runtime | MIT | yes |
| axum (+ `ws`) | 0.8 | HTTP + WebSocket | MIT | yes |
| tracing / tracing-subscriber | 0.1/0.3 | structured logs | MIT | yes |
| serde / serde_json | 1.x | envelopes | MIT/Apache-2.0 | yes |
| (M1+) jsonwebtoken, hmac/sha1, uuid | — | grants, TURN creds, ids | MIT/Apache-2.0 | yes |

## Web (`@fjarr/core`, `@fjarr/react`) — ships to customer bundles

| Dependency | Version | Purpose | License | Ships |
|---|---|---|---|---|
| TypeScript | ^5.9 | build | Apache-2.0 | build-only |
| React (peer dep) | ≥ 19 | `@fjarr/react` only | MIT | peer |
| (M2+) xterm.js | — | terminal view | MIT | yes |
| `@fjarr/core` runtime deps | **none** (design goal) | keep the core dependency-free | — | — |

## Demos, website, tooling — never shipped

| Dependency | Version | Purpose |
|---|---|---|
| Vite | ^8 | demo-dashboard dev/build |
| Astro + Starlight | ^7 / ^0.42 | website + docs rendering |
| pnpm (via corepack) | 10.x | JS workspace |
| markdownlint-cli2, lychee | latest | docs gates |
| ajv | ^8 | protocol schema conformance gate (`make protocol-check`) |
| CMake/Ninja/ccache, clang-18 suite | noble | C++ build/lint |
| GoogleTest (M1) | — | C++ tests (BSD-3) |
| coturn (container) | 4.6 | dev/self-host TURN (BSD-3) |
| Xvfb/openbox/x11vnc/noVNC (robot-sim) | noble | fake robot desktop |
| Docker + Compose v2 | ≥ 24 | the environment itself |

## Version discipline

Toolchains are pinned (`rust-toolchain.toml`, `packageManager` field,
Dockerfile base images by tag). Distro libraries float within Ubuntu 24.04
LTS. `Cargo.lock` and `pnpm-lock.yaml` are committed. Upgrades are ordinary
PRs with a dependencies-row diff; base-image bumps get a changelog note.
