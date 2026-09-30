---
title: Supported Platforms
description: OS, display server, GPU, browser, and network support matrices.
---

Fjarr is **opinionated**: a small, well-tested matrix beats a broad, flaky
one. Targets outside the primary column are adapters, not core concerns.

## Robot (agent) — operating system

| | Status | Notes |
|---|---|---|
| **Ubuntu 26.04 LTS x86-64 / arm64** | **Primary** ([ADR-0022](adr/0022-baseline-ubuntu-2604-gstreamer-128.md)) | GStreamer 1.28, libnice 0.1.23, libsoup 3.6, PipeWire, libei — everything the specs need from distro packages; systemd required for the packaged agent (ADR-0019) |
| Ubuntu 24.04 LTS | Embedders only, at their own risk | GStreamer 1.24 works as the offerer against browsers; a `webrtcbin` answerer (`fjarr-opsim`, loop tests) needs ≥ 1.26 for `reuse-source-pads` ([docs/23](23-agent-core-architecture.md#offer-construction-and-renegotiation)); not supported by the packaged agent |
| Ubuntu 22.04 | Not targeted | GStreamer 1.20 lacks `vah264enc`, unusable libei; upgrade the robot instead |
| Debian 13 / other distros | Untested, likely works | Same component versions; no CI |
| NVIDIA Jetson (Ubuntu-based, L4T) | Planned, M2.6 | arm64, NVIDIA's own GStreamer: the `nvv4l2` encoder family ([ADR-0025](adr/0025-encoder-families.md)). **Not** the nvcodec plugin a desktop card uses — different plugin, packages and memory type |

## Robot — GPU / encoder

Four families behind one adapter ([ADR-0025](adr/0025-encoder-families.md)).
**A family with no nightly runner is best effort and untested, never
supported** — a hardware path nobody exercises rots quietly and is then
found by a customer:

| Family (`media.encoder`) | Status | Nightly runner | Notes |
|---|---|---|---|
| **`vaapi`** (Intel iHD) | **Primary** | wanted | `vah264enc` via GStreamer `va`; Gen9+ iGPU incl. Meteor Lake NUCs; `LIBVA_DRIVER_NAME=iHD`; DMABuf straight into `vapostproc` |
| `nvcodec` (NVIDIA dGPU) | Planned, M2.6 | **yes** (`gpu-desktop`, RTX 2080 Ti) | `nvh264enc`, CUDA memory, x86-64; needs the container toolkit's `video` capability (`NVIDIA_DRIVER_CAPABILITIES`), and the CUDA runtime compiler for device-side convert/scale |
| `nvv4l2` (Jetson) | Planned, M2.6 | planned: a Jetson lab machine with a ZED camera ([docs/12](12-development-environment.md#lab-machines-and-fjarr-lab)) | `nvv4l2h264enc`, NVMM, arm64, L4T packages from NVIDIA |
| `software` | Explicit choice only | every runner | `openh264enc` for machines with no usable hardware family — never chosen silently ([docs/16](16-performance-budgets.md), [docs/23](23-agent-core-architecture.md)) |
| `x264enc` | **Forbidden in shipped artifacts** | — | GPL — [ADR-0011](adr/0011-license-open-core.md); doctor enforces absence |

`auto` probes `vaapi`, then `nvv4l2`, then `nvcodec`, and fails loudly when
none works. On a robot with an Intel iGPU **and** a discrete NVIDIA card the
iGPU wins on purpose: the dGPU is usually running perception, and taking its
encoder to stream a camera steals from the job the robot exists to do.

## Robot — display server (remote desktop capability)

Both X11 and Wayland are evaluated head-to-head before committing
([docs/07](07-desktop-backends.md), [ADR-0006](adr/0006-desktop-backend-selection.md)):

| Concern | X11 (Xorg) | Wayland |
|---|---|---|
| Capture | `ximagesrc` (+XDamage/XFixes) | PipeWire + ScreenCast portal |
| Input | XTest | libei / RemoteDesktop portal |
| Input (below compositor) | uinput | uinput |
| Unattended after reboot | in an auto-login kiosk session (the kiosk lays out its own outputs); reachable at LightDM's X11 login screen as root, never at GDM's | in an auto-login session via mutter's interfaces (E) with no consent step, or the portal (C) with a grant written at provisioning; GNOME's login screen is unreachable (ADR-0006) |
| Ubuntu 26.04 stock | **not available** — GNOME 50 is Wayland-only; X11 only via a kiosk (Xorg + a small window manager) or a non-GNOME desktop the robot chooses | the default and only GNOME session |
| No display attached | a forced connector (kernel `video=…e` plus an installer-supplied EDID) or a dummy plug | the same forced connector; or, for E only, a mutter virtual monitor created per session |

## Robot — optional platform packages

Desktop support is installed per display server: `fjarr-desktop-x11`
and `fjarr-desktop-wayland` are runtime modules loaded by the core when
configured ([ADR-0021](adr/0021-desktop-backends-as-runtime-modules.md));
a headless robot installs neither and carries no X11 or PipeWire
dependency.

## Robot — vendor camera packages

The agent core is architecture-independent; vendor camera support is a
separate GStreamer plugin package per vendor and per architecture
([ADR-0020](adr/0020-vendor-sources-as-gstreamer-plugins.md)), so a robot
installs only what its hardware needs. Availability is a property of the
vendor's SDK (a Jetson-only SDK yields an arm64-only package); the doctor
and `fjarr-agent --check` report each configured source's element as
present or missing.

## Operator — browser

| | Status | Notes |
|---|---|---|
| **Chromium-family ≥ 120** (Chrome, Edge) | **Primary** | H.264 + VP8 decode, full WebRTC feature set; Window Management API for automated multi-monitor fullscreen ([docs/22](22-remote-desktop-client.md#presentation-mode)), straight-to-fullscreen popups from ≥ 123 |
| Firefox ESR+ | Supported | verify H.264 availability in CI (platform-dependent); no Window Management API — multi-monitor fullscreen is the manual path |
| Safari 17+ | Best-effort | test at M3; known WebRTC quirks; no Window Management API |
| Mobile browsers | Not targeted for M≤6 | dashboard responsive layouts still apply |

## Operator — `fjarr-connect` (the network tunnel)

A native client, because a browser cannot create a network interface
([docs/27](27-network-tunnel.md), [ADR-0024](adr/0024-native-operator-client.md)).

| | Status | Notes |
|---|---|---|
| **Linux x86-64** | **Primary** | run in the lab against real robots every slice: interface, per-robot /32 routes, `ssh`, a hash-verified 1 GiB `scp`, `ros2 topic list`. Configures the interface over netlink in-process, so `setcap cap_net_admin+ep` is enough and no `iproute2` is needed |
| Linux arm64 | Expected to work, unverified | same code paths; the release builds and install-tests arm64 packages on hosted runners, and Raspberry Pi 4/5 and Jetson lab machines are planned ([docs/12](12-development-environment.md#lab-machines-and-fjarr-lab)) |
| **macOS (Apple silicon)** | **Written, type-checked, never run** | `utun` instead of `/dev/net/tun`, a 4-byte address-family header on every packet, `ifconfig`/`route` under `sudo` because macOS has no `setcap` equivalent. CI type-checks it for `aarch64-apple-darwin` on every change (`make connect-platform-check`); it does **not** link the binary — webrtc-rs pulls `ring`, whose build script needs the Apple SDK — and nothing has run on macOS hardware. Until it has, treat this row as an intention with a compiler behind it, not as support |
| Windows | Not targeted | WSL2 is the free answer; native Wintun is [question #24](18-open-questions.md) |
| **`fjarr-connect shell`, every platform** | **Linux primary; macOS and Windows type-checked, never run** | the [shell](27-network-tunnel.md#shell) needs no interface and no privilege, so it is not tied to a tunnel platform: raw mode is termios on Linux and macOS and the console API (with virtual-terminal input) on Windows, where the window size is polled because there is no `SIGWINCH`. `make connect-platform-check` type-checks that code for `aarch64-apple-darwin` and `x86_64-pc-windows-msvc`, the same way and with the same limit as the macOS row above: it does not link the binary, and nothing has run on either |

## Network requirements

| Path | Requirement |
|---|---|
| Signaling | outbound WSS (TCP 443-friendly) from robot and browser to `fjarr-server` |
| Media (best) | UDP outbound; STUN reachable |
| Media (fallback) | TURN over UDP 3478; TURNS/TCP 443 fallback planned M5 ([open question](18-open-questions.md)) |
| Assume | carrier-grade NAT on LTE robots ⇒ **relay-only is a normal case**, not an error ([docs/16](16-performance-budgets.md)) |

## Backend (`fjarr-server` sidecar)

Linux x86-64/arm64 container (distro-independent, `debian:bookworm-slim`
base). Customer backend stack: **any** — integration is HTTP/JSON per
[ADR-0015](adr/0015-backend-integration-strategy.md).

## Dashboard library

React ≥ 19 for `@fjarr/react`; `@fjarr/core` is framework-agnostic ES2022
(any bundler; Vite-tested). Node ≥ 22 for tooling.
