---
title: "ADR 0025: Four encoder families behind one adapter, and what each owes CI"
---

- **Status**: accepted
- **Date**: 2026-09-24
- **Closes**: [open question #2](../18-open-questions.md) (Jetson/NVENC encoder adapter — when?)

## Context

`EncoderAdapter` exists so that hardware differences live in one place
([docs/23](../23-agent-core-architecture.md#encoders-and-tiers)), but it has
only ever had one hardware implementation: VA-API on Intel
([ADR-0022](0022-baseline-ubuntu-2604-gstreamer-128.md)). A seam with one
implementation is not a seam, it is a spelling of that implementation, and we
will not find out which until a second one arrives.

Two things changed on 2026-09-24. A machine with an NVIDIA dGPU joined the
project as a self-hosted runner, and its whole stack was verified working
inside our own dev image: with the container toolkit's `video` capability
enabled, `gst-inspect-1.0 nvcodec` lists `nvh264enc` and fifteen siblings.
Nothing below Fjarr is missing. And the maintainer stated the target
explicitly: Intel, NVIDIA desktop and Jetson, with nightly tests on each.

The temptation is to read that as "add NVENC". The trap is that **"NVIDIA" is
two platforms**. A desktop card encodes through the nvcodec plugin in CUDA
memory on x86-64. A Jetson does not use nvcodec for encode at all: it uses
NVIDIA's L4T GStreamer with `nvv4l2h264enc`, on arm64, with frames in NVMM.
Different plugin, different packages, different architecture, different
zero-copy path. Treating them as one adapter would produce an abstraction
that fits neither.

## Options considered

**Keep VA-API only and defer.** Cheapest, and what open question #2 has said
since M0: design-partner demand decides. But the seam's genericity stays
unproven while it is still cheap to change, and the project already publishes
a platform matrix promising both adapters.

**One "NVIDIA" adapter.** Superficially tidy and wrong: nvcodec and nvv4l2
share a vendor and nothing else. The shared abstraction would be so thin that
each would need its own branch anyway, with the added cost of pretending
otherwise.

**Four families behind the existing adapter.** `software`, `vaapi`,
`nvcodec`, `nvv4l2` — named after the GStreamer plugin family, which is
unambiguous, self-documenting for an integrator reading NVIDIA's or Intel's
own docs, and exactly what the doctor probes. The seam stays; each family is
a branch that builds a bin and declares what it can do.

## Decision

**Four encoder families behind `EncoderAdapter`**, selected by
`media.encoder`:

| Family | Encoder | Platform | Source memory it wants |
|---|---|---|---|
| `software` | `openh264enc` | anywhere | system |
| `vaapi` | `vah264enc` | Intel, x86-64 | DMABuf / VAMemory |
| `nvcodec` | `nvh264enc` | NVIDIA dGPU, x86-64 | CUDA |
| `nvv4l2` | `nvv4l2h264enc` | Jetson, arm64, L4T | NVMM |

Three rules make that more than a list.

**1. `auto` prefers VA-API over nvcodec on a machine with both, and never
falls back to software in silence.** A robot with an Intel iGPU *and* a
discrete NVIDIA card is a normal robot, and on it the dGPU is usually running
perception. Taking its encoder to stream a camera is stealing from the job
the robot exists to do, while the iGPU is otherwise idle. `auto` therefore
probes `vaapi`, then `nvv4l2`, then `nvcodec`, and **fails loudly** when no
hardware family works — the software encoder stays an explicit choice
([docs/23](../23-agent-core-architecture.md)), now for four families instead
of one.

**2. The source's memory type picks the upload path, not the encoder.** A
frame already on the device is never round-tripped through system memory, and
a frame in system memory is converted once, on the CPU, and uploaded once.
Per family: DMABuf goes straight to `vapostproc`; system memory goes
`videoconvert ! cudaupload` for nvcodec, or `cudaupload ! cudaconvertscale`
where the CUDA runtime compiler is installed; NVMM goes through `nvvidconv`.
The tier's scale and rate filters move to the device wherever a device-side
equivalent exists. This is where the work actually is — the element name is
the easy part.

**3. A family declares whether it can change bitrate while playing.** Slice
6a's rate control sets an encoder's target every 500 ms. A family that cannot
take a live bitrate change reports that, and rate control degrades to tier
switching alone for its tracks, reusing the `adaptive: false` path slice 6b
already built for passthrough. No family gets to make rate control lie.

**Each family owes the nightly a runner.** A hardware path nobody exercises
nightly rots, quietly, and is then discovered by a customer. So the platform
matrix records, per family, whether a nightly runner exists. A family with no
runner is published as **best effort and untested**, never as supported. This
is the real recurring cost of the decision and the reason it is stated as a
rule rather than an aspiration.

Sequencing: `nvcodec` first, because the hardware exists and everything below
Fjarr is verified working on it; `nvv4l2` when a board does. The work lands
in **M2.6**, directly after the M2.5 packaging milestone, which already
builds arm64 images and owns the driver catalog a Jetson package belongs in.

## Consequences

The encoder seam gets its second and third implementations, which is the only
way to learn whether it is generic. Expect it to change shape; that is the
point of doing it while it is cheap.

Four families means four probes in the doctor, four rows in the platform
matrix, and a longer answer to "what hardware do I need". The `auto`
preference rule will surprise someone who bought a dGPU expecting it to be
used; the reasoning is written down so the surprise is answerable.

Licensing is unaffected: nvcodec is LGPL, the L4T elements ship from NVIDIA's
own repository, and the vendor driver libraries are loaded at runtime and
never linked or redistributed by us — the same posture as the Intel driver
([ADR-0011](0011-license-open-core.md)).

The runner rule is a standing obligation. If a machine dies and its family
loses nightly coverage, the honest move is to downgrade that row in the
matrix, not to leave it claiming support.
