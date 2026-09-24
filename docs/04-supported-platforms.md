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
| `nvv4l2` (Jetson) | Planned, M2.6 | no board yet | `nvv4l2h264enc`, NVMM, arm64, L4T packages from NVIDIA |
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
| Unattended after reboot | straightforward | portal permission model is the hard part |
| Ubuntu 24.04 default | available | default session |

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
