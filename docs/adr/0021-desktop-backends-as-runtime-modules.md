---
title: "ADR 0021: Desktop backends are in-tree runtime modules in separate packages"
---

- **Status**: accepted (supersedes the desktop-backend sentence of
  [ADR-0020](0020-vendor-sources-as-gstreamer-plugins.md))
- **Date**: 2026-09-17

## Context

[ADR-0020](0020-vendor-sources-as-gstreamer-plugins.md) keeps vendor
camera SDKs out of the core by shipping them as GStreamer plugins. Desktop
support is only half a capture problem: `ximagesrc` and `pipewiresrc` are
already GStreamer elements, but input injection (XTest, libei, uinput),
monitor enumeration and hot-plug (RandR, the portal), cursor metadata,
clipboard and the portal session for unattended access are *libraries*
(libX11/libXtst/libXrandr, libei, libportal/D-Bus, libevdev), not
elements. Linking them into `libfjarr` would make every robot — including
a headless one that only streams a USB camera — depend on X11 and
PipeWire packages: the same "install drivers you don't use" problem
ADR-0020 removes for cameras.

## Options considered

- **Link every backend into `libfjarr`** (ADR-0020's original wording).
  Simple; every install drags in X11, PipeWire, libei and their
  dependency trees whether or not a desktop exists.
- **Build-time feature flags.** Removes the dependency for whoever builds
  the binary, but a distributed package must still pick one set; a
  customer cannot add Wayland support to an installed agent.
- **In-tree backend modules loaded at runtime** — `libfjarr-desktop-x11.so`,
  `libfjarr-desktop-wayland.so` (and the privileged `fjarr-inputd` of
  [ADR-0009](0009-privilege-separation.md)) built from this repository,
  packaged as `fjarr-desktop-x11` / `fjarr-desktop-wayland`, discovered
  in `/usr/lib/fjarr/desktop/` and `dlopen`ed by the `fjarr.desktop`
  capability when configured. Each module exports one versioned C entry
  point returning a `DesktopBackend` factory. Because the modules are
  authored in-tree and released with the core, the entry point is an
  internal seam, not a public plugin ABI (docs/05 promises none before
  M6).

## Decision

Runtime modules in separate packages. `libfjarr` and `fjarr-agent` have no
X11, Wayland, PipeWire, libei or D-Bus dependency; capture inside a module
still goes through the GStreamer elements the platform ships
(`gstreamer1.0-x` for `ximagesrc`, `gstreamer1.0-pipewire`), which the
module's package depends on. The `fjarr.desktop` capability is always
present in the core; with no module installed, or none matching the
running display server, it reports `unavailable` with the reason and the
package to install, and every other capability runs. Backend selection is
by config (`[capabilities."fjarr.desktop"] backend = "auto" | "x11" |
"wayland"`), with `auto` probing the display server; which combination is
the default remains [ADR-0006](0006-desktop-backend-selection.md)'s
question for the M2 spikes.

## Consequences

Two more packages and a small module loader in the core. The same rule
now holds for every optional platform integration: the core is
architecture- and display-server-independent, and a robot installs only
what its hardware and its desktop need. The docs/14 rows for the X11,
libei, PipeWire and D-Bus libraries move from "ships in the core" to
"ships in the backend package".
