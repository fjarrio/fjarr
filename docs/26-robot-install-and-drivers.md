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

A data file in the repository (`packaging/profile.toml`), installed as
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

`packaging/catalog.toml` in the repository, installed as
`/usr/share/fjarr/catalog.toml` (also published at `https://fjarr.io/drivers/`
later, and versioned with the agent), is the single source the tools read.
**Shipped 2026-09-28 with the built-in entries only**: `test`, `v4l2` and
`rtsp`, each `builtin = true` with `source` naming its `type` in `fjarr.toml`
and `element` what the doctor checks. Vendor entries and their packages are
M3, chosen by the design partner's hardware; the desktop entries come with the
desktop packages (M3). The shape, with the vendor entries as they will look:

```toml
[v4l2]
title   = "V4L2 cameras (USB webcams, UVC, CSI through the kernel)"
builtin = true
source  = "v4l2"
element = "v4l2src"
matches = [{ api = "v4l2" }]                                 # gst-device-monitor's device.api

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
on this machine" instead of failing an install. A `usb` rule is a `vvvv:pppp`
id with an optional trailing `*`; it beats an `api` rule for the same device
(a RealSense is a v4l2 device too, and the vendor entry is the one that brings
its depth stream). The tool reads the file's order and nothing else: adding a
vendor is an entry plus a plugin package.

## The commands

| Command | Does |
|---|---|
| `fjarr-agent setup` | interactive first run: enrolls (or takes a dev token), detects the display server, lists attached devices (`gst-device-monitor-1.0` for v4l2/PipeWire plus udev ids matched against the catalog), proposes `fjarr.toml` with one track per detected camera, installs the matching packages (`apt`, with the repository already configured), prints the manual steps for anything behind a EULA, and ends with `--check` |
| `fjarr-agent drivers list` | the catalog, with per-entry status on *this* machine: installed / available / not for this architecture / needs manual step; `--json` for agents |
| `fjarr-agent drivers install <name>` | installs one entry and its prerequisites, prints the post-install steps |
| `fjarr-agent drivers detect` | hardware currently attached, matched against the catalog, with the package each needs |
| `fjarr-agent --check` | the doctor: every configured source and backend with element availability, plus the one-line fix for each missing one (`install: sudo fjarr-agent drivers install realsense`). **Always a `desktop` row** (ADR-0021, slice 2c): available with the backend serving it, or unavailable naming the package to install — and never a failure, because a robot with no desktop is the normal case |
| `fjarr-agent --probe-source …` | bring up one source standalone ([docs/09](09-interfaces.md#the-video-source-contract)) |
| `fjarr-agent net setup` | the tunnel's one-time setup: address, `fjarr-net.service` recreating the device at every boot, optional ROS ordering and DDS file ([below](#fjarr-agent-net-setup), [docs/27](27-network-tunnel.md)) |

| `fjarr-agent setup desktop` | makes a desktop robot reachable unattended ([ADR-0006](adr/0006-desktop-backend-selection.md)): chooses or creates the auto-login account (no password, no remote login), enables auto-login, the GDM watchdog and the session helper's user unit; for X11 kiosks the output layout; for headless robots, on request, a forced connector with an EDID on the kernel command line |
| `fjarr-agent setup --undo [<feature>]` | reverses every change `setup` recorded for that feature (`setup`, `net`; later `desktop`: GDM, GRUB, accounts, units); bare, every feature, newest change first |

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

## The setup tool {#the-setup-tool}

Decided 2026-09-28. `setup`, `setup desktop`, `net setup` and `drivers` are one
small **Rust** program, `fjarr-setup`, shipped in the `fjarr-agent` package at
`/usr/lib/fjarr/fjarr-setup`. `fjarr-agent` hands those subcommands over to it
unchanged, so the commands above keep their names. Why a separate tool:

- **It is installer UX and system configuration, not device runtime.** An
  embedder of libfjarr never needs it, so it does not grow the C++ daemon.
- **It reuses tested code.** The boot-time tunnel device is created over
  netlink by the same module `fjarr-connect` uses for its own persistent device
  (docs/27).
- **One prompt library across the operator's machine and the device:**
  [cliclack](https://crates.io/crates/cliclack). `fjarr-connect` moves from
  dialoguer to cliclack in the same slice, after its filter mode is confirmed
  to serve the device picker as well as dialoguer's fuzzy select does. The
  repository then carries one prompt library (docs/14).

Rules for every command:

- **Say "device", not "robot".** The tool is for any connected machine. (The
  configuration and protocol field `robot_id` keeps its name; renaming it
  would be a protocol change, taken separately if ever.)
- **Every prompt has a flag** (`--yes`, `--server`, `--device-id`,
  `--ros-units`, `--dds none|cyclone|fastdds`, …), so a fleet provisions with
  a script and the prompts are only the friendly face.
- **Every change is recorded** in `/var/lib/fjarr/setup-changes.json`, and
  `fjarr-agent setup --undo <feature>` reverses exactly those changes.
- **System changes are applied only on Ubuntu with apt.** Elsewhere the tool
  prints the options to set. It never replaces the ecosystem's package manager.
- **Each command ends with `fjarr-agent --check`**, whose profile rows verify
  what it did.

**Built 2026-09-28:** `net setup`, `net up` and `setup --undo net`
(`signaling/crates/fjarr-setup`), shipped in `fjarr-agent` with
`fjarr-net.service`, and proven on the spike machine; then, the same day,
`setup` (the first run, [below](#fjarr-agent-setup)), `drivers list|detect|install`
with the built-in catalog ([below](#fjarr-agent-drivers)) and the bare
`setup --undo`, proven on the spike machine against the lab server and in the
package install test. `setup desktop` says so and exits non-zero until M3.
`fjarr-connect` still prompts with dialoguer; its move to cliclack is owed
(docs/14).

### `fjarr-agent setup`

The first run, which the install script calls. It detects the machine (OS,
architecture, hardware H.264 encode, display server, cameras via
`gst-device-monitor-1.0` matched against the catalog), then asks:

```text
┌  fjarr setup
│
◇  This device: Ubuntu 26.04 · amd64 · VA-API H.264 ✔ · GNOME on Wayland
│
◆  Where does it connect?
│  ● Your own fjarr-server   ○ Fjarr Cloud
◇  Server: wss://fleet.acme.com/ws
◇  Device id: dev-024 (from the hostname)
◆  Device token (from your dashboard):  ••••••••
◇  Token accepted by the server ✔  stored in /etc/fjarr/fjarr.toml (0640, root:fjarr)
│
◆  Cameras found. Which should stream?
│  ◼ Front  Logitech C920 (usb-046d_C920…)   mjpeg 1280×720@30
│  ◻ Depth  Intel RealSense D435  → needs the realsense driver
◆  Install the realsense driver? (adds Intel's apt repository)   Yes
│
◆  Also set up:
│  ◼ Terminal       → as which account?  operator
│  ◼ Tunnel         → runs `net setup` next
│  ◻ Remote desktop → runs `setup desktop` next
│
◇  /etc/fjarr/fjarr.toml written · fjarr-agent started · online ✔
└  fjarr-agent --check: all rows ok · undo: fjarr-agent setup --undo
```

Until M5 builds enrollment ([docs/17](17-roadmap.md), [docs/10](10-security.md)),
`setup` asks for the **device token** the server accepts today and writes it
to the configuration. The prompt becomes a one-time enrollment token, redeemed
for the per-device key, when M5 lands. The flow around it does not change.
The "Fjarr Cloud" choice joins the server prompt with M7; today the prompt is
the server URL. Installing a vendor driver from this flow is M3 with the first
vendor entry: a detected camera that needs one is shown with its entry and left
unchecked.

What it does, in order, as built 2026-09-28 (`fjarr-setup`'s `setup.rs`):

1. **Detects**: OS and architecture (os-release), hardware H.264 encode (the
   line `fjarr-agent --check` prints, so the encoder the agent will probe is
   the one reported), the graphical session (`loginctl`), and the cameras
   (`gst-device-monitor-1.0 Video/Source`, matched against the
   [catalog](#the-driver-catalog) by udev usb id and `device.api`).
2. **Asks** the server URL, the device id (default: the hostname, or the
   existing config's), and the device token (a password prompt; an existing
   token is offered to keep). Before anything is written it opens a TCP
   connection to the server's host and port: a typo or a closed port fails
   here, with nothing changed, and `--offline` skips the check for a device
   provisioned before its uplink exists (offline is a normal state,
   [ADR-0019](adr/0019-agent-process-model.md) addendum; a first run that
   cannot be checked is not a finished setup, so it is said, not assumed).
3. **Decides the encoder** with the agent's answer: `auto` with hardware
   encode; otherwise `software`, confirmed, because the agent has no silent
   fallback and would refuse to start ([docs/23](23-agent-core-architecture.md)).
4. **Proposes one track per camera** that a built-in source serves, pre-selected;
   the id is a slug of the camera's name (unique among the file's tracks), the
   label its name, the source `{ type = "v4l2", device = <by-id name> }` with
   the mode picked as MJPEG 1280×720@30 when the camera has it, else the
   largest MJPEG mode up to 1080p at ≥ 15 fps, else the largest raw mode
   ([docs/06](06-capabilities.md#fjarrcamera--camera-video-m1-reference-implementation)'s format; editable).
5. **Asks the terminal's account**, with no default ([docs/06](06-capabilities.md#fjarrterminal--remote-terminal-m2)):
   the agent's own account works now; another account is written as chosen,
   with the warning that the terminal reports `unavailable` until the agent
   runs as it. `--yes` without `--terminal` configures none.
6. **Writes `/etc/fjarr/fjarr.toml`**, `0640 root:fjarr`: created plain when
   absent, edited in place when present (comments, order and the customer's
   other keys kept; a track with the same id is replaced). The whole file is
   recorded, with its previous contents beside the record, so `--undo` puts
   back exactly what was there.
7. **Starts the agent** (`systemctl restart fjarr-agent.service`; enabled by
   the package, enabled here if it was not) and waits for its `STATUS=online`
   ([ADR-0019](adr/0019-agent-process-model.md), second addendum; `--timeout`,
   30 s) — the server's acceptance of the token is the agent's own word, not a
   second handshake in the tool. A failed or restarted unit ends the wait with
   the journal's reason; a timeout says the configuration stays and the agent
   keeps trying. Without systemd (a container) the agent is not started and
   the tool says how to run it.
8. **Ends with `--check`**, or hands over to `net setup` when the tunnel was
   chosen (`--net yes`, with `--ros`, `--ros-units`, `--dds` passed through).

The flags, one per prompt: `--yes`, `--server`, `--device-id`, `--token` (or
`FJARR_DEVICE_TOKEN`, for scripts that keep it off the command line),
`--offline`, `--encoder auto|vaapi|software`, `--cameras all|none|<device,…>`
(by-id names or `/dev` paths, as `drivers detect` lists them), `--terminal
none|<account>`, `--net yes|no`, `--timeout`. What it leaves on the device,
recorded for `--undo`:

| Where | What |
|---|---|
| `/etc/fjarr/fjarr.toml` | `agent.robot_id`, `agent.server_url`, `agent.dev_token`, `media.encoder`, one `capabilities."fjarr.camera".tracks.<id>` per chosen camera, `capabilities."fjarr.terminal"` when an account was chosen; `0640 root:fjarr` |
| `/var/lib/fjarr` | created `0700 fjarr` when the service has not run yet |
| `fjarr-agent.service` | started; recorded as enabled only when the package had not already enabled it, as started only when it was not running |
| `/var/lib/fjarr/setup-changes.json` | the record, feature `setup`; the file's previous contents under `setup-backups/` |

The package install test (`packaging/install-test.sh`) runs the scripted form
on a clean Ubuntu 26.04 without systemd and a server: the first run fails
before writing anything and names `--offline`; `--offline` writes the file the
agent's `--check` then passes on; `--undo` removes it and the check names
`setup` again.

### `fjarr-agent net setup`

The tunnel's one hard rule is that its device exists **before** any software
that should use it starts (docs/27#lifecycle), because DDS picks its interfaces
when a participant is created. `net setup` derives or takes the address, checks
the range against existing routes, enables `fjarr.net` in the config, and
installs `fjarr-net.service`: a oneshot that recreates the device **at every
boot** (owned by `fjarr`, at the chunk MTU, before `fjarr-agent`), because a
tun device does not survive a reboot.

ROS and DDS are an **explicit question, never detected**. ROS often runs in a
container, invisible from the host, so detection would answer "no" on exactly
the machines that need the ordering. "No" skips the ordering and DDS steps
(`--ros no`); "yes" asks which units start it and which DDS it uses:

```text
┌  fjarr net setup
│
◇  Device id: dev-024 → tunnel address 100.70.118.224 (derived)
◇  Range 100.64.0.0/10 is free on this machine ✔
│
◆  Does software on this machine use ROS 2 / DDS over the tunnel?
│  ● Yes   ○ No
│
◆  Which services start it? They must start after the tunnel.
│  ◼ bringup.service
│  ◼ docker.service   (ROS in containers: they start with the runtime)
│
◆  Which DDS?
│  ● Cyclone DDS (writes /etc/fjarr/cyclonedds.xml, LAN interface enp3s0)
│  ○ Fast DDS (no file needed)
│  ○ None
│
◇  fjarr-net.service installed · 1 ordering drop-in · config updated
└  fjarr-agent --check: all net rows ok · undo: fjarr-agent setup --undo net
```

The ordering is a systemd drop-in per chosen unit — `After=` and `Wants=` of
both `fjarr-net.service` (the device exists) and `fjarr-agent.service` (the
agent has attached it, which is when it has carrier: the condition a Fast DDS
participant needs, docs/27#lifecycle) — which is the ordering rule applied to
the customer's own services. The wait is bounded by the robot, not the WAN:
the agent's `READY` comes once its capabilities are configured and its loop
runs, before any server is reached
([ADR-0019](adr/0019-agent-process-model.md), second addendum).
When ROS runs in containers, the unit to order is the container runtime
(`docker.service`) or the unit that starts the compose stack, and the containers
need host networking to see `fjarr0` (as in
[containerized devices](#containerized-robots)).

What it leaves on the device, each recorded for `--undo net`:

| Where | What |
|---|---|
| `/etc/fjarr/fjarr.toml` | `capabilities."fjarr.net".enabled = true`, and `address` when `--address` pinned it; only those keys, comments and order kept |
| `fjarr0` | created now, owned by the agent's account (the profile's `[net] owner`), `<address> peer 100.64.0.1`, at the chunk MTU |
| `fjarr-net.service` | enabled and started; at every boot it runs `fjarr-setup net up`, which reads the config, asks `fjarr-agent --net-address` and recreates the same device (a no-op when it is already there) |
| `/etc/systemd/system/<unit>.d/50-fjarr-net.conf` | the ordering drop-in, one per `--ros-units` entry: after `fjarr-net.service` and `fjarr-agent.service` |
| `/etc/fjarr/cyclonedds.xml` | with `--dds cyclone`: [docs/27](27-network-tunnel.md#ros2)'s file with the tunnel and the LAN interface (the default route's, or `--lan-interface`); the ROS 2 processes take it through `CYCLONEDDS_URI` |
| `/var/lib/fjarr/setup-changes.json` | the record, with a replaced file's previous contents beside it under `setup-backups/` |

The flags: `--yes`, `--ros yes|no` (required without a terminal: it is never
detected), `--ros-units a.service,b.service`, `--dds cyclone|fastdds|none`,
`--address`, `--lan-interface`. A running agent is restarted at the end so it
attaches now; a stopped one is left alone. It ends with `fjarr-agent --check`,
whose `net` rows verify the unit and the device.

Before 2026-09-28 the drop-in ordered after the device alone, because
`READY` waited for the first `hello-ack` and ordering after the agent would
have held a robot's bringup on the WAN; that was [#32](18-open-questions.md),
closed by the ADR-0019 addendum above.

### `fjarr-agent setup desktop`

**Built in M3**, with the desktop packages (moved from M2.5 on 2026-09-28): it
configures the session helper and backend M3 builds. The appliance pieces the
M2 spikes showed a desktop device needs
([ADR-0006](adr/0006-desktop-backend-selection.md),
[ADR-0028](adr/0028-desktop-session-helper.md)):

```text
┌  fjarr setup desktop
│
◇  Session: GNOME 50 on Wayland under GDM → backend: mutter
◇  Monitors: 1 (DELL U2422H on HDMI-1)
│
◆  Which account should the desktop run as? It logs in by itself at boot.
│  ● Create "desktop" (no password, no remote login)   ○ Use existing: operator
│
◇  Auto-login enabled for desktop in GDM
◇  GDM watchdog enabled (restarts the login if the session dies)
◇  Session helper enabled for desktop · group fjarr-desktop
│
▲  desktop must log in once for its group to apply. Reboot now?   Yes / Later
└  Undo: fjarr-agent setup --undo desktop
```

On an X11 kiosk it installs the output-layout helper and the `xhost` grant
instead. With no monitor it offers a forced connector: it shows which connector
and resolution, edits the kernel command line, installs the EDID, and says a
reboot is needed.

### `fjarr-agent drivers`

Mostly output, for people and scripts alike (`--json`):

```text
$ fjarr-agent drivers list
  NAME        STATUS          PACKAGE              NOTE
  v4l2        built in        —                    source = { type = "v4l2", … } in fjarr.toml (docs/06)
  realsense   available       fjarr-gst-realsense  needs Intel's apt repository (added for you)
  zed         needs manual    fjarr-gst-zed        SDK behind a EULA: see the link
  jetson-csi  not for amd64   fjarr-gst-argus      available on arm64

$ fjarr-agent drivers detect
  /dev/video0      Logitech C920                    → v4l2 (built in)  mjpeg 1280×720@30
  usb 8086:0b07    Intel RealSense D435             → realsense (available)
```

The status per entry is `built in`, `installed` (dpkg's view of the package),
`available`, `not for <arch>` or `needs manual`, in that precedence; `--json`
gives `{ "arch", "catalog", "drivers": [ { name, title, status, package,
element, note, docs } ] }` for `list` and `{ "arch", "devices": [ { name,
path, api, usb_id, serial, modes, stable_name, driver, status } ] }` for
`detect`, where `stable_name` is the by-id name a `v4l2` source takes and
`modes` the caps the monitor reported. Neither needs root. `detect` without
`gst-device-monitor-1.0` is an error naming `gstreamer1.0-plugins-base-apps`,
which the `fjarr-agent` package depends on for exactly this.

`fjarr-agent drivers install v4l2` says the entry is built in and exits 0.
`sudo fjarr-agent drivers install realsense` will show the prerequisites, ask
before adding a vendor repository, install, reload udev, and finish with a
`--probe-source` of the device it found — with the first vendor entry, M3;
until then it says so and exits non-zero.

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

The image runs as `fjarr` with a **fixed, published uid: 10001**. The Dockerfile
creates the user before installing the package, and the package's sysusers
entry then only adds `video` and `render`; on an apt robot the uid is dynamic.
The image's health check is **live**: the running agent's introspection
endpoint answers. That says more about a running container than a static
check of its files (corrected 2026-09-28 from a `--check --health` that was
never built). The helper checks
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
  (`packaging/install-test.sh`). `make deb-embed-test` builds `demo-robot` out of
tree against the installed `libfjarr-dev` alone (`find_package(fjarr)`), which
is what a customer's CMake project does. CI runs all three on native amd64 and
arm64 runners.
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
`setup` (with `--undo`), `drivers` and the catalog with
the built-in entries; `setup desktop` and the desktop packages are M3 ([ADR-0031](adr/0031-distribution-apt-and-containers-first.md)). The first
vendor packages are M3, chosen by the design partner's hardware. Slice 3
already ships the runtime half: `unavailable` with reason, `--check`,
`--probe-source`, `/sources`.
