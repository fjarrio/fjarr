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
| GStreamer core + base/good/bad plugins | 1.28.2 | pipelines, RTP, WebRTC; `gstreamer1.0-plugins-base-apps` for `gst-device-monitor-1.0`, which `fjarr-agent setup` and `drivers detect` run to find cameras (docs/26), a package dependency since 2026-09-28 | LGPL-2.1 (dynamic) | yes |
| `libx265` (pulled by Ubuntu's `gstreamer1.0-plugins-bad` for its `x265enc`) | 26.04 | **nothing**: present on disk, never loaded by the agent. `x265enc` is an encoder, encoders are never autoplugged, and the agent's encoder is explicit (VA-API, openh264). Recorded 2026-09-28, when the image's GPL guard was sharpened | GPL-2.0 | present, not a dependency: the rule (ADR-0011) is about what enters the agent's process. The image guard fails on `x264enc`, `avdec_*`, `libx264` and `libxvidcore` instead, which are what decodebin or a GPL encoder would load |
| `gstreamer1.0-nice` (libnice) | 0.1.23 | ICE for webrtcbin; the agent also links libnice directly (`libnice-dev` at build) to give it the local addresses to gather from, without the tunnel interface (docs/23, #34) | LGPL-2.1/MPL | yes |
| `gstreamer1.0-pipewire` | 1.6.2 | Wayland capture (`pipewiresrc`) | MIT | yes |
| libva + intel-media-driver (iHD; amd64 only — arm64 uses mesa's VA drivers) | 2.22 / 26.1 | VA-API H.264 encode | MIT | yes (driver from distro) |
| libx11 / libxtst / libxfixes / libxrandr / libxi | 26.04 | X11 backend | MIT/X11 | yes — in `fjarr-desktop-x11` only ([ADR-0021](adr/0021-desktop-backends-as-runtime-modules.md)) |
| libei | 1.5.0 | Wayland input injection | MIT | yes — in `fjarr-desktop-wayland` only |
| libpipewire (+ `gstreamer1.0-pipewire`) | 1.6.2 | portal capture | MIT | yes — in `fjarr-desktop-wayland` only |
| libevdev | 1.13.6 | uinput helper | MIT-ish (X11) | yes (in `fjarr-inputd`) |
| libdbus / sd-bus | 26.04 | portal negotiation | AFL-2.1/GPL dual → use LGPL path; verify at M2 | yes — in `fjarr-desktop-wayland` only |
| nlohmann-json | 3.11 | envelopes, config | MIT | yes |
| GStreamer `nvcodec` plugin (`nvh264enc`, plugins-bad) | 1.28 | the `nvcodec` encoder family for discrete NVIDIA cards ([ADR-0025](adr/0025-encoder-families.md), M2.6) | LGPL-2.1 (dynamic) | yes (optional) |
| NVIDIA driver libraries (`libnvidia-encode`, `libcuda`) | the host's driver | what `nvcodec` loads at runtime; **never linked or redistributed by us** — the machine's own driver install provides them, exactly as the Intel VA driver is | proprietary, not shipped | **no** |
| NVIDIA L4T GStreamer (`nvv4l2h264enc`) | JetPack | the `nvv4l2` encoder family on Jetson (M2.6); from NVIDIA's own repository on the board, arm64 only | NVIDIA SDK terms, not shipped | **no** |
| openh264 (`openh264enc`, plugins-bad) | 2.4 | the explicit `encoder = "software"` path (CI, no-GPU dev, portable robots) — never a silent fallback (docs/23) | BSD-2 | yes (optional) |
| libsoup-3 (+ glib-networking) | 3.6 | WS/HTTP signaling client (ADR-0017) and the introspection server (docs/24) | LGPL-2.1 (dynamic) | yes |
| toml++ (`tomlplusplus`) | 3.4 (header-only) | `fjarr.toml` config (docs/23) | MIT | yes |
| nlohmann json-schema-validator | 2.3 (no Ubuntu package: pinned via CMake `FetchContent`, built into `libfjarr`) | capability config validation against `config_schema` | MIT | yes |
| GStreamer `gstreamer.supp` (valgrind suppressions, vendored at `agent/tests/valgrind/`) | 1.28.2 source tree | `make agent-memcheck` (docs/23 memory ladder, nightly) | LGPL-2.1-or-later (a data file; never linked) | **no** — dev only |
| `python3-gi` + `gir1.2-gst-rtsp-server-1.0` + `gstreamer1.0-rtsp` | 26.04 | the lab's RTSP camera simulator (`docker/lab/rtsp-sim.py`, the `rtsp-sim` compose service) for `fjarr.camera`'s `rtsp` track | LGPL-2.1 (PyGObject, gst-rtsp-server) | **no** — dev image and CI only |
| `gstreamer1.0-libav` (`avdec_h264`) | 1.28 | receive-side H.264 decode in `fjarr-opsim` (docs/23) — the dev image and CI only. It had crept into the shipped agent image, where `decodebin` would autoplug it for RTSP; removed 2026-09-28, and the image guard now fails on it | LGPL-2.1 (Ubuntu's ffmpeg build enables GPL parts) | **no** — a test tool; never linked into `libfjarr`, `fjarr-agent` or a demo |
| libsystemd (`sd_notify`) | 26.04 | READY/WATCHDOG supervision — required by the packaged agent (ADR-0019 addendum) | LGPL-2.1 (dynamic) | yes |
| GoogleTest | 1.17 | C++ unit/loop tests | BSD-3 | dev-only |
| mutter, PipeWire, WirePlumber, GTK 4, libei (Ubuntu 26.04 packages) | 50.1 / 1.6 / 0.5 / 4.22 / 1.5 | the desktop fixture's headless GNOME session (`docker/desktop-fixture`, M3 slice 3.0): the compositor under test, its screencast plumbing, the test window, EIS | GPL-2.0+ (mutter), MIT (PipeWire, WirePlumber, libei), LGPL-2.1+ (GTK) | **no** — a test image only; never in a shipped artifact |
| FFmpeg dev packages, with `libx264` and `libxvidcore` (pulled by Ubuntu's `libgstreamer-plugins-base1.0-dev` and `-bad1.0-dev`) | 26.04 | nothing of Fjarr's: they arrive on any machine that **builds** against GStreamer, so on an embedder's build machine with `libfjarr-dev`. libfjarr links GStreamer, never FFmpeg, and the robot runtime packages (`fjarr-agent`, `fjarr-tools`) do not bring them (`make deb-install-test` checks) | GPL-2.0 | **no** — build machine only |
| debhelper, devscripts, equivs, fakeroot (the package builder, `docker/deb-builder`) | 26.04 | building the `.deb`s (docs/26#releases, ADR-0031) | GPL-2.0+ (build tools; nothing of theirs is in a package) | **no** — build only |
| Vendor camera SDKs (ZED, RealSense, Jetson multimedia, GigE vendors…) | per vendor | **never a `libfjarr` dependency** — each ships as its own GStreamer plugin package `fjarr-gst-<vendor>` per architecture ([ADR-0020](adr/0020-vendor-sources-as-gstreamer-plugins.md)) | per vendor (checked per package) | optional, separate packages |
| **Forbidden**: `gstreamer1.0-plugins-ugly` (x264enc) | — | — | GPL | **never** (doctor-enforced) |

## Setup tool (`fjarr-setup`) — ships in `fjarr-agent`

The installer commands `fjarr-agent` hands over
([docs/26](26-robot-install-and-drivers.md#the-setup-tool)): a Rust binary at
`/usr/lib/fjarr/fjarr-setup`, no GStreamer, nothing of it in the agent's process.

| Dependency | Version | Purpose | License | Ships |
|---|---|---|---|---|
| cliclack | 0.5 | the prompts (every one has a flag); the one prompt library once `fjarr-connect` moves off dialoguer | MIT | yes |
| toml_edit | 0.22 | `/etc/fjarr/fjarr.toml` edited in place, comments and order kept — it is the customer's file | MIT/Apache-2.0 | yes |
| clap, anyhow, libc, serde, serde_json, tokio | as the operator client | the CLI, errors, `getpwnam`/`getuid`/`isatty`, the change record, the netlink runtime | MIT/Apache-2.0 | yes |
| `fjarr-netdev` (rtnetlink) | in-tree | the same device code `fjarr-connect` uses: the persistent tun, its address, the routing table for the range check | MIT (rtnetlink) | yes |

## Signaling (`fjarr-signaling` / `fjarr-server`) — ships as sidecar/Cloud

| Dependency | Version | Purpose | License | Ships |
|---|---|---|---|---|
| Rust toolchain | 1.98.0 (pinned; bumped from 1.89.0 in slice 4.5e because webrtc-rs 0.21 uses `Ipv4Addr::from_octets`, stabilised after 1.89) | build | MIT/Apache-2.0 | build-only |
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

## Operator client (`fjarr-connect`) — ships in `fjarr-tools` (planned, M4.5)

Versions are pinned when the binary is implemented; these rows exist so the
licence question is settled before the code is written
([ADR-0024](adr/0024-native-operator-client.md)). The **robot** side of the
tunnel adds no dependency at all — a TUN device is the kernel plus two
ioctls.

| Dependency | Version | Purpose | License | Ships |
|---|---|---|---|---|
| webrtc-rs | pinned at implementation | peer connection + data channels, no media | MIT/Apache-2.0 | yes |
| tokio | 1.x | async runtime | MIT | yes |
| libc | 0.2 | the virtual interface, with no wrapper crate: `TUNSETIFF` plus one unbuffered read and write per packet is three calls, and a TUN crate would bring its own I/O model to sit beside tokio's. Interface, address and route setup shells out to `ip`, which is on every host that has a TUN device (slice 4.5e; the macOS `utun` path is the same three calls against a different socket family) | MIT/Apache-2.0 | yes |
| rtnetlink (with netlink-packet-core / -route / -utils, netlink-proto, netlink-sys, nix) | 0.23 | the interface's address, MTU and per-robot routes, **in process**. A file capability is not inherited by a child, so shelling out to `ip` cannot work under the `setcap` install docs/27 promises — and speaking netlink here also means the binary needs no `iproute2` on the host (slice 4.5e) | MIT | yes |
| clap | 4.x | the CLI surface | MIT/Apache-2.0 | yes |
| getrandom | 0.3 | the 128-bit `state` of the loopback login (docs/27#logging-in), from the OS on every platform; it replaced a Linux-only `libc::getrandom` so the binary builds where the [shell](27-network-tunnel.md#shell) runs. Already in the tree under `rand` | MIT/Apache-2.0 | yes |
| windows-sys | 0.61 | Windows only: the console modes and size behind `fjarr-connect shell`'s raw mode (docs/27#shell); on Unix that is termios through `libc`. Already in the tree under tokio | MIT/Apache-2.0 | yes |
| dialoguer | 0.11 | the robot picker — type to filter, enter to connect (docs/27#what-it-feels-like) — rather than a hand-rolled terminal UI (slice 4.5f). To be replaced by cliclack, the setup tool's prompt library, once its filter mode is confirmed to serve the picker (docs/26#the-setup-tool) | MIT | yes |
| toml, dirs | 0.8, 6 | `~/.config/fjarr/config.toml` and the 0600 credential cache beside it (slice 4.5f) | MIT/Apache-2.0 | yes |
| reqwest (rustls) | 0.12 | the operator API calls — robots, grants, login codes (docs/09#operator-api); already in the tree for the server's webhooks | MIT/Apache-2.0 | yes |
| `fjarr-protocol` (this repo) | workspace | shared signaling message types — the reason wire drift is a compile error. Its own crate since slice 4.5e, so an operator client links the types without the server's axum/hyper/HMAC stack (ADR-0024) | AGPL-3.0 | yes |
| reqwest (rustls) | 0.12 | webhook delivery | MIT/Apache-2.0 | yes |
| tokio-tungstenite | 0.24 | the operator's signaling socket (`fjarr-connect`, slice 4.5e); also the e2e test WS client | MIT | **yes** (it was dev-only until `fjarr-connect` shipped one) |
| futures-util | 0.3 | stream/sink combinators on that socket | MIT/Apache-2.0 | yes |
| anyhow | 1.x | error context in the operator client, where a failure is a message to a developer rather than a value to match on | MIT/Apache-2.0 | yes |
| tracing, tracing-subscriber | 0.1 / 0.3 | the operator client's logging | MIT | yes |

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
| @xterm/xterm + @xterm/addon-fit | ^5.5 / ^0.10 | the terminal renderer in `<TerminalView>` (docs/06 `fjarr.terminal`) | MIT | yes (`@fjarr/react/terminal` entry with **optional** peer dependencies, imported only when the component mounts — a dashboard with no terminal pays nothing) |
| d3-graphviz (+ its `d3-selection`, `d3-transition` peers) + @hpcc-js/wasm | ^5 (^3) / ^2 | DOT rendering in `<PipelineGraph>` and the on-robot viewer (docs/24) | BSD-3 (ISC) / Apache-2.0 | yes (`@fjarr/react/pipelines` entry with optional peer dependencies; the viewer app's static files served from `introspect.viewer_dir`, docs/24) |
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
| openssh-client | 26.04 | the operator end of the tunnel lab: `make tunnel-ssh` / `tunnel-scp` run through it over `fjarr0` (docs/27 gate, slice 4.5c). BSD-2/ISC, dev image only — nothing in a shipped artifact links or invokes it |
| openssh-server (`fjarr-robot-services` container) | 26.04 | the ssh daemon the tunnel gate reaches, in a sidecar on the robot's network namespace. BSD-2/ISC, dev-only fixture: on a real robot this is the integrator's own package and the agent never knows about it |
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
