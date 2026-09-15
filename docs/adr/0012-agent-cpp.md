---
title: "ADR 0012: Agent in C++"
---

- **Status**: accepted
- **Date**: 2026-09-15

## Context

The agent lives inside GStreamer/GLib (C object model), touches VA-API,
X11/portals/libei, and must embed into customer robot stacks (frequently
C++/ROS 2).

## Options considered

**C++20** (native GStreamer/GLib fit; camera-streamer's production-proven idioms —
RAII wrappers, generation-counted contexts, single-loop marshaling — port
directly; first-class ROS 2 embedding) · Rust (memory safety, but
gstreamer-rs + the C callback boundary reintroduces unsafe seams, no
existing production experience in-house, and C++ customers embed it less
naturally) · Go (GC pauses + cgo friction in the media path).

## Decision

C++20 with the camera-streamer idiom set as required style (docs/09), GoogleTest,
clang-tidy/format enforced.

## Consequences

Memory safety is discipline + idiom + sanitizer CI (asan preset) rather
than language-guaranteed; revisit per-component Rust (e.g. `fjarr-inputd`)
if evidence warrants — that would be a new ADR.
