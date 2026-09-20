---
title: Robot Install & Drivers
description: How the agent is installed on a robot and how a customer discovers, installs and manages only the drivers their hardware needs — with the tool doing the work and telling them what it cannot.
---

> **Status: draft.** The customer-facing side of [ADR-0020](adr/0020-vendor-sources-as-gstreamer-plugins.md)
> and [ADR-0021](adr/0021-desktop-backends-as-runtime-modules.md): the
> core carries no driver; this document is how the *right* drivers get
> onto a robot without the customer becoming a packaging expert.

## Principles

1. **Install the core in one line, drivers only when a use case needs
   them.** A robot that streams a USB webcam and a remote desktop never
   sees a stereo-camera SDK; a robot with a depth camera gets exactly
   that vendor's package and its prerequisites.
2. **The tool knows the catalog; the customer states the use case.**
   Fjarr ships a machine-readable driver catalog; `fjarr-agent setup`
   detects what is attached, proposes a config, installs what it can, and
   prints precisely what it cannot (vendor EULAs, kernel modules, reboot).
3. **Missing is a state, not a crash.** A configured track whose driver
   is absent is `unavailable` with the package to install — in the
   doctor, the manifest label, the introspection endpoint and the
   dashboard — and everything else runs.
4. **The same answer everywhere.** Robot-side `--check`, the dashboard's
   robot page and (later) the fleet view all render the same source
   status from the same data.

## Distribution channels

