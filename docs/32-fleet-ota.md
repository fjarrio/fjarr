---
title: Fleet OTA
description: fjarr.ota — signed releases that update a whole robot (OS image, applications, configuration and peripherals) under one version, with readiness conditions, prepare actions, step-by-step run logs, budgeted delta downloads, human-promoted staged campaigns, and reference recipes for Yocto, NixOS and Ubuntu on Jetson and x86.
---

`fjarr.ota` updates robots in the field. A robot company publishes a
**release**: one version that pins everything the robot runs, from the OS
image to the GNSS receiver's configuration. Fjarr gets it onto the robots
safely, when the robot and its owner are ready, and shows every step as it
happens. This document is the design (M8); [docs/17](17-roadmap.md#m8--fleet-ota)
has the slices and [docs/06](06-capabilities.md#fjarrota--fleet-ota-updates-m8)
the acceptance criteria.

The design was brainstormed with Erik on 2026-10-12, after a survey of the
market (Mender, balena, Foundries.io, Torizon, Peridio, Ubuntu Core, AWS IoT
Jobs, Azure Device Update, Memfault, ATC Deploy, Viam, the update engines
SWUpdate, RAUC, OSTree and systemd-sysupdate, and the peripheral tooling) and
a critique of that survey. The decisions he made:

| Topic | Decision |
|---|---|
| What an update replaces | one release may carry a whole OS image, or only lighter components on top of the image already installed |
| Who decides when | the robot company configures it per fleet: robots install on their own within its rules, or the robot's owner chooses within them |
| Version identity | one fleet-wide release version; a robot always runs exactly one release |
| Rollout | stages chosen and promoted by a person, after watching the stage for days; parallel within a stage, batched so the server is not flooded; abortable at any time |
| Peripherals | a handler contract and a Fjarr-maintained library of handlers |
| Reference hardware | a Stereolabs ZED Box Mini (Jetson Orin NX/Nano, NVMe) and a second x86 mini-PC in the lab, reflashed nightly so the first keeps its M3 desktop tests |
| Operating systems | Yocto and NixOS from the start, side by side, on both machines; an Ubuntu A/B image on x86; plain Ubuntu with apt as a non-atomic path |
| Signing | keys held by the robot company in production; a Fjarr-managed key per tenant as an easy start |
| Transport | HTTPS, resumable, content-addressed chunks, under a budget per time interval so a download can spread over days |
| Progress | every update shown like a CI job: a heading per step, its outcome, its output streamed |
| Order | M8 before M7, reporting just enough (the release version and update outcomes) |
| Open core | the robot side open (AGPL); fleet campaigns, the chunk service and statistics paid |

Points first proposed in this document and decided on review are at the
end, under [Decided on review](#review-decisions).

## Releases {#releases}

A **release** is one signed manifest per release version (`2026.11.1`). It
lists every **component**, each with a `version` and a content hash:

| Kind | Examples | Applied by |
|---|---|---|
| `os` | a Yocto image, a NixOS system, an Ubuntu A/B image | the OS applier for the robot's recipe ([below](#appliers)) |
| `firmware` | the Jetson bootloader capsule | the OS applier, as a separate component; under review for unified Jetson A/B (8.0, E2, [below](#updater)) |
| `app` | container images, a Nix closure, a set of apt packages | the app applier |
| `config` | files under `/etc`, ROS parameters, maps shipped with the release | the config applier |
| `peripheral` | MCU firmware, a motor controller's firmware or parameters, u-blox settings, router configuration, a PLC program | a peripheral handler ([below](#peripherals)) |

A release that changes only components on top of the image carries the same
`os` component as the previous release: nothing of the image travels, and the
robot does not reboot. Every component declares:

- `hardware`: the variants it fits (`zedbox-mini/orin-nx16/2x-zedx`). A robot
  takes the components matching the variant it reports; one manifest serves
  every variant;
- `requires`: constraints on other components' versions, such as "motor
  firmware ≥ 3 requires `os` ≥ 2026.10", evaluated against the versions the
  robot will have **after** the release is installed, and checked before
  anything is fetched. They also give the order peripherals install in;
- `rollback`: `atomic` (it is in an A/B slot), `restorable` (its applier or
  handler can put the previous version back) or `forward-only` (the Jetson
  bootloader capsule, under review for unified Jetson A/B in 8.0, E2
  ([below](#updater)); most MCU flashes);
- `migrates_data`: the paths under `/data` it may change, which the updater
  snapshots before it runs ([below](#data));
- `safety_relevant`: true for safety functions (STO, a safety PLC,
  safety-rated drive firmware), which are not installed over the air unless a
  fleet's policy allows it ([below](#safety)).

A release is marked `security` when it fixes a vulnerability; such releases
follow the security channel's rules ([below](#security-channel)).

**One version, always.** A robot runs exactly one release at a time. It
reports the release version plus a **version vector** read back from the
hardware (the booted slot's image identity, each peripheral's firmware and
configuration hash), so a robot whose GNSS receiver was reconfigured by hand
shows up as deviating from its release instead of silently differing. Both
travel, signed, with the agent's registration on every connect (the field is
added to [docs/08](08-protocol.md#signaling) in slice 8.1). The history of
which robot ran which release, and when, is kept indefinitely; M7 keys every
metric and error on the same release version.

## Signing {#signing}

Whoever can sign a release can command every robot in a fleet, so signing is
designed as a safety boundary, not an upload detail. The model follows TUF
and Uptane:

- an **offline root key** signs the role keys and their rotation;
- **release keys** sign manifests: the robot company's, in its CI or on a
  hardware token, optionally requiring two signatures for production;
- **online keys** held by Fjarr Cloud sign only timestamps and targeting
  (which robots get which release, when). They cannot make a robot install
  anything the company did not sign: a compromised Fjarr Cloud can delay
  updates, never forge one.

Two custody modes. **Company-held keys** are the documented production setup.
**Fjarr-managed keys**, one set per tenant, are an easy start for
prototypes, labelled as such in the UI; switching to company-held keys is an
ordinary key rotation. The robot verifies every signature and every chunk's
hash before anything is installed. Its device identity lives on `/data` and
survives slot switches.

**No going back by accident.** The robot keeps a monotonic counter of the
highest release it has committed, on `/data`. It installs a lower release only
with a signed **downgrade authorization**: a targeting record from a
campaign, or, for an offline robot, one signed with a release key. This stops
anyone from reinstalling an old signed release with a known vulnerability,
from the network or a USB stick, while still allowing a deliberate downgrade.
A bundle installed from a local path is checked by its manifest signature,
its variant and the counter. Expired timestamp metadata leads to fetching
fresh metadata; a robot that cannot reach the server installs a local bundle
on those three checks alone, so robots without a clock or offline for months
still update.

## The robot side: `fjarr-update` {#updater}

The updater is a process of its own, beside the agent. The agent may be one
of the components it replaces and restarts into the new slot; the update
survives that. Its state machine, per release:

| Step | What happens | Waits on |
|---|---|---|
| 1. Fetch | the chunks the robot lacks, over HTTPS, into the local cache ([below](#transport)) | the fetch budget, free space |
| 2. Ready | the readiness conditions hold, and the owner's choice when the fleet's policy asks for one ([below](#readiness)) | conditions, consent |
| 3. Prepare | the customer's registered prepare action, confirmed by its predicate; the update hold begins | the action, its timeout |
| 4. Install | `os` to the inactive slot; `app` and `config` into the new slot for A/B recipes, or in place with a snapshot otherwise | nothing: conditions are re-checked first |
| 5. Switch | reboot into the new slot when `os` changed; otherwise restart the affected services | boot counting |
| 6. Health | the agent's reconnection and the health probes, within the confirmation window ([below](#health)) | probes |
| 7. Peripherals (restorable) | `restorable` peripheral components in `requires` order, each verified by read-back, then the probes again | handlers |
| 8. Commit | mark the slot good, raise the release counter, delete the snapshots | — |
| 9. Forward-only | `forward-only` components (the bootloader capsule, MCU flashes), last; under review for unified Jetson A/B (8.0, E2, below) | handlers, the applier |

On failure up to step 8, everything rolls back in reverse order: peripherals
through their handlers, apps and config with the slot or their snapshot, the
OS by booting the old slot. The **point of no return** is the commit (step 8),
or the first `forward-only` component if one must run earlier because of
`requires`; the run shows it, and abort and the owner's rescheduling stop
there. A `forward-only` component that fails after it does not roll the OS
back (the new firmware may require the new OS): the run ends **degraded**,
the robot is flagged, and the run shows the recovery path its recipe
documents.

**Under review for unified Jetson A/B (8.0, E2).** Step 9 and the
`firmware` component assume a bootloader updated apart from the OS. A
Jetson with NVIDIA's root filesystem A/B in its unified mode, which the
Yocto recipe uses, ties each bootloader chain to a root slot: NVIDIA
documents that chain A boots root slot A and chain B root slot B, so every
slot switch would also be a chain switch. That rule, and the model below
drawn from it and from edk2-nvidia's source, stay a hypothesis until D2 and
E2 confirm them on the ZED Box Mini
([the Jetson test image](#jetson-test-image)), and the rows above change
only then:

- on Yocto on Jetson, and on NixOS through path (i) of
  [Q10](#jetson-test-image), the capsule belongs to `os`: the root slot is
  written at install (step 4); the switch (step 5) stages the capsule on
  the ESP, and UEFI writes it to the inactive chain during the reboot,
  which switches chain and slot together. Rolling the slot back brings the
  old chain back with it, until the next update overwrites that chain. A
  capsule applied after commit, as step 9 does, would write the old slot's
  chain and boot the new bootloader on the old root. An update without a
  capsule boots whatever firmware the other chain holds, so the applier
  tracks each chain's version;
- L4TLauncher stays outside the pair: both chains load it from the one
  ESP, which neither the slot nor the capsule changes;
- what is one-way is the **version ratchet**: each successful capsule raises
  the lowest firmware version the box accepts, and only a reflash lowers it;
- `forward-only` stays for MCU flashes, and for capsules only where the
  bootloader is not tied to a slot: Ubuntu with apt, and NixOS booting
  through systemd-boot (path ii of [Q10](#jetson-test-image)).

Every step is idempotent and resumable: a power cut while fetching resumes,
while installing rewrites the inactive slot, and during the switch the
bootloader's boot counter falls back to the old slot. The run's state is a
versioned file on `/data`. The updater of release N must read the state the
updater of release N-1 wrote; if it cannot, health fails and the boot counter
falls back. The updater confirming that it runs is itself a health probe, so
a new slot whose updater crashes does not commit.

The updater, the manifest format, the appliers, the readiness conditions,
the local API and the handler contract are the open-source half of
`fjarr.ota`. They work on a single robot with a release bundle on a USB stick
or a local path, with no cloud at all; robots out of coverage for weeks
install the same signed releases, and their run logs upload when they
reconnect.

### The local API {#local-api}

The robot's own stack talks to the updater over a local socket,
`/run/fjarr/update.sock` (owned by the updater's account, group
`fjarr-update`, mode `0660`; the robot's own services join the group). It
is new in M8 and specified in full in slice 8.7; the robot side never needs
the agent for it. Over it the robot's stack:

- reports **robot facts** the built-in conditions read: battery level,
  charging, a mission running;
- places **holds**: "not now", with a reason, an expected end and a lifetime
  it must renew;
- registers **prepare actions** ("drive to the charger") with a confirmation
  predicate over the facts ("charging"), and **health probes** (an
  executable whose exit code decides, or a call on the socket);
- gives the **on-site confirmation** a `safety_relevant` component needs
  (from the robot's own HMI or a button);
- reads the **update hold** and the **not-ready-to-move** state below.

Prepare actions reuse the accept, feedback and result shape of
[docs/08](08-protocol.md#envelope) over this local transport, so a robot
company that already writes capabilities knows the pattern.

### Readiness and preparation {#readiness}

An update installs only when the robot is ready, and says why it is not.

- **Conditions** are evaluated on the robot, also offline: a time window in
  the site's local time zone, on the charger, battery above a level, no
  mission running, enough free space, a freeze period, no hold. The run shows
  the current reason ("waiting: not on charger since 14:02").
- **Prepare** runs the registered action through the local API; Fjarr never
  commands motion itself. It succeeds when its predicate holds within its
  timeout; otherwise the update waits for the next window.
- **The update hold.** From prepare until commit, the agent refuses
  teleoperation: the `motion` control domain gets a system holder, the
  update, that `take-control` cannot pre-empt (it is answered `control-held`
  naming the update run, [docs/10](10-security.md#session-ownership)). The
  updater places the hold through the agent; after a reboot the agent grants
  `motion` to no operator until the updater has said whether a run is in
  progress, and the hold ends if the updater dies. The hold blocks operator
  sessions only, not the robot's own stack, which may be driving to the
  charger. The deadman and `release_all_input` stay in force throughout.
- **Not ready to move.** Until the restorable peripherals have been verified
  and the probes pass after them, the updater publishes a not-ready-to-move
  state on the local API. The robot's own stack must honour it before it
  moves autonomously with new motor parameters or a new GNSS configuration;
  the recipes say so, and a test checks the state is visible.
- Conditions are re-checked before every step that cannot be undone.
- **Ceilings and deadlines.** A hold may last at most the ceiling the fleet's
  policy sets; an owner's choice must fall before the deadline the company
  set. When either runs out, the run proceeds as soon as the conditions hold,
  and the run log says so. What wins when rules meet: the on-site
  confirmation of a `safety_relevant` component, then the readiness
  conditions, then the security channel's ceiling, then the fleet's policy.

**Hooks** remain as a small escape hatch, mapped to the steps:
`pre-fetch` (before 1), `prepare` (after the prepare action's predicate
holds), `pre-install` (before 4), `post-install` (after 4), `health` (with
the probes in 6 and 7) and `pre-commit` (before 8). Each has a timeout, must
be idempotent, and declares an outcome: `ok`, `retry-later` with a reason, or
`fail`. In `health` and `pre-commit`, `retry-later` counts as a failure.

### Health and commit {#health}

A new slot is marked good only when the robot is healthy, not merely booted:

- **Reachability.** Before an update the updater records which of two
  endpoints the robot reaches: the signaling server, and the chunk store (a
  DNS lookup and a TCP connection). After the switch it must reach at least
  one it reached before. If it reached neither before, the local probes
  decide alone, so a cloud outage does not roll back a fleet, while an image
  that broke the robot's own networking does roll back.
- **Probes.** The registered health probes pass: cameras enumerate and
  stream, CAN is up and not in bus-off, the GNSS receiver has a fix, the
  robot's own self-test, the updater itself.
- **The confirmation window** is 10 minutes by default, configurable per
  fleet. If it ends without a pass, the updater boots the old slot. On Jetson
  the decision is wired into L4T's rootfs validation service, so NVIDIA's own
  boot-failure fallback respects it: on the Yocto image `fjarr-boot-verify`
  takes the place of meta-tegra's `nv_update_verifier`
  ([below](#jetson-test-image)).

### Persistent data {#data}

`/data` (configuration, maps, calibration, databases, the agent's identity,
the run state, the release counter) outlives slots. A component that migrates
data declares the paths it may change; the updater snapshots them before it
runs, and a rollback restores the snapshot with the slot. Without this, a
rollback boots an old version that cannot read data the new version migrated
forward, the most common way real rollbacks fail.

### Peripherals {#peripherals}

A peripheral component is applied by a **handler**: a separate program, in
any language, speaking JSON over stdin and stdout, with a fixed set of verbs:

| Verb | Contract |
|---|---|
| `identify` | the device, and its current version or configuration hash |
| `diff` | what would change; nothing to do is a valid answer |
| `prepare` | preconditions (motors off, the CAN bus quiet), with a timeout |
| `install` | write the new firmware or configuration |
| `verify` | read back and compare: mandatory, a write is not a success |
| `commit` | make it persistent (a GNSS receiver's RAM settings to flash) |
| `rollback` | restore the previous version, for `restorable` components |

A handler's output becomes its step's log ([below](#runs)). Fjarr ships three
reference handlers in M8, MIT-licensed so customers can fork them into their
own ([ADR-0038](adr/0038-reference-handlers-permissive.md)). They call tools
the customer installs, so no GPL flasher is bundled into Fjarr:

- **u-blox GNSS configuration**: diff the settings, write them to RAM, check
  the fix, then persist;
- **a CAN flasher**: CANopen or MCUboot over CAN;
- **router configuration** for OpenWrt and Teltonika, with commit-confirm.
  A configuration can cut the robot's only uplink, and a robot that has lost
  its link to the router cannot revert it, so the handler arms the revert
  **on the router** (OpenWrt's apply-with-rollback, its Teltonika
  equivalent), which the router carries out by itself, also across its
  reboot. The robot side only confirms, by cancelling the revert once the
  agent reaches Fjarr through the new configuration.

More handlers come from the driver catalog ([docs/26](26-robot-install-and-drivers.md))
and from customers.

### Safety {#safety}

- `safety_relevant` components are **not installed over the air** unless the
  fleet's policy explicitly allows it, and then only after the on-site
  confirmation. Changing a safety function can require a new conformity
  assessment under the EU Machinery Regulation (applying from January 2027),
  so by default a person on site does it.
- `forward-only` components document their physical recovery path in their
  recipe (the ZED Box Mini's USB recovery mode, a technician visit).
- **The ZED Box Mini's recovery path** is USB recovery mode (RCM) from a
  host on its recovery port, which then reflashes the box: a technician's
  laptop in the field, the lab's mini-PC in slice 8.0. Linux on the box
  enters RCM by software, from a slot or the recovery-partition rescue, and
  in the lab also from the network rescue
  ([the Jetson test image](#jetson-test-image)). The recovery button is
  needed only when no Linux comes up to do that: a bootloader chain that
  hangs before UEFI, or a kernel that hangs before userspace
  ([docs/18](18-open-questions.md) #48), neither of which a power cut
  cures, since the cold boot starts the same chain and slot again; and,
  without the lab's network rescue, firmware that finds nothing on the NVMe
  to boot.
- The update hold, the not-ready-to-move state and the refusal of
  `safety_relevant` components get regression tests like every other safety
  behaviour ([docs/15](15-testing-strategy.md#safety-behaviors)).

## Run logs {#runs}

Every update is a **run**, shown the way a CI job is:

- each step of the state machine, and each component within it, is a heading
  with a status (queued, running, succeeded, failed, skipped, rolled back,
  degraded) and a duration; fetching shows bytes against the budget, waiting
  shows the current reason;
- the stdout and stderr of whatever a step runs (the applier, hooks,
  handlers, prepare actions) is captured line by line with timestamps. Lines
  such as `::warning::` and `::error::` become annotations on the step;
- while someone watches, lines stream through the agent's connection to the
  server, at most 50 lines a second per run; beyond that, lines are counted
  and summarized. The robot keeps every run in a local journal on `/data`
  (16 MiB per run; when full, the oldest output lines go first and step
  headings and annotations are always kept), so what was logged offline or
  before a reboot uploads when it reconnects. The messages and their channel
  are specified in [docs/08](08-protocol.md) in slice 8.2;
- a campaign shows a grid of robots by steps, like a matrix build: a robot
  opens its run, a failed step jumps to its first error, and the robot's
  live view (camera, terminal, tunnel) is one click away, so an engineer can
  watch the first robot of a campaign update and step in if it gets stuck.

Runs are kept with each robot's update history, keyed by release.

## Transport {#transport}

Release content travels over **HTTPS** from object storage (R2 by default,
any S3-compatible store when self-hosted), never over the WebRTC session:
gigabytes relayed through TURN would cost whoever runs the relay, and plain
HTTPS from storage without egress fees is both cheaper and faster
([ADR-0037](adr/0037-release-content-over-https-chunks.md)). Fjarr's live
connection carries control, progress and run logs only.

- **Chunks.** Content is cut by content-defined chunking (casync-style, about
  64 KiB on average), each chunk named by its SHA-256 and compressed with
  zstd on its own. Images are chunked uncompressed, or nearly every chunk
  would change with every build. The robot keeps a chunk index of its active
  slot on `/data`, computed at install, and fetches only chunks neither the
  index nor its cache holds: that is the delta. A NixOS system's store paths
  are chunked the same way, so "only the changed store paths travel" is a
  consequence of chunking, not a second transport. The 64 KiB average is
  measured on real Ubuntu updates ([below](#measured-x86)); container and
  model updates are still to measure.
- Every chunk is verified against the signed manifest before it is stored.
  Fetching **resumes** at the chunk after any interruption.
- **A budget per time interval** caps what each robot fetches: "2 GB a day",
  or "20 GB a month, resetting on the 14th" to match a SIM's billing cycle.
  The interval follows the site's time zone, and a chunk in flight may overrun
  the budget by at most itself. A release can therefore download over several
  days. An operator may raise one robot's budget for one release; that is
  audited.
- **A rate cap** keeps a background fetch from starving a live session: by
  default the fetch uses at most half of the bandwidth the agent's rate
  control estimates for the robot's uplink ([docs/23](23-agent-core-architecture.md#rate-control-and-tier-switching)),
  and less while a video or teleoperation session is active.
- Fetching and installing are separate steps with separate conditions, so a
  release can be fetched days ahead and installed in the next window.
- Since the server knows every robot's installed release, it **predicts** the
  bytes each robot needs before a campaign starts, and when each will be
  ready at its budget.
- Space is checked before fetching; the cache is cleaned up after commit.

**Precomputed deltas** (Erik wants them explored, 2026-10-13). Chunks
reuse what is unchanged, but a small change spread through a large file (a
recompiled binary, a retrained model) can touch nearly every chunk. A binary
diff (xdelta, bsdiff) from one specific old version describes only the
changed bytes. Since the server knows every robot's installed release, it can
compute, for a new release, one diff per source release still present in
the fleet (usually two or three), and a robot fetches the diff matching its
release, falling back to chunks otherwise. Diffs are signed in the manifest
like chunks and reconstruct the same content-addressed result, so they change
nothing after the fetch. Slice 8.0 measures chunks against diffs on real
updates (a kernel, CUDA, an ML model); slice 8.3b builds them, scoped by
those numbers: file-by-file diffs for `app` components
([measured](#measured-apps)).

Later, not in M8: robots at one depot sharing chunks over their LAN. The
chunk format allows it without change: every chunk is verified against the
signed manifest whatever its source.

### Measured in 8.0: x86 Ubuntu {#measured-x86}

Measured 2026-10-14 on real week-apart updates of the reproducible
[x86 test image](#x86-test-image): a 16 GiB root slot with about 3 GB of
content, and its UKI. Pinned to snapshot.ubuntu.com 30
September and 7 October; between them, exactly two things changed in the
image's 187 packages, the kernel and OpenSSH. Bytes a robot fetches, at a
64 KiB average chunk (missing chunks at zstd 19, plus the compressed index
and 500 bytes per request); diffs by xdelta3, applied and compared before
they count (`spikes/ota-delta/results/`):

| Update | Full download | Chunks | xdelta3 |
|---|---|---|---|
| OpenSSH security update (3 packages), root slot | 897 MB | 2.25 MB (0.61 MB of it the index) | 0.12 MB |
| Kernel 7.0.0-34 to -38, root slot | 897 MB | 175 MB | 161 MB |
| Kernel, its UKI | 73 MB | 39.3 MB | 34.3 MB |
| The week's roll-up (both), root slot | 897 MB | 176 MB | 161 MB |
| The same release rebuilt without pinning, root slot | 897 MB | 82.7 MB | 0.46 MB |

What this settles, for the Ubuntu recipe so far:

- **64 KiB average chunks.** The total over these updates is flat between 32
  and 128 KiB (177 to 178 MB) and lowest at 64; smaller chunks pay more in
  index than they save. With the container, CUDA and model updates below
  added, 64 KiB is still the lowest (802 MB over twelve updates, against
  815 at 16 KiB and 840 at 256).
- **Reproducible builds are a precondition for chunks.** An unpinned rebuild
  of an unchanged release costs 83 MB in chunks, because filesystem metadata
  changes in scattered places and each one costs a whole chunk; a diff pays
  0.46 MB for the same noise. `fjarr bundle check` refuses such drift.
- **A kernel update is about a fifth of the slot, whichever way it travels.**
  Ubuntu compresses each kernel module separately, so changed modules
  share nothing with their old versions; a diff saves 8% over chunks. That
  is far from the threshold for building precomputed diffs (a diff at most
  half the chunks' size), and it holds for Ubuntu only: Yocto and NixOS can
  ship modules uncompressed.
- **Small updates are small either way.** The OpenSSH update fetches about 2
  MB in chunks against 0.12 MB by diff; the chunk index, one entry per chunk
  of the whole slot, is a third of that, and sending only the index's
  changes would remove it. 8.3's chunk server measures the real per-request
  cost.
- **Whole-image diffs need memory the robot may not have**, which is why
  only xdelta3 ran on the 16 GiB slot (bsdiff needs 17 times the old size,
  `zstd --patch-from` stops at 2 GiB). A file-by-file diff needs memory for
  one file at a time, and costs about what a whole-image diff costs (below).

### Measured in 8.0: applications, CUDA and models {#measured-apps}

Measured 2026-10-15/16 on pinned public updates
(`spikes/ota-delta/fetch-pairs.sh`): the official ROS 2 base container
rebuilt by its maintainers 16 days apart, an application container's patch
release (Caddy 2.11.6 to 2.11.7 on an unchanged Alpine base), CUDA 13.4.1 to
13.4.2 (the runtime and cuBLAS), and four ONNX models. The file-by-file
column diffs only changed files (the smallest of bsdiff, `zstd --patch-from`
and the file alone), pays once for content that appears under several names
and nothing for content the robot already holds, and includes the file list.

| Update | Full download | Chunks (64 KiB) | Best whole-file diff | File by file |
|---|---|---|---|---|
| ROS 2 container rebuild, every layer rebuilt | 344 MB as `docker pull`, 207 MB at zstd 19 | 69 MB | 8.8 MB | 6.7 MB |
| Application container, patch release | 21 MB as `docker pull` | 14.2 MB | 1.48 MB | 1.48 MB |
| The same application's binary alone (52 MB, recompiled Go) | 13.9 MB | 13.6 MB | 1.47 MB (bsdiff) | — |
| CUDA runtime and cuBLAS, 604 MB of libraries | 412 MB | 80.7 MB | 47.4 MB | 46.8 MB |
| A model fine-tuned, every weight changed (ViT, 347 MB) | 321 MB | 324 MB | 321 MB | — |
| A detector retrained, every weight changed (RT-DETR, 83 MB) | 75 MB | 76 MB | 75 MB | — |
| The same model exported again, weights unchanged (ResNet-50) | 95 MB | 0.12 MB | 0.002 MB | — |
| Only the classifier head retrained (ResNet-50, an 8 MB head) | 95 MB | 7.8 MB | 7.6 MB | — |
| OpenSSH update, root slot (U1 above) | 897 MB | 2.25 MB | 0.12 MB | 0.76 MB (0.74 of it the file list) |
| Kernel update, root slot (K1 above) | 897 MB | 175 MB | 161 MB | 172 MB |

What this settles:

- **Precomputed diffs pay off for applications, not for images or models.**
  A recompiled binary moves its code and addresses throughout the file, so
  chunks reuse almost nothing (13.6 of 13.9 MB) while bsdiff fetches a ninth
  of that. Containers rebuilt upstream without reproducible builds change
  every tar header, which costs chunks 69 MB and a diff 7 to 9 MB. CUDA
  saves 40%. Models gain nothing: where every weight changes, everything is
  fetched whichever way; where few change, chunks already fetch about what
  changed.
- **Slice 8.3b is therefore scoped to `app` components** (container
  images, binaries, packages), **as file-by-file diffs** from each release
  still in the fleet: they cost what whole-file diffs cost, need memory for
  one file at a time on the robot, and fall back to chunks for any file
  without a diff. OS slots stay with chunks and reproducible builds, where
  diffs saved 8% on a kernel update; so do models.
- **The file list dominates small updates.** A file-by-file update of the
  OpenSSH fix moves 18 KB of change and 741 KB of list; the list travels as
  a diff against the robot's own, which 8.3b measures.
- **Ubuntu 26.04's coreutils is one 11.8 MB binary under a hundred names.**
  Per-name accounting would have charged it a hundred times (246 MB instead
  of 6.7 MB for the ROS container); the transport pays per content hash, as
  chunks already do.

## Appliers {#appliers}

The OS-level work (install to the inactive slot, set the next boot, read the
slot state, mark good, roll back now) is behind a narrow **applier
adapter**, one per recipe ([ADR-0035](adr/0035-releases-orchestrated-appliers-narrow.md)).
Fjarr owns everything around it: fetching, conditions, ordering, health,
peripherals, reporting. The applier installs only from the local cache, never
streaming over the network. Which engine backs which recipe is settled by the
measurements of slice 8.0. The starting points are:

- **Yocto on Jetson:** SWUpdate on meta-tegra's A/B layout, run with no
  bootloader backend (`-B none`), as on x86, where the applier writes EFI
  Boot Guard's environments itself. The boot state stays NVIDIA's:
  L4TLauncher's retry counter and the boot-chain variables, read and
  written through `nvbootctrl` and the UEFI variables. Fjarr adds a
  trial-boot counter on `/data` and a boot-verify unit,
  `fjarr-boot-verify`, which replaces meta-tegra's `nv_update_verifier`
  ([the Jetson test image](#jetson-test-image));
- **NixOS:** the system's generations inside an A/B image layout; on the
  ZED Box Mini, 8.0 tries NVIDIA's root A/B with extlinux in each slot
  first, sharing the Yocto recipe's applier (Q10,
  [the Jetson test image](#jetson-test-image));
- **Ubuntu A/B on x86:** SWUpdate with EFI Boot Guard, the same engine as the
  Yocto recipe, chosen in 8.0 ([ADR-0040](adr/0040-x86-ubuntu-ab-swupdate-efibootguard.md),
  superseding systemd-sysupdate in [ADR-0039](adr/0039-x86-ubuntu-ab-sysupdate-systemd-boot.md));
  Yocto and NixOS on x86 are chosen in 8.5b;
- **plain Ubuntu:** apt, for the non-atomic path.

## The fleet side (paid) {#campaigns}

- A **campaign** sends one release to a group of robots in **stages**. The
  first stage is chosen by hand, often one robot. The campaign then waits,
  showing the stage's progress and outcomes, until someone **promotes** it
  with the next stage (10 robots, then the rest). Promotion is a human
  decision, taken after watching the stage for as long as the company wants;
  there is no automatic promotion or halt in M8.
- Within a stage, robots update in parallel. Their progress reaches the
  server through **batching** with backpressure, so a whole fleet finishing
  in the same night does not flood it.
- **Abort** stops every robot that has not passed its point of no return; the
  rest finish and report. Abort never downgrades a robot: going back is a
  separate campaign of an older release, with a downgrade authorization,
  where its components allow it (Jetson firmware cannot go backwards).
- A campaign can be written as reviewable YAML (stages, group, policy,
  windows) as well as built in the UI.
- When choosing a stage, its robots can be spread across hardware variants
  (an option, off by default).
- Before M7, a campaign page shows each stage against the rest of the fleet
  on what M8 itself knows: install success, rollbacks, reboots and
  disconnects. M7 adds the product's own metrics, keyed on the same release
  version.

**Who decides when** is the fleet's policy, set by the robot company, and a
campaign may set its own: `auto` installs within the conditions and informs
the robot's owner; `ask-end-user` lets the owner choose now, tonight in the
window, or a time, until the company's deadline. The owner sees the choice in the company's own app, with
`<UpdatePrompt>` from `@fjarr/react` (open source, like the rest of the web
library) or through a plain API, together with the live readiness reason, and
can change it until the point of no return. The paid part is the policy and
its enforcement in Fjarr Cloud, not the component.

### The security channel {#security-channel}

A release marked `security` installs automatically by default; the owner may
postpone it only up to a ceiling the company sets (for example 7 days); and
security fixes can ship separately from feature releases. This is the EU
Cyber Resilience Act's rule for security updates (applying from December
2027), built into the policy model rather than added later.

## Building releases {#building}

**`fjarr bundle`** is one tool whatever builds the OS:

- it takes the build's output (an image, a NixOS system closure, container
  images, configuration, peripheral firmware) and produces the release: the
  chunks and the signed manifest;
- it stamps the release version and build identity into the image
  (`os-release` `IMAGE_VERSION`, the Nix store hash), so what the robot
  reports is what was built;
- `fjarr bundle check` warns about non-reproducible builds that would make
  every delta large (timestamps, compression), and predicts each source
  release's download size;
- `fjarr bundle push` uploads only the chunks the store does not have yet.

A **board package** is one versioned unit per robot computer: the vendor BSP
pieces, device tree and overlays, pinmux, vendor kernel modules, and their
constraints ("the ZED Link driver needs exactly this kernel"). Every recipe
for that board consumes it, so the board-specific work is done once. A
release whose kernel does not match its drivers is refused when it is
bundled, not discovered on the robot.

**Recipes**, each built in CI and each stating what it guarantees:

| Recipe | Machine | OS and slots | Rollback | Delta |
|---|---|---|---|---|
| Yocto | ZED Box Mini | meta-tegra + meta-fjarr, A/B rootfs on NVMe; the bootloader chain (QSPI) expected to switch with the slot, its capsule with it (under review until E2, [above](#updater)) | A/B atomic; the QSPI bootloader expected to roll back with the slot, L4TLauncher in the shared ESP not (E2); the version ratchet one-way | chunks |
| NixOS | ZED Box Mini | jetpack-nixos flake + the Fjarr module, A/B image layout | A/B atomic | chunks of store paths |
| Yocto | second x86 mini-PC | the same layer, UEFI | A/B atomic | chunks |
| NixOS | second x86 mini-PC | the same flake, UEFI | A/B atomic | chunks of store paths |
| Ubuntu A/B | second x86 mini-PC | an Ubuntu 26.04 image on two slots, built by mkosi, written by SWUpdate and booted by EFI Boot Guard ([ADR-0040](adr/0040-x86-ubuntu-ab-swupdate-efibootguard.md)) | A/B atomic | chunks |
| Ubuntu + apt | any Ubuntu robot | today's robots, unchanged | per package, not atomic | apt's own |

The last row matters: most robots running Fjarr today are stock Ubuntu. They
get the same releases, conditions, consent and run logs, without atomic
rollback, and the recipe table says so.

**The x86 layout** (all three x86 recipes): a GPT disk with an EFI system
partition, two root slots and a `/data` partition; the boot state kept by
the loader chosen in 8.0 for each recipe (EFI Boot Guard for Ubuntu, one
environment and one UKI per slot, [ADR-0040](adr/0040-x86-ubuntu-ab-swupdate-efibootguard.md)); `/usr` read-only in the slot, `/etc` from the image with
machine-specific state on `/data`, and `/home` and `/var` on `/data`. The
accounts and groups the agent and the desktop helper need (ADR-0028) are
created by `sysusers.d` and `tmpfiles.d` in the image, so a desktop survives
slot switches. The Ubuntu image carries a GNOME desktop, as robots with a
desktop run today, so the M3 desktop tests can run on it too.

### The x86 test image in slice 8.0 {#x86-test-image}

Slice 8.0 tests one concrete layout on the lab's x86 mini-PC before any
recipe is built. The research behind it (2026-10-13) put **systemd-sysupdate
with systemd-boot** first and **RAUC with GRUB** second; the rehearsal and
the power cuts below chose the first
([ADR-0039](adr/0039-x86-ubuntu-ab-sysupdate-systemd-boot.md)). SWUpdate with
EFI Boot Guard, tested the same way afterwards, replaced it
([below](#x86-swupdate-ebg), [ADR-0040](adr/0040-x86-ubuntu-ab-swupdate-efibootguard.md));
the slots, rescue system, reflash and image findings here carry over.

- **Built with mkosi** (26, from Ubuntu 26.04's universe) from pinned
  inputs: a snapshot mirror, `SourceDateEpoch`, a fixed `Seed`. One build
  gives the disk image, each slot's root partition image and its unified
  kernel image (UKI).
- **The GPT layout:** an EFI system partition of 2 GiB; a second ESP-type
  partition `fjarr-rescue` of 1 GiB; two root slots of 16 GiB, labelled
  `fjarr_<version>` (an empty slot is `_empty`); `fjarr-data` for the rest.
- **Each UKI pins its own slot** with `root=PARTLABEL=fjarr_<version>`.
  Never gpt-auto or `root=dissect`: both pick the newest root partition, so
  booting the old UKI would mount the new root and defeat the rollback.
- **Boot counting** by systemd-boot, three tries, in the UKI's file name
  (`+LEFT-DONE`). `systemd-bless-boot` removes the counter once
  `boot-complete.target` is reached, and Fjarr's health check is ordered
  before it. Nothing sets a default entry (`loader.conf` `default`,
  `LoaderEntryDefault`, `bootctl set-default`): the sort order alone picks
  the newest healthy slot, then the rescue entry, then exhausted slots.
- **A rescue system:** one UKI with a small OS running from RAM (sshd with
  the lab keys, DHCP, the disk tools). It is both the installer that
  rewrites the disk and the permanent rescue, kept on the main ESP and as
  the removable-path loader of `fjarr-rescue`, which is the firmware's
  second boot entry.
- **Machine state on `/data`:** the SSH host keys, the DHCP client
  identifier (the MAC, so a router's reservation survives a new image) and
  the update cache. The root is mounted read-write in 8.0 (below).
- **A trial boot that cannot come up reboots:** `panic=10`, the hardware
  watchdog, and a deadline on `boot-complete.target`, so a failing slot uses
  up its tries instead of waiting in emergency mode.
- **The applier spike** wraps `systemd-sysupdate` (installing from a local
  directory, never a URL) in the [five verbs](#appliers). sysupdate has no
  verb to roll back now or to install without activating; the spike renames
  the UKI for both, and 8.0 measures how much such glue costs.

**Reflashing the lab machine.** The machine is rewritten over the network,
never by hand at its keyboard:

1. Boot the rescue UKI once, with the firmware's one-shot `BootNext`. If
   anything goes wrong, the next reset boots the old system.
2. Over SSH into the RAM system, write the image, grow the disk's last
   partition, copy the rescue UKI to both ESPs and create the firmware's two
   boot entries.
3. Check the entries and both ESPs before any reboot; then boot the on-disk
   rescue once, then slot A.

The first rewrite of a machine waits until it can be power-cycled remotely
or someone is near it; every image is rehearsed in QEMU with OVMF first.

**Rehearsed in QEMU (2026-10-14, `spikes/ota-x86`).** Every case below
passes from a fresh disk: an update installed but not activated, then tried
and blessed; rolling back now; a failed health check rolling back; a kernel
panic on the trial boot using up one try; a root partition that never
appears using up all three (about two minutes each); every slot exhausted
booting the rescue entry; the reflash through `BootNext`; and the firmware
falling through to `fjarr-rescue` when systemd-boot is missing. What the
rehearsal changed:

- **The root is read-write in 8.0.** Ubuntu keeps dpkg's database in `/var`,
  so a read-only root with `/var` on `/data` is its own design; the SSH host
  keys and the update cache are on `/data` already.
- **A missing root hangs unless told not to.** The initrd waits for the root
  device forever, and PID 1 keeps the watchdog fed. Each UKI carries
  `rootflags=x-systemd.device-timeout=90s`, and an extra initrd makes the
  initrd's emergency and rescue modes reboot.
- **Ubuntu deny-lists the watchdog drivers** (`iTCO_wdt`, `wdat_wdt` and
  others). The images remove those lines, udev loads the one the board has,
  and systemd drives it with a 30-second timeout. The lab NUC11's watchdog is
  the firmware's ACPI WDAT table (`wdat_wdt`, 2 to 613 s), not the chipset's
  TCO, and it resets the board: a kernel crashed with reboot-on-panic off
  came back 53 s later without the power switch (2026-10-14).
- **On the real NUC** (2026-10-14), the rescue UKI boots once through
  `BootNext` and the next reboot returns to the installed system, three times
  of three; the unclaimed rescue reboots itself after 15 minutes; a cut
  through the power switch brings the machine back in 33 s. Three
  `systemd-pcrlock-*` units fail on a machine with a TPM and are masked.
  Then `reflash.sh` rewrote the NUC to this layout (the write runs at about
  50 MB/s, 13 minutes for the 36 GiB image; `/data` grows to 198 GB), and
  the on-disk rescue and slot A both boot. On the hardware, install (22 s
  from a staged 0.9 GB root), set-next and bless, rollback-now and a failed
  health check all behave as in QEMU; a reboot takes about 23 s. The
  firmware adds its own entry for the ESP's removable path, after Fjarr's
  two, which only adds a fallback.
- **Power cuts mid-update pass on the NUC** (2026-10-15, 60 cuts through
  the Shelly plug at random moments, `spikes/ota-x86/powercut.sh`,
  `results/`). Twenty during the install, from the root write to the UKI
  renames: the NUC came back on 1.0 every time, the half-written 1.2 never
  activated. Twenty within two seconds after `set-next`: it came back trying
  1.2 every time, so the rename is on disk once `sync -f` returns. Twenty
  during 1.2's first boot, from the kernel to `systemd-bless-boot`'s
  rename: it came back on 1.2 and blessed it every time, or found it already
  blessed. Not once did it need the rescue system, a second power cycle or
  a hand, and after each group a clean update to 1.2 succeeded. `fsck` found
  no damage on either ESP; the main ESP reads as dirty because it is mounted
  read-write while the system runs. This meets 8.0's gate for the Ubuntu
  A/B image on x86: a power cut mid-install boots the old slot.
- **The ESP is mounted only on demand** (2026-10-15): `/efi` is an
  automount that unmounts after 30 seconds idle, so between updates the ESP
  is unmounted and `fsck` finds it fully clean. sysupdate, the applier and
  `systemd-bless-boot` mount it as they need it; every rehearsal case
  passes, and the NUC took the change as an ordinary A/B update (1.2 to 1.3,
  the 1.0 slot reused, 1.2 kept as the fallback).
- **The disk rewrite runs at 418 MiB/s, not 49:** `dd` reading a pipe gets
  64 KiB at a time, and with `oflag=direct` but no `iflag=fullblock` each
  read became its own direct write. The 36 GiB image now takes about a
  minute and a half instead of 13.
- **The NUC's TPM went into dictionary-attack lockout** (`0x921`) during
  the power cuts, so `systemd-tpm2-setup` fails; the images do not use the
  TPM and mask it. A robot whose identity key lives in a TPM would meet the
  same lockout after repeated power cuts ([#9](18-open-questions.md)).

- **sysupdate activates what it installs:** the new version sorts first at
  once. The applier renames the new UKI to `+0-0` (sorted last) until
  `set-next`; a power cut between the two renames boots the new slot early,
  with its three tries.
- **udev lags the relabel:** after sysupdate renames the slot's partition,
  the applier triggers a change event so `state` reads the new label.
- **Compressed sections defeat chunks.** A UKI's initrd is zstd-compressed,
  so two UKIs that differ in one script share few chunks (5.9 MB to fetch at
  a 64 KiB average, against 0.8 MB by xdelta3, of 73 MB for the whole UKI).
  The delta measurements weigh an uncompressed initrd.
- **Two builds of one release are byte-identical** (the 16 GiB root partition
  and the UKI), so a delta carries only real change. It took a pinned
  package snapshot, `SourceDateEpoch` and a fixed `Seed`, and three fixes
  found by comparing two builds: Ubuntu's `alternatives.log` and ldconfig's
  `aux-cache` are removed from the image, the image's own initrd archive is
  clamped to `SourceDateEpoch`, and the microcode archive is supplied by the
  build (mkosi 26 stamps its own with the build time and concatenates the
  files in directory order). `fjarr bundle check` looks for the same kinds of
  drift.

**Recipes, not images.** M8 publishes recipes and tooling only; whoever
builds an image from them distributes it to their own robots and owes what
that brings (the GPL's source offer, NVIDIA's and Stereolabs' licence terms).
Ready-made images are revisited when a customer asks (Erik, 2026-10-13;
[docs/18](18-open-questions.md) #43). The Yocto recipe has a "no GPLv3"
option (`INCOMPATIBLE_LICENSE = "GPL-3.0*"`) for robots locked with secure
boot that might count as consumer products under GPLv3; the recipes document
the question, which is the robot company's (#44). The ZED Box Mini's board
files are packaged openly only if Stereolabs' licences allow it, and
otherwise fetched from Stereolabs at build time (#45, settled in 8.0).

**Provisioning.** `fjarr provision` flashes both slots (an empty second slot
is a known trap on custom Jetson carriers), installs the device identity and
enrols the robot in its fleet, replacing the flash scripts every team writes
by hand.

### SWUpdate and EFI Boot Guard, compared {#x86-swupdate-ebg}

ADR-0039's first comparison covered RAUC on paper and dismissed SWUpdate in
a line. It was then built and tested the same way (2026-10-16,
`spikes/ota-x86/image-ebg`): the same slots, rescue system and reflash,
with EFI Boot Guard 0.22 (isar-cip-core's x86 loader) choosing the slot from
one environment per slot, and SWUpdate 2025.12 writing the slot and its UKI
from a signed package. Every QEMU case passed, and on the NUC the A/B cycle,
a failed health check, a crash on the trial boot, a hung trial kernel and 80
power cuts.

| | systemd-sysupdate + systemd-boot | SWUpdate + EFI Boot Guard |
|---|---|---|
| Tries per update | three (a counter in the UKI's name) | one: any reset during the trial (a crash, the watchdog, a power cut) rolls back |
| A crash on the trial boot | tried again; the second try came up | rolled back to the old version |
| A root that never appears | three tries, 391 s, then the old version | one try, 131 s |
| **A trial kernel that hangs before the watchdog driver loads** | **nothing resets the board**: it waits for a person or a power cut, three times over | **the loader armed the watchdog before the kernel**: reset and rollback by itself, 70 s in QEMU, 88 s on the NUC (its WDAT) |
| Power cuts on the NUC | 60 of 60: install, after set-next, trial | 80 of 80: the same plus the confirmation write; trial cuts before confirmation rolled back (12 of 20), after it kept the update (8) |
| Boot state | the UKI's file name on the ESP | two environments with a revision and a CRC; a damaged one falls back to the other |
| No bootable version at all | the rescue entry sorts after exhausted slots | the loader exits to the firmware, which boots the rescue partition |
| Install on the NUC | 22 s from a staged root | 24 s from a staged signed package |
| Applier glue | 45 lines, plus parking the new UKI | 50 lines, plus the confirmation units |
| Image | — | 53 MiB larger (SWUpdate pulls in Python, GnuPG, Lua, a web server; all unused and masked) |
| Ubuntu 26.04 | systemd 259 (main), mkosi (universe) | SWUpdate and EFI Boot Guard 0.19 in universe; 0.19's loader lacks a rollback fix, so the loader comes from 0.22 |
| Signing | the release signature only | Ubuntu's SWUpdate refuses unsigned packages: a second key, or a rebuilt SWUpdate |
| Licence | LGPL-2.1+ | GPL-2.0-only, `libebgenv` included: run as tools, never linked into Fjarr |
| Engine across recipes | x86 only | the same SWUpdate as the Yocto recipe on Jetson |

Findings on the way: EFI Boot Guard's own tools mount every FAT partition
on the disk read-write to look for environments, the rescue partition
included, so the applier reads and writes one environment partition at a
time; Ubuntu's kernel feeds a running watchdog itself until userspace opens
it, so `watchdog.handle_boot_enabled=0` is needed for the loader's
watchdog to cover the kernel's start; a raw slot write keeps the
filesystem's label but not the GPT's, so each UKI finds its root by
filesystem label; the confirmation cuts never landed inside the
environment write, which takes milliseconds, and the damaged-environment
case stands in for it.

**Decided (Erik, 2026-10-16): SWUpdate with EFI Boot Guard**, for one update
engine across the Jetson and x86 recipes
([ADR-0040](adr/0040-x86-ubuntu-ab-swupdate-efibootguard.md)). The early hang
supports it: a kernel update that hangs on one robot's hardware before
userspace is the failure an update system exists for, and only the loader's
watchdog recovers from it without a person. One try instead of three costs a
retried download when power fails in the first seconds of a new version.

### The Jetson test image in slice 8.0 {#jetson-test-image}

Slice 8.0 tests the Jetson recipes' layout on the lab's ZED Box Mini before
either recipe is built, as it did [on x86](#x86-test-image). The Jetson half
started on 2026-10-17, after research (2026-10-16/17) into meta-tegra,
tegra-demo-distro, jetpack-nixos and NVIDIA's edk2-nvidia source. What it
found about NVIDIA's A/B scheme is expected from that source and NVIDIA's
documentation, not measured; the phases below measure it. This section
states what the test image is and the rules it is tested under; the
procedures are in [`spikes/ota-jetson/README.md`](../spikes/ota-jetson/README.md),
and what the applier reads in NVIDIA's source, variable by variable, in
[`spikes/ota-jetson/applier/README.md`](../spikes/ota-jetson/applier/README.md).

**The lab box** is a Stereolabs ZED Box Mini with an Orin NX 16 GB module
and an NVMe disk, as delivered: L4T R36.4.4 with Stereolabs'
`stereolabs-zedbox-mini` 1.3.1 package, one root partition and A/B kernel
partitions. It is powered through its own Shelly plug (192.168.10.202), and
a USB cable from its recovery port to the lab's mini-PC, the M3 runner, lets
the mini-PC find it in USB recovery mode (RCM) and flash it. No serial
console is connected.

**Decided by Erik (2026-10-17)**, accepting the research's recommendations:

| # | Decision |
|---|---|
| Q1 | L4T **36.5.2** for both Jetson recipes: the release meta-tegra `scarthgap` and jetpack-nixos `master` are on, with Stereolabs' 1.4.4 driver package for it. Stereolabs publishes no flash script for 36.5.2, so its board package is Fjarr's port |
| Q2 | **Yocto first**; NixOS then reuses its board package and applier |
| Q3 | The factory image stays **restorable**: a full backup and one proven restore before the first Fjarr flash |
| Q4 | A **relay across the recovery button**, driven by the mini-PC, **or a person on site** for the gated steps: writes to the bootloader flash (QSPI), the first capsule, the hung-kernel and capsule-failure tests, and power cuts during a QSPI write (rule L4). Everything else runs unattended behind the network rescue and the dead-man |
| Q5 | `RootfsRetryCountMax` **3**, NVIDIA's default, plus the applier's **one-try counter** for trial boots. The value lives in the firmware and changes only by capsule or reflash |
| Q6 | The mini-PC **may be changed** for flashing and the network rescue (an NFS server, NVIDIA's udev rules, Docker, IPv6, dnsmasq for the box's MAC), each change by a script that can undo it, never 00:00–06:00 |
| Q7 | Yocto builds on Erik's workstation, with `rm_work`, and the shared-state cache on another disk if one is free |
| Q8 | **Early-hang protection is a gate for 8.4**, measured in 8.0 ([docs/18](18-open-questions.md) #48) |
| Q9 | The capsule test makes a **real version change**: a 36.4.4 build, then a capsule to 36.5.2 |
| Q10 | NixOS boots first through NVIDIA's root A/B with extlinux in each slot, sharing the applier (path i); systemd-boot with a UKI per slot (path ii) comes after |
| Q11 | An ADR on capsule signing and the version ratchet follows the capsule phase (E); until then the spike signs capsules with meta-tegra's test keys |

**The image**, built off the box in phase A:

- **Built with kas** from pinned layers: poky, meta-oe, meta-tegra
  `scarthgap` (L4T 36.5.2) and meta-swupdate. Its machine,
  `zedbox-mini-orin-nx16`, builds on meta-tegra's `p3509-a02-p3767-0000`,
  the board configuration Stereolabs' flash script uses, in the module's
  non-super power mode as Stereolabs ships it.
- **SWUpdate** without a bootloader backend, Lua or web server, accepting
  only a signed package that writes the slot partitions and never the ESP.
  meta-tegra's capsule recipe builds the capsule beside the image.
- **A trial that cannot come up resets warm**, the reset L4TLauncher
  counts: `panic=10`, `rootflags=x-systemd.device-timeout=90s`, an initrd
  whose emergency mode reboots and whose systemd opens the watchdog (30 s),
  and a deadline on `boot-complete.target`, as on x86.
- **The kernel** carries two upstream fixes to the Tegra watchdog driver
  (`timer-tegra186`), neither in NVIDIA's 36.5.2 tree: the self-ping fix
  (b42d781e0350) and a kernel watchdog armed at probe and kept alive from
  the driver's interrupt (46fb093b87a0). A second slot image keeps the
  unpatched kernel, so D2b measures both.
- `/boot/efi` is an automount, as `/efi` is on x86.

**The layout** is NVIDIA's NVMe A/B table (`flash_l4t_t234_nvme_rootfs_ab.xml`
in L4T's `tools/kernel_flash/`, byte-identical in L4T 36.4.4 and 36.5.2, and
the file meta-tegra uses for its redundant layout) with `/data` added. Phase
A1 read the sizes from it and, in edk2-nvidia's source, which partitions
each bootloader chain uses (2026-10-17); D2 confirms them on the box:

| Partitions | What they hold | Size |
|---|---|---|
| `APP`, `APP_b` | the two root slots: L4TLauncher takes `APP` on chain A and `APP_b` on chain B. Each carries its kernel, initrd and device tree in `/boot`, named by `/boot/extlinux/extlinux.conf`, which L4TLauncher reads, so they switch with the slot. Unlike the stock box's single `APP`, neither grows to fill the disk | 16 GiB each, set by the image as on x86 (NVIDIA's default is 27 GiB, meta-tegra's 27.5 GiB) |
| `A_kernel`, `A_kernel-dtb`, `B_kernel`, `B_kernel-dtb` | each chain's kernel and device tree, which L4TLauncher boots when the slot's extlinux configuration is missing or what it names cannot be loaded; written with the slot | 128 MiB and 768 KiB each |
| `recovery`, `recovery-dtb` | the recovery-partition rescue (below), one pair for both chains | 80 MiB, 512 KiB |
| `recovery_alt`, `recovery-dtb_alt` | unused: copies for NVIDIA's image-based OTA, which L4TLauncher never boots; empty after a flash | 80 MiB, 512 KiB |
| `esp` | the only partition of the EFI system type, used by both chains: UEFI loads L4TLauncher (`BOOTAA64.efi`) from it whatever the chain, and takes a staged capsule only from a partition of that type. It sits outside the A/B pair | 64 MiB |
| `esp_alt` | unused: the fail-safe copy NVIDIA's image-based OTA keeps beside `esp`; no partition type, empty after a flash, used by neither chain | 64 MiB |
| `A_reserved_on_user`, `B_reserved_on_user`, `reserved` | NVIDIA's reserve, left unused | 31.6 MiB each, 479.5 MiB |
| `UDA` | NVIDIA's user-data partition, too small for `/data`, left unused | 400 MiB |
| `/data` | new: the machine state, the update cache and the run state, as on x86 | the rest of the disk, about 205 GiB on the lab box's 238.5 GiB NVMe |

**One L4TLauncher serves both chains.** It sits in `esp`, outside the A/B
pair, and only a flash writes it (L5), so it must start the kernel on
whatever firmware each chain holds. By edk2-nvidia's source, 36.4.4's
L4TLauncher also runs on 36.5.2 firmware, while 36.5.2's drops the
firmware's own kernel arguments on 36.4.4 firmware. E4 runs the first
pairing: after its reflash to a 36.4.4 build, 36.4.4's L4TLauncher starts
the chain its capsule moves to 36.5.2.

**The boot state stays NVIDIA's**, as expected from edk2-nvidia's source
until D2 measures it:

- **L4TLauncher's retry counter.** A PMC scratch register holds a count per
  root slot. Each boot through L4TLauncher takes one from the current slot;
  at zero it marks the slot unbootable (`RootfsStatusSlotA` or `B`,
  non-volatile UEFI variables) and switches to the other slot, and with it
  to the other bootloader chain; with both slots unbootable it boots
  `recovery`. A cold boot sets the register up afresh, so a power cut gives
  a slot its full count back: only warm resets (a panic, a reboot and, by
  NVIDIA's documentation, the watchdog) count down. NVIDIA's kernel driver
  arms the watchdog's "system POR" reset, so D2b measures which kind of
  reset it is.
- `RootfsRetryCountMax` is **3** (Q5). The applier adds a **one-try trial
  counter** on `/data`: the second boot of a trial, warm or cold, rolls it
  back, which a power cut during the trial would otherwise not do.
- **The boot-chain variables** (`BootChainFwCurrent`, `BootChainFwNext`,
  `BootChainFwStatus`, `BootChainFwPrevious`, `BootChainFwResetCount`)
  drive the switch between the two QSPI chains. A stale status or next
  value cancels a capsule, a set `BootNext` makes a chain switch fail, and
  the reset count stops chain switches at 3. Before it stages anything the
  applier checks `BootChainFwStatus`, `BootChainFwNext` and `BootNext`.
  `BootChainFwResetCount` and `BootChainFwPrevious` cannot be checked from
  Linux: edk2-nvidia declares them boot-services-only under
  `gNVIDIATokenSpaceGuid`, so efivarfs never shows them (the stock box's has
  neither). The applier checks the count only if it ever appears; by the
  source it is deleted at the end of every switch, so it reaches 3 only
  within one switch's resets, and D2 checks it during a switch.
- **Each chain's firmware version** comes from `SystemFwVersions`, which
  UEFI fills at every boot from both chains' VER partitions, as NVIDIA's own
  boot scripts read it. The applier decides by it whether the other chain
  needs the capsule, so a reflash or a capsule outside Fjarr cannot leave a
  stale record deciding, and a chain it cannot read gets the capsule; its
  own per-chain marker on `/data` only says whether Fjarr's last write of
  the chain completed.
- **A finished switch leaves `BootChainFwStatus` set** to 0 (success), and
  left set it cancels the next capsule and makes the next chain switch end
  without switching. `mark-good` and `fjarr-boot-verify` clear a leftover 0
  once the boot is known good, and `rollback-now` clears any status before
  it switches; a failure status (such as 12, the new chain failed) is left
  for a person. D2 measures whether `nvbootctrl` already does either.
- **The status variable alone does not make a slot unbootable**:
  L4TLauncher reads `RootfsStatusSlot` only to seed the PMC retry counters
  when the register is uninitialised, and the register outlives a warm
  reset. So whenever the applier marks a slot unbootable or bootable again
  it writes the slot's PMC retry counter too (`rootfs_retry_count_a` or
  `_b`, NVIDIA's sysfs nodes), next to `RootfsStatusSlot`; without that, a
  slot rewritten after a failed trial would keep a counter of 0 and fall
  straight back.
- **`nvbootctrl`**, NVIDIA's closed tool, reads and writes this state, and
  `nvbootctrl verify` marks a boot good. **`fjarr-boot-verify` replaces
  meta-tegra's `nv_update_verifier`**: it verifies every boot that is not a
  trial, and only `mark-good` verifies a trial, after health. Without it, a
  committed slot would use up its retries in three warm reboots.

**Flashing from the mini-PC.** Linux on the box enters RCM by software
(`reboot forced-recovery`, which meta-tegra ships as `reboot-recovery`),
and a power cycle through the plug leaves it, with no button: seen once on
the stock box on 2026-10-16. meta-tegra's troubleshooting guide calls the
button the safer way in, citing an NVIDIA forum report of MB1 crashing on a
software reboot into RCM from chain B; L3 makes every Fjarr Linux, on
either chain, pass its own RCM round trip before anything relies on it. The
mini-PC then flashes the box over USB: meta-tegra's `initrd-flash` runs
natively, and NVIDIA's `l4t_initrd_flash.sh`, with its backup and restore
tools, runs in an Ubuntu 22.04 container, a host NVIDIA supports. NVIDIA's
known issue 4695663 (L4T 36.4.4: initrd flashing can fail on host USB ports
of 10 Gbps or more; the workaround is a hub or a slower port) called for a
USB 2.0 hub between the two, unless B found a port on the mini-PC slower
than 10 Gbps. B2 found one: the box is on a port of the mini-PC's ASM1042A
controller, whose SuperSpeed side is 5 Gbps (the box links at USB 2.0,
480 Mbit/s), below the issue's 10 Gbps. Whether the USB 2.0 hub is still
required before the first flash is Erik's call (open, 2026-10-17); the
recommendation is to keep one at hand as the fallback if an initrd flash
on that port fails. Every new firmware build first boots from RAM over
RCM, writing nothing (D0), before it is written to the QSPI.

**Three rescue layers**, each able to end in RCM, from which the mini-PC
can always reflash the box:

1. **The recovery-partition rescue.** Fjarr's own small system in
   `recovery`: an Android boot image with the slot's kernel and an
   initramfs carrying sshd with the lab keys, DHCP, the disk tools and
   `reboot-recovery`, with its device tree in `recovery-dtb`. L4TLauncher
   boots it when both slots are unbootable, and the lab reaches it once
   through a `BootNext` entry that starts L4TLauncher with
   `bootmode=recovery`. meta-tegra leaves the partition empty, so the
   recipe fills it.
2. **The network rescue.** When the firmware runs but nothing on the NVMe
   boots, UEFI tries its network boot entries and then waits at its setup
   screen. The mini-PC answers with proxy-DHCP and PXE and HTTP boot, only
   to the box's MAC, and serves a kernel with a built-in initramfs carrying
   the same tools. It serves whenever anything writes the NVMe, the ESP or
   the UEFI variables.
3. **RCM**, entered by software from either rescue or from a slot. The
   recovery button is needed only when none of them comes up: a bootloader
   chain that hangs before UEFI, or a trial kernel that hangs before
   userspace while no watchdog covers it (D4, [docs/18](18-open-questions.md)
   #48; [Safety](#safety)).

**The lab dead-man.** Every Fjarr Linux on the lab box, each slot and both
rescues, enters RCM when no operator contact has reached it for 20 minutes
(the lab scripts touch a file over ssh). A power cut is a cold boot that
resets the retry counter, so without the dead-man a slot that boots but
cannot be reached would be booted again forever. A trial also keeps its
confirmation deadline, a warm reboot, so it falls back first.

**The board package**, shared by both recipes:

- **Fjarr's own bootloader files:** a patch carrying Stereolabs' MB1
  pinmux and GPIO changes to NVIDIA's two files for them, which are BSD-3,
  and an MB2 configuration of Fjarr's own without the carrier EEPROM read
  (the carrier has none, and with NVIDIA's default MB2 stops before UEFI).
  NVIDIA's MB2 file states no licence and includes NVIDIA's proprietary
  common file, so Fjarr's short file includes that common file and sets
  only the EEPROM block, instead of patching NVIDIA's. NVIDIA's three files
  are byte-identical in L4T 36.4.4 and 36.5.2, so Stereolabs' changes carry
  over unchanged ([docs/18](18-open-questions.md) #45). Every reflash and
  every capsule carries them.
- **Fjarr's own carrier device tree** (the USB port mapping, a micro-USB
  connector, a pulse-per-second input, a suspend key), applied to NVIDIA's
  36.5.2 device tree, in the slot, so it switches with it. Stereolabs'
  36.4.4 kernel device trees are never copied over NVIDIA's: they would
  undo NVIDIA's 36.5.2 changes.
- **Stereolabs' camera package**, pinned by SHA-256 and fetched at build
  time; its maintainer scripts never run ([docs/18](18-open-questions.md)
  #45). Its kernel modules have to be built from source for Fjarr's
  kernels, so the cameras wait for that source (#47); 8.0's OTA tests do
  not need them.

**Lab safety rules**, in every phase:

| # | Rule |
|---|---|
| L1 | One lock on the mini-PC for anything that touches the box, its plug or RCM; nothing runs 00:00–06:30, the nightly window and the 30 minutes `fjarr-lab` gives a job still running at its end |
| L2 | Before any write to the NVMe, the ESP or the UEFI variables: the plug answers, the box answers on its MAC (scripts find it by MAC, not by address), the network rescue is serving, and the latest backup has a verified checksum list. One exception, B6's T2: its `BootNext` writes, to entries already in `BootOrder`, come before the network rescue is proven, since its PXE step must find no server |
| L3 | Every Fjarr Linux can reach RCM: it ships `reboot-recovery`, opens the watchdog from its initrd, runs the dead-man, and passes one RCM round trip before anything relies on it |
| L4 | No QSPI write of a firmware build that has not booted from RAM over RCM first. No QSPI write at all without Q4, except a capsule byte-identical to firmware this box has already booted from QSPI, once the first capsule passed under Q4 |
| L5 | `BootNext` is never left set while a capsule or chain switch is pending, and `BOOTAA64.efi` is never written |
| L6 | At most two warm resets in a row without a verify, on any committed slot |
| L7 | Anything unexpected: stop, record the state if the box answers, retry nothing blindly |
| L8 | The plug powers on by default after its own outage, and has no timers |

**The phases**, in order:

| Phase | What | Writes |
|---|---|---|
| A | Off the box: the BSPs and their layout (A1), the board package, the image, the capsule's contents, both rescues, the applier against simulated UEFI variables, the flash container, the restore tools | nothing |
| B | On the stock box: a read-only state dump and online backup, five RCM round trips, then one-shot boots through `BootNext` (the network rescue, the stock recovery, Fjarr's rescue in `recovery`) and the watchdog tests last | `recovery` (backed up), boot entries, a safety script for the stock recovery |
| C | A full backup with NVIDIA's tools, and one proven restore (Q3) | NVMe; QSPI only with Q4 |
| D | Fjarr's firmware booted from RAM (D0), the first Fjarr flash (D1), NVIDIA's A/B semantics (D2), the watchdog on a committed slot (D2b), the applier's rehearsal (D3), a hung trial kernel (D4, Q4) | NVMe, QSPI |
| E | Bootloader capsules: as the slot switch (E1, Q4), rolled back with the slot (E2), refused when unsafe (E3), a real version change from 36.4.4 (E4, Q9, Q4), a broken chain (E5, Q4) | QSPI |
| F | Power cuts through the plug, 20 per group: during install (8.0's gate), `set-next` with and without a capsule, the trial and `mark-good`; during a capsule's write only with Q4 | NVMe, ESP; QSPI only with Q4 |
| G | NixOS on 36.5.2: path (i) from a Yocto slot, its firmware, the applier and the install cuts | NVMe, QSPI |

**NVIDIA's restore tool** (`tools/backup_restore/`, the same QSPI handling
in L4T 36.4.4 and 36.5.2) discards and rewrites the whole NVMe, and erases
and rewrites the whole QSPI, the UEFI variables with it, whenever the
backup's index lists the QSPI. Its comment filter matches nothing, so a
commented-out line still counts: C's restore without Q4 runs from an index
with that line deleted.

**Measured**, filled in as the phases run (the files are in
[`spikes/ota-jetson/results/`](../spikes/ota-jetson/results/README.md)):

- The stock box's state, read only (B1, 2026-10-17): L4T R36.4.4 with
  Stereolabs' 1.3.1 package, bootloader chain A current and active, both
  chains normal, RootFS A/B off (`RootfsRedundancyLevel` 0),
  `RootfsRetryCountMax` 3, the ESRT at 36.4.4; `BootNext`,
  `OsIndications`, `BootChainFwNext` and `BootChainFwStatus` absent
  (`BootChainFwPrevious` and `BootChainFwResetCount` are not visible to
  Linux by the source, above); `/proc/mtd` empty, so the stock kernel does
  not show the QSPI.
- The mini-PC as the flashing host, read only (B2, 2026-10-17): the box's
  port above (5 Gbps SuperSpeed side); 60 s of listening saw no DHCP or PXE
  traffic on the lab network; GNOME automount is on for the desktop user,
  which the lab scripts keep the box away from.
- The online backup (B3, 2026-10-17): the GPT, sgdisk's backup, the UEFI
  variables and the state, 38 files with `sha256sum -c` and `zstd -t`
  good. Not yet: the root partition's stream, which did not fit before the
  hard stop, and the encrypted second copy, which the workstation's uplink
  was too slow for; both wait for a morning slot.
- The plug (B5, 2026-10-17): `initial_state` set from `restore_last` to
  `on`, auto-on and auto-off off, no schedules, webhooks or scripts: L8
  holds, unchanged at the re-check.
- RCM entered by software and left by a power cycle, five times of five
  (B4): **5 of 5 on 2026-10-18** (`LAB_DESKTOP_OK=1`): RCM 21 s after
  `reboot --force forced-recovery` (5.3 W in RCM); after 10 s off, L4T back
  on USB in 38–39 s, ssh in 39–40 s, started in 39–42 s; the reset reason
  `SYS_RESET_N` and the slot state unchanged every time
  (`spikes/ota-jetson/results/2026-10-18-rcm.csv`).
- `BootNext` honoured and consumed (B6 T2, 2026-10-18): consumed on a boot
  through the NVMe entry, and honoured through `UEFI PXEv4` with no server,
  whose fall-through to the NVMe took 30 s longer (106 s from the reboot to
  ssh); the slot state unchanged. T2 set `BootNext` before the network rescue
  was proven, the exception in L2.
- The network rescue (B6 T5, 2026-10-18): reached by both PXEv4 (76 s from
  the reboot to ssh) and HTTPv4 (51 s), served from the mini-PC to the box's
  MAC only; each time `BootNext` consumed, the NVMe read only (the GPT as the
  stock system reads it, `APP` mounted `ro,noload`), the Ethernet port up on
  `r8168`, `reboot-recovery` to RCM in 20 s, and the stock system back 38–41 s
  after one power cycle.
- The stock recovery through a `bootmode=recovery` entry (B6 T4,
  2026-10-18): L4TLauncher booted it from an entry made with
  `efibootmgr -C` outside `BootOrder`, `BootNext` consumed, `BootOrder`
  unchanged, and the lab's safety script ran and wrote both variables through
  efivarfs; but it did not answer ssh within 600 s, so T4 stopped (L7) and one
  power cycle brought back the stock system. Its networking is open, and T6
  and T3 wait for it.
- Whether the watchdog fires once userspace stops feeding it, or NVIDIA's
  driver keeps feeding it itself, and what a panic does to the retry count
  (B6 T3): pending, after T4.
- One restore of the full backup (C): pending.
- Whether the factory QSPI carries NVIDIA's QSPI fix (`trimmer2-val`),
  read from its dump (B, C): not answerable in B, since the stock kernel
  does not show the QSPI to Linux (B3); C's QSPI dump with NVIDIA's backup
  tool answers it: pending.
- Fjarr's firmware from RAM: MB2 past its EEPROM step, and the flashing
  gadget with Fjarr's device tree (D0): pending.
- The first flash: both slots and the rescue written, RCM reached from the
  Fjarr image (D1): pending.
- NVIDIA's semantics: retries on warm reboots without a verify, what
  `nvbootctrl verify` writes, the count after a cold boot, whether a
  fallback switches the chain, `BootChainFwResetCount` after a successful
  switch, and that both chains take L4TLauncher and the capsule from `esp`,
  as A1 read in edk2-nvidia's source (D2): pending.
- A watchdog reset on a committed slot, with and without the kernel fixes,
  and whether it counts down the retry counter like a warm reboot or
  restores it like a power cut (D2b): pending.
- Every rehearsal case from a fresh flash (D3): pending.
- A hung trial kernel recovering by itself, or the gap recorded (D4,
  [docs/18](18-open-questions.md) #48): pending.
- The capsule as the slot switch, rolled back with the slot: the hypothesis
  [above](#updater) (E1, E2): pending.
- The version ratchet after a reflash, and a real version change with its
  apply time, with 36.4.4's L4TLauncher left in the shared ESP (E4):
  pending.
- Power cuts during install: 20 of 20 back on the old slot, 8.0's gate for
  the Yocto image (F): pending.
- NixOS through L4TLauncher from a slot, with rollback and RCM (G): pending.
- Flash, install and capsule times: pending.

**After measuring**, two ADRs are planned: one on the Jetson boot path and
applier (decided by D and G), and one on the capsule ratchet and signing
keys (decided by E, Q11).

## Open core {#open-core}

Open (AGPL): the release format and `fjarr bundle`, the board packages and
recipes, `fjarr-update` with its appliers, conditions, local API, prepare
and hooks, the handler contract, `fjarr provision`,
`<UpdatePrompt>`, and installing a release on one robot or by hand. Paid
(Fjarr Cloud and the commercial license): campaigns across a fleet (stages,
batching, abort, the fleet's update policy and its enforcement), the chunk
store and download prediction as a service, Fjarr-managed signing keys, and
long-term release statistics. The reference handlers are MIT
([ADR-0038](adr/0038-reference-handlers-permissive.md)). See
[docs/03](03-product-strategy.md#open-core-split-adr-0011-adr-0015).

## Decided on review {#review-decisions}

Erik's answers to the points this design first proposed (2026-10-13):

- **Policies per campaign** as well as per fleet: yes. A campaign may set its
  own policy and deadline (an urgent fix as `auto` within 24 hours in a fleet
  that otherwise asks its owners).
- **`ask-operator`** (someone at the robot company approves each robot):
  later, when a customer needs it; it is the same consent mechanism pointed
  at another person, so adding it changes nothing else.
- **Stages spread across hardware variants:** an option when choosing a stage,
  off by default, so a release that breaks one variant is caught in the
  first stage.
- **The reference handlers are MIT**
  ([ADR-0038](adr/0038-reference-handlers-permissive.md)), so customers can
  fork them into proprietary handlers.

## Not in M8

Depot chunk sharing; the Syslogic AGX Orin board package; statistical release comparison (M7);
automatic promotion or halting of campaigns; handlers beyond the three
reference ones.

## Open questions

[docs/18](18-open-questions.md) #45: whether the ZED Box Mini's board files
may be packaged openly, settled by slice 8.0. #47: the source of
Stereolabs' camera modules, which Fjarr's Jetson kernels must build. #48:
protection against a trial kernel that hangs early on Jetson, a gate for
8.4 (Q8). #43 and #44 were closed on 2026-10-13
([Recipes, not images](#building)).
