---
title: "ADR 0024: fjarr-connect — a native operator client, data channels only"
---

- **Status**: accepted
- **Date**: 2026-09-23

## Context

[ADR-0023](0023-network-tunnel-virtual-interface.md) puts a virtual network
interface at both ends of the `fjarr.net` link. A browser cannot create
one, so the operator end needs a native binary — Fjarr's first non-browser
peer. It runs on developer laptops: Linux certainly, macOS very likely.

It needs signaling, a peer connection, one data channel, and a TUN device.
It needs **no media at all**, which is what makes the choice interesting:
the constraint that forced GStreamer onto the robot does not apply here.

## Options considered

**Reuse `libfjarr` as a client mode.** One WebRTC implementation for the
whole project and no protocol drift by construction. But it drags GStreamer
onto every developer laptop for a tool that moves no media, and GStreamer on
macOS is a support burden out of all proportion to a packet pump. It also
grows a client mode inside a library whose entire design is robot-side.

**A TypeScript client on `@fjarr/core` with a native TUN addon.** Reuses the
client implementation that browsers already exercise, but needs a Node
WebRTC binding, and that ecosystem is not a foundation to build a shipped
tool on. A native addon per platform removes the one advantage.

**A Rust binary using data channels only.** A single static binary with no
runtime dependency, on a tier the project already staffs. TUN and utun are
handled by mature crates, so macOS costs little. It shares the signaling
message types with the `fjarr-signaling` crate, so wire drift is caught by
the compiler rather than by a test. The cost is a third WebRTC
implementation in the project.

## Decision

Ship **`fjarr-connect`**, a Rust binary in the `fjarr-tools` package, using
data channels only and no media.

It shares signaling types with `fjarr-signaling` rather than redeclaring
them. It requires `CAP_NET_ADMIN` to create its interface and add per-robot
routes, granted by `setcap` at install or by `sudo`, and no other privilege.

## Consequences

A third WebRTC implementation to keep current with
[docs/08](../08-protocol.md). The blast radius is small — it exercises
signaling, one unreliable unordered data channel and reconnection, not
media, tracks, renegotiation or repair — and the protocol conformance tests
of [docs/15](../15-testing-strategy.md) cover it as a third party.

The project gains a second operator platform matrix, including macOS, where
nothing else in Fjarr runs today.

`fjarr-connect` is the natural home for later developer-facing operator
features that are awkward in a browser. That is a reason to keep it small
now, not a licence to grow it: anything a browser can do belongs in
`@fjarr/core`.
