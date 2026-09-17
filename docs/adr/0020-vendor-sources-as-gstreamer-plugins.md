---
title: "ADR 0020: Vendor camera support ships as separately packaged GStreamer plugins"
---

- **Status**: accepted — the sentence on desktop backends is superseded by
  [ADR-0021](0021-desktop-backends-as-runtime-modules.md) (they are runtime
  modules in separate packages, not linked into the core)
- **Date**: 2026-09-17

## Context

Fjarr will support many vendor-specific cameras (stereo/depth cameras with
SDKs, industrial GigE cameras, Jetson CSI), but any one customer uses a
small subset, and some SDKs exist only for some architectures (a ZED SDK
build for Jetson arm64 and not for a given amd64 NUC, for example). A
customer who wants remote desktop and a USB webcam must not have to
install — or even be able to install — a stereo-camera SDK for the agent
to start. The [video source contract](../09-interfaces.md#the-video-source-contract)
already keeps the core ignorant of what a source is; this decision is
about how vendor support is *built and distributed*.

## Options considered

- **Compile vendor sources into `libfjarr` behind build flags.** One
  binary per combination of vendors and architectures; a customer's
  package either drags every SDK in or is a bespoke build. Rejected.
- **A Fjarr-specific source plugin ABI** (`fjarr_source_plugin_init()`
  in `libfjarr-source-<vendor>.so`, loaded at runtime). Solves
  installation, but invents a second plugin system with its own ABI
  stability burden next to GStreamer's, before docs/05 promises any ABI
  at all (M6).
- **Vendor sources as GStreamer plugins, packaged separately.** A vendor
  integration is a GStreamer element (`zedsrc`, `realsensesrc`, …),
  either the vendor's own plugin or one we write (`libgstfjarr-<vendor>.so`),
  in its own package (`fjarr-gst-<vendor>`, built per architecture where
  the SDK exists). The core reaches it through tier 1 of the source
  contract — a description string in config — via GStreamer's registry
  at runtime. GStreamer provides the ABI, discovery, versioning and
  packaging conventions; Fjarr provides none of that.

## Decision

Vendor sources that Fjarr distributes are GStreamer plugins in separate
packages. `libfjarr` and `fjarr-agent` link GStreamer, libsoup, nlohmann
and toml++ only — never an SDK. Tier 2 of the source contract
(`register_source_type` in C++) exists for the *embedding application's
own* code, where the customer already controls linking; it is not how
Fjarr ships drivers.

Runtime behaviour follows from this:

- A configured track whose element is not installed is **`unavailable`
  with the reason** (`element "zedsrc" not found — install fjarr-gst-zed`)
  in the manifest label, the introspection endpoint and the doctor; the
  agent starts and every other track works. A track marked
  `required = true` in config makes the missing element a startup error
  (exit 1) instead.
- `fjarr-agent --check` and the doctor list every configured source with
  its element's availability and version; `--probe-source` exercises one.
- Packaging: `fjarr-agent` (core, all architectures) plus
  `fjarr-gst-<vendor>` per vendor, each declaring its architecture and SDK
  dependency; container images follow the same split (a base agent image
  and per-vendor layers). Desktop backends (X11, PipeWire/libei) stay
  inside `libfjarr` as dynamically linked, ubiquitous platform libraries
  ([ADR-0006](0006-desktop-backend-selection.md) chooses among them by
  config, not by package).

## Consequences

Fjarr writes GStreamer elements for vendors that lack one — more work per
vendor than a bespoke C++ class, but each is independently testable with
`gst-launch-1.0` and `--probe-source`, usable by customers outside Fjarr,
and packaged like every other GStreamer plugin. The core's dependency
list in [docs/14](../14-dependencies.md) stays short and
architecture-independent; a customer's install is exactly the drivers
they use.
