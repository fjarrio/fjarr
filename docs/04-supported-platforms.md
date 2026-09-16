---
title: Supported Platforms
description: OS, display server, GPU, browser, and network support matrices.
---

Fjarr is **opinionated**: a small, well-tested matrix beats a broad, flaky
one. Targets outside the primary column are adapters, not core concerns.

## Robot (agent) — operating system

| | Status | Notes |
|---|---|---|
| **Ubuntu 24.04 LTS x86-64** | **Primary** ([ADR-0002](adr/0002-ubuntu-2404-baseline.md)) | GStreamer 1.24, PipeWire 1.0, libei 1.2 — everything the specs need from distro packages |
| Ubuntu 22.04 | Not targeted | GStreamer 1.20 lacks `vah264enc`, unusable libei; upgrade the robot instead |
| Debian 13 / other distros | Untested, likely works | Same component versions; no CI |
| NVIDIA Jetson (Ubuntu-based) | Planned adapter | camera-streamer heritage: `nvv4l2h264enc` encoder adapter; post-M6, [open question](18-open-questions.md) |

## Robot — GPU / encoder

| | Status | Notes |
|---|---|---|
| **Intel VA-API (iHD)** | **Primary** | `vah264enc` via GStreamer `va`; Gen9+ iGPU incl. Meteor Lake NUCs; `LIBVA_DRIVER_NAME=iHD` |
| Software fallback | Dev only | `openh264enc`/`vp8enc` for machines without a GPU — never the product path ([docs/16](16-performance-budgets.md)) |
| `x264enc` | **Forbidden in shipped artifacts** | GPL — [ADR-0011](adr/0011-license-open-core.md); doctor enforces absence |
| NVIDIA (`nvh264enc`/Jetson) | Planned adapter | with the Jetson work |

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
