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
| **apt repository** (`deb [arch=amd64,arm64] https://apt.fjarr.io …`, components `stable` and `testing`) | the packages [below](#packages) | robots on Ubuntu 26.04 LTS (the supported platform, docs/04, ADR-0022) |
| **Container images** | `ghcr.io/fjarrio/fjarr-agent:<ver>` (core) and per-vendor variants `…:<ver>-zed`, `…:<ver>-realsense`, plus `-desktop-x11`/`-wayland`; **built from the same `.deb`s**, multi-arch, cosign-signed with an SBOM. The core image exists from slice 5b (`docker/agent/Dockerfile`, built and smoke-tested in CI, amd64, unpublished — [docs/12](12-development-environment.md#running-the-demo-robot-from-the-agent-image)); the variants, arm64 and publishing are this milestone | containerized robot stacks ([below](#containerized-robots)), the demo, CI |
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

**Pixi (conda-forge/RoboStack), Nix and Yocto** follow customer by customer
([ADR-0031](adr/0031-distribution-apt-and-containers-first.md)). Each renders
the same [system profile](#the-system-profile) its own way (a conda package, a
NixOS module, a `meta-fjarr` layer), and each takes on its own GStreamer
version as a supported, CI-tested row in docs/04.

### Packages {#packages}

**Packages install; `setup` switches on; `--check` verifies.** Nothing
invasive happens as a side effect of `apt install`.

| Package | Installs | Switched on by |
|---|---|---|
| `libfjarr-dev` | the **static** library, headers and the CMake package (`find_package(fjarr)`) | the customer, for embedding |
| `fjarr-agent` | the daemon, linked statically against libfjarr; `fjarr-agent.service`; the `fjarr` user (sysusers) in `video` and `render`; `/run/fjarr` and `/var/lib/fjarr` (tmpfiles); `/usr/share/fjarr/fjarr.toml.example`; the viewer, the driver catalog and the system profile under `/usr/share/fjarr/`; the boot unit that creates the tunnel device before the robot's software starts, inert unless `fjarr.net` is configured | the package enables the service, which starts only once `/etc/fjarr/fjarr.toml` exists (`ConditionPathExists`); `setup` writes that file |
| `fjarr-desktop-wayland` | the Wayland module; `fjarr-desktop-session` and its **user** unit ([ADR-0028](adr/0028-desktop-session-helper.md)); the `fjarr-desktop` group; the GDM watchdog unit; the fake-monitor EDIDs for headless robots | `setup desktop` |
| `fjarr-desktop-x11` | the X11 module; the kiosk session's `xhost +si:localuser:fjarr` grant as an autostart entry; the output-layout helper and its RandR listener | `setup desktop` |
| `fjarr-tools` | `fjarr-connect` (given `cap_net_admin` at install) | nothing |
| `fjarr-gst-<vendor>` | camera drivers, per vendor and architecture | `drivers install` (M3, per design partner) |

`fjarr-inputd` is not packaged: none of the chosen desktop backends needs it
(ADR-0006).

Three choices were corrected when the packaging was built (2026-09-28):

- **No shared `libfjarr` until the ABI is stable (M6).** A C++ shared library
  would change its SONAME, and so its package name, with every 0.x release.
  Embedders link the static library from `libfjarr-dev`, and so does
  `fjarr-agent`.
- **`/etc/fjarr/fjarr.toml` is written by `setup`, not shipped as a
  conffile.** An enabled service with a placeholder config would restart in a
  loop. The unit waits for the file instead.
- **`fjarr-opsim` is not shipped.** It is a lab tool whose receive side
  decodes with libav, which is dev and CI only (docs/14). Probing a source is
  `fjarr-agent --probe-source`.

### The system profile {#the-system-profile}

A data file in the repository, installed as
`/usr/share/fjarr/profile.toml`. Per feature (`core`, `net`,
`desktop-wayland`, `desktop-x11`, `headless`), it lists the users, groups,
units, udev rules, kernel arguments and GDM settings that feature needs. It is
used three ways. The package scripts and `setup` apply it where the OS allows.
`--check` verifies it on every robot, however it was installed, and names each
missing piece. Later channels (a NixOS module, a Yocto layer) render the same
data declaratively.

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
| `fjarr-agent --check` | the doctor: every configured source and backend with element availability, plus the one-line fix for each missing one (`install: sudo fjarr-agent drivers install realsense`). **Always a `desktop` row** (ADR-0021, slice 2c): available with the backend serving it, or unavailable naming the package to install — and never a failure, because a robot with no desktop is the normal case |
| `fjarr-agent --probe-source …` | bring up one source standalone ([docs/09](09-interfaces.md#the-video-source-contract)) |
| `fjarr-agent net setup` (M4.5) | creates the persistent tunnel interface owned by the `fjarr` user, derives the robot's address, checks the configured range against existing routes, writes the systemd ordering so the agent attaches before the robot's software, and offers to write the Cyclone DDS configuration file ([docs/27](27-network-tunnel.md)) |

| `fjarr-agent setup desktop` | makes a desktop robot reachable unattended ([ADR-0006](adr/0006-desktop-backend-selection.md)): chooses or creates the auto-login account (no password, no remote login), enables auto-login, the GDM watchdog and the session helper's user unit; for X11 kiosks the output layout; for headless robots, on request, a forced connector with an EDID on the kernel command line |
| `fjarr-agent setup --undo <feature>` | reverses every change `setup` recorded for that feature (GDM, GRUB, accounts, units) |
| `fjarr-agent --check --health` | the container health check: the same profile verification, exit status only |

`setup` never guesses silently: every proposed track and every install
is shown and confirmed (`--yes` for provisioning scripts), and the result
is a plain `fjarr.toml` the customer can read and edit. It applies system
changes only where the OS allows it (Ubuntu with apt). On a system whose
configuration is generated or read-only, it prints the options to set
instead. It never replaces the ecosystem's package manager. Every change it
makes is recorded, so `--undo` can reverse it.

The tunnel is the one piece of setup that **must** run before the robot's
own software starts, and the one that needs root. The interface is created
once, here; afterwards the agent attaches to it unprivileged, and upgrading
the agent does not disturb anything bound to it
([docs/27](27-network-tunnel.md#lifecycle)). `fjarr-agent --check` reports
the interface, its address, whether the agent is attached, and whether the
configured range overlaps a route that already exists on the machine.

## Containerized robots {#containerized-robots}

A reference compose file ships with each release and states what the agent
needs from the host:

| Need | In compose | Why |
|---|---|---|
| WebRTC | `network_mode: host` | bridged networking hides the robot's addresses from ICE and adds a NAT hop |
| Encoder, cameras | `devices: /dev/dri, /dev/video*`; `group_add` with the host's render GID | group ids differ per host |
| Config and device key | volumes for `/etc/fjarr` and `/var/lib/fjarr` (`0700`) | the key survives image upgrades |
| Tunnel | `cap_add: NET_ADMIN`, `/dev/net/tun`, host networking | the agent creates the persistent `fjarr0` in the host's namespace; ROS containers `depends_on` the agent being healthy, which is the ordering rule ([docs/27](27-network-tunnel.md#lifecycle)) in compose |
| Desktop | the host installs `fjarr-desktop-session` from the `.deb`; its socket `/run/fjarr/desktop.sock` is bind-mounted into the container | the helper has to run inside the desktop user's session; descriptors cross a bind-mounted socket unchanged |

The image runs as `fjarr` with a **fixed, published uid**. The helper checks
the agent's uid (ADR-0028), so user-namespace remapping (rootless Docker,
`userns-remap`) is either off or its mapped uid is configured. On an X11
kiosk, `/tmp/.X11-unix` is bind-mounted, and the package creates a host account
with the image's uid so the `xhost` grant can name it.

## Repository, versions and releases {#releases}

- **The repository** is generated in CI by `reprepro`/`aptly` from debhelper
  packages built inside Ubuntu 26.04 containers on native amd64 and arm64
  runners (`dpkg-shlibdeps` derives the dependencies). It is signed with a
  Fjarr key held in CI secrets and served from Cloudflare R2 at
  `apt.fjarr.io`. The key's custody and yearly rotation are documented beside
  the release runbook, and the install script carries its fingerprint.
- **One version** for every artifact, from one tag `vX.Y.Z`: packages, images,
  `fjarr-connect`, the `fjarr-server` image, crates and npm packages. The wire
  protocol keeps its own major ([docs/08](08-protocol.md#versioning)). 0.x
  until the extension API is stable (M6).
- **A release** is: tag → full CI → packages and images to `testing` → a
  manual, protected promotion to `stable` that copies the same artifacts and
  never rebuilds them, then publishes the crates and npm packages. Release
  notes come from the conventional commits since the last tag.
- **Building locally**: `make deb` builds the packages for the host's
  architecture in a throwaway builder container (`docker/deb-builder`) into
  `dist/deb/<arch>/`. `make deb-install-test` installs them on a clean Ubuntu
  26.04 and checks every promise in the table above
  (`packaging/install-test.sh`). CI runs both on native amd64 and arm64 runners.
- **The install script** (`get.fjarr.io`) only does what apt cannot do by
  itself. It detects Ubuntu 26.04 and the architecture, adds the key and the
  repository, installs `fjarr-agent` and runs `setup`. On anything else it
  refuses, naming the supported systems and the container route. `--dry-run`
  prints every step without doing it.

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
the repository, the packages and their units, the system profile, the images
and the reference compose file, the install script, the release pipeline,
`setup` (with `setup desktop` and `--undo`), `drivers` and the catalog with
the built-in entries ([ADR-0031](adr/0031-distribution-apt-and-containers-first.md)). The first
vendor packages are M3, chosen by the design partner's hardware. Slice 3
already ships the runtime half: `unavailable` with reason, `--check`,
`--probe-source`, `/sources`.
