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
| `firmware` | the Jetson bootloader capsule | the OS applier, as a separate component |
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
  bootloader capsule, most MCU flashes);
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
| 9. Forward-only | `forward-only` components (the bootloader capsule, MCU flashes), last | handlers, the applier |

On failure up to step 8, everything rolls back in reverse order: peripherals
through their handlers, apps and config with the slot or their snapshot, the
OS by booting the old slot. The **point of no return** is the commit (step 8),
or the first `forward-only` component if one must run earlier because of
`requires`; the run shows it, and abort and the owner's rescheduling stop
there. A `forward-only` component that fails after it does not roll the OS
back (the new firmware may require the new OS): the run ends **degraded**,
the robot is flagged, and the run shows the recovery path its recipe
documents.

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
  boot-failure fallback respects it.

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
those numbers.

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
  index than they save. Container and model updates still to measure may
  move it.
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
  `zstd --patch-from` stops at 2 GiB). A file-by-file diff inside the
  slot is the variant still to measure, with containers, CUDA and models.

## Appliers {#appliers}

The OS-level work (install to the inactive slot, set the next boot, read the
slot state, mark good, roll back now) is behind a narrow **applier
adapter**, one per recipe ([ADR-0035](adr/0035-releases-orchestrated-appliers-narrow.md)).
Fjarr owns everything around it: fetching, conditions, ordering, health,
peripherals, reporting. The applier installs only from the local cache, never
streaming over the network. Which engine backs which recipe is settled by the
measurements of slice 8.0. The starting points are:

- **Yocto on Jetson:** SWUpdate on meta-tegra's A/B layout;
- **NixOS:** the system's generations inside an A/B image layout;
- **x86 and Ubuntu images:** RAUC or systemd-sysupdate;
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
| Yocto | ZED Box Mini | meta-tegra + meta-fjarr, A/B rootfs on NVMe; the bootloader capsule a separate `firmware` component | A/B atomic; capsule forward-only | chunks |
| NixOS | ZED Box Mini | jetpack-nixos flake + the Fjarr module, A/B image layout | A/B atomic | chunks of store paths |
| Yocto | second x86 mini-PC | the same layer, UEFI | A/B atomic | chunks |
| NixOS | second x86 mini-PC | the same flake, UEFI | A/B atomic | chunks of store paths |
| Ubuntu A/B | second x86 mini-PC | an Ubuntu 26.04 image on two slots (build tool chosen in 8.0, mkosi the candidate) | A/B atomic | chunks |
| Ubuntu + apt | any Ubuntu robot | today's robots, unchanged | per package, not atomic | apt's own |

The last row matters: most robots running Fjarr today are stock Ubuntu. They
get the same releases, conditions, consent and run logs, without atomic
rollback, and the recipe table says so.

**The x86 layout** (all three x86 recipes): a GPT disk with an EFI system
partition, two root slots and a `/data` partition; boot counting by the
bootloader chosen in 8.0 (systemd-boot with unified kernel images, or GRUB's
environment); `/usr` read-only in the slot, `/etc` from the image with
machine-specific state on `/data`, and `/home` and `/var` on `/data`. The
accounts and groups the agent and the desktop helper need (ADR-0028) are
created by `sysusers.d` and `tmpfiles.d` in the image, so a desktop survives
slot switches. The Ubuntu image carries a GNOME desktop, as robots with a
desktop run today, so the M3 desktop tests can run on it too.

### The x86 test image in slice 8.0 {#x86-test-image}

Slice 8.0 tests one concrete layout on the lab's x86 mini-PC before any
recipe is built. The research behind it (2026-10-13) put **systemd-sysupdate
with systemd-boot** first and **RAUC with GRUB** second; the measurements
decide, and an ADR records the choice.

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
  TCO; whether it resets this board is tested with the power switch.
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
may be packaged openly, settled by slice 8.0. #43 and #44 were closed on
2026-10-13 ([Recipes, not images](#building)).
