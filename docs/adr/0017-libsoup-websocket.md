---
title: "ADR 0017: libsoup-3 for agent networking"
---

- **Status**: accepted
- **Date**: 2026-09-15

## Context

The C++ agent needs a WebSocket (and occasional HTTP) client for signaling
(docs/08). The agent's concurrency model is a single GLib main loop shared
with GStreamer (docs/09 idioms) — a networking library that lives on that
loop avoids an entire class of thread-marshaling seams.

## Options considered

- **libsoup-3** — GNOME's HTTP/WS client. GLib-native: async results are
  delivered on the same `GMainContext` as GStreamer callbacks; LGPL-2.1
  dynamically linked (ADR-0011-clean); packaged in Ubuntu 24.04; the pairing
  used throughout upstream webrtcbin examples.
- libwebsockets — small MIT C library, embedded pedigree, but brings its own
  event loop that must be glued to GLib, plus manual HTTP plumbing.
- Boost.Beast/Asio — modern C++, but a large dependency running its own
  `io_context` thread with hand-written marshaling into the GLib loop; the
  most code for the same result.

## Decision

libsoup-3 (`libsoup-3.0-dev`), used strictly behind the `SignalingTransport`
seam (ADR-0013) so the choice stays swappable.

## Consequences

One new shipped dependency (docs/14 row added); reconnect/backoff logic
implemented by us per docs/08 (libsoup does not provide it); TLS via
glib-networking, present on the target platform.