| Channel | Contents | For |
|---|---|---|
| **apt repository** (`deb [arch=amd64,arm64] https://apt.fjarr.io …`) | `fjarr-agent` (core + `fjarr.test` + introspection), `fjarr-desktop-x11`, `fjarr-desktop-wayland`, `fjarr-inputd`, `fjarr-gst-<vendor>` per vendor and architecture, `fjarr-tools` (opsim, probe) | robots on Ubuntu 26.04 LTS (the supported platform, docs/04, ADR-0022) |
| **Container images** | `ghcr.io/fjarrio/fjarr-agent:<ver>` (core) and per-vendor variants `…:<ver>-zed`, `…:<ver>-realsense`, plus `-desktop-x11`/`-wayland`; the same packages, layered. The core image exists from slice 5b (`docker/agent/Dockerfile`, built and smoke-tested in CI, amd64, unpublished — [docs/12](12-development-environment.md#running-the-demo-robot-from-the-agent-image-slice-5b)); the variants, arm64 and publishing are this milestone | containerized robot stacks, the demo, CI |
| **Embedding** | `libfjarr` as a CMake package (`find_package(fjarr)`), headers = docs/09; the customer's app links the core and installs the driver packages it wants | robot companies embedding the library in their own daemon |
| **Install script** | `curl -fsSL https://get.fjarr.io \| sh` — adds the repository, installs `fjarr-agent`, runs `fjarr-agent setup` | first contact |

The `fjarr-agent` package installs the introspection viewer's static
files under `/usr/share/fjarr/viewer` and points `introspect.viewer_dir`
there in its shipped `fjarr.toml` ([docs/24](24-pipeline-introspection.md#the-viewer));
the container image carries the same directory.
The `fjarr-agent` package ships the systemd unit (`Type=notify`,
`WatchdogSec=30`, `Restart=on-failure`, running as the unprivileged
`fjarr` user per docs/10) and enables it; the daemon requires systemd on
robots ([ADR-0019](adr/0019-agent-process-model.md)).

Every package declares its architecture and its vendor prerequisites
(`Depends`/`Recommends`) so `apt` does the dependency work where the
vendor publishes packages; where a vendor SDK is an installer behind a
EULA (some stereo-camera SDKs, CUDA), the package declares a *virtual*
prerequisite and the catalog carries the human steps.

## The driver catalog

`share/fjarr/drivers.toml` (also published at `https://fjarr.io/drivers/`
and versioned with the agent) is the single source the tools read:

```toml
[realsense]
title       = "Intel RealSense (D4xx, L5xx)"
package     = "fjarr-gst-realsense"
element     = "realsensesrc"
arch        = ["amd64", "arm64"]
matches     = [{ usb = "8086:0b*" }, { usb = "8086:0a*" }]   # udev-visible ids
prereqs     = ["librealsense2 (from Intel's apt repo, added by the package)", "udev rules: 99-realsense-libusb.rules (installed by the package)"]
post_install = "replug the camera or reboot for the udev rules to apply"
docs        = "https://fjarr.io/docs/drivers/realsense"

[zed]
title       = "Stereolabs ZED (2, 2i, X, Mini)"
package     = "fjarr-gst-zed"
element     = "zedsrc"
arch        = ["arm64"]                    # SDK availability decides
requires_manual = "ZED SDK ≥ 4.x from stereolabs.com (EULA; CUDA on Jetson)"
matches     = [{ usb = "2b03:*" }]
docs        = "https://fjarr.io/docs/drivers/zed"

[desktop-x11]
title    = "Remote desktop on X11 (Xorg)"
package  = "fjarr-desktop-x11"
matches  = [{ display = "x11" }]

[desktop-wayland]
title    = "Remote desktop on Wayland (PipeWire + portal)"
package  = "fjarr-desktop-wayland"
matches  = [{ display = "wayland" }]
```

The `matches` rules are what `setup` uses to *detect* hardware; `element`
is what the doctor checks; `arch` is why the tool can say "not available
on this machine" instead of failing an install.

## The commands

| Command | Does |
|---|---|
| `fjarr-agent setup` | interactive first run: enrolls (or takes a dev token), detects the display server, lists attached devices (`gst-device-monitor-1.0` for v4l2/PipeWire plus udev ids matched against the catalog), proposes `fjarr.toml` with one track per detected camera, installs the matching packages (`apt`, with the repository already configured), prints the manual steps for anything behind a EULA, and ends with `--check` |
| `fjarr-agent drivers list` | the catalog, with per-entry status on *this* machine: installed / available / not for this architecture / needs manual step; `--json` for agents |
| `fjarr-agent drivers install <name>` | installs one entry and its prerequisites, prints the post-install steps |
| `fjarr-agent drivers detect` | hardware currently attached, matched against the catalog, with the package each needs |
| `fjarr-agent --check` | the doctor: every configured source and backend with element availability, plus the one-line fix for each missing one (`install: sudo fjarr-agent drivers install realsense`) |
| `fjarr-agent --probe-source …` | bring up one source standalone ([docs/09](09-interfaces.md#the-video-source-contract)) |

`setup` never guesses silently: every proposed track and every install
is shown and confirmed (`--yes` for provisioning scripts), and the result
is a plain `fjarr.toml` the customer can read and edit.

## In the dashboard and the fleet view

The introspection endpoint's `/sources` ([docs/24](24-pipeline-introspection.md))
carries the same status per source (`available`, `missing: fjarr-gst-realsense`,
`not-for-arch`, `needs-manual: ZED SDK`), so the demo dashboard's robot
page shows a "driver missing" badge with the install command instead of a
black tile, and Fjarr Cloud (M7) shows it fleet-wide — "3 robots configured
for RealSense without the driver" — and, once OTA campaigns exist (M8),
can push a driver package to a robot group like any other update.

## What stays honest

- Fjarr does not redistribute SDKs it is not licensed to redistribute;
  the catalog says so and links the vendor's download, and the tool
  verifies the SDK is present before installing the plugin package.
- A driver that fails after install (kernel module, permissions) shows up
  as a source error with the bus message, not as a generic media failure —
  the `--probe-source` output is the support ticket.
- The catalog is data: adding a vendor is a catalog entry plus a plugin
  package, never a change to `fjarr-agent`.

## Roadmap

Packaging is the **M2.5** milestone ([docs/17](17-roadmap.md#m25--packaging--install)):
the repository, the packages and their systemd unit, the install script,
`setup`, `drivers` and the catalog with the built-in entries. The first
vendor packages are M3, chosen by the design partner's hardware. Slice 3
already ships the runtime half: `unavailable` with reason, `--check`,
`--probe-source`, `/sources`.
