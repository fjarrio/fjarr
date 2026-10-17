#pragma once
// EncoderAdapter — the core owns encoding; hardware differences live here.
// spec: docs/23-agent-core-architecture.md#encoders-and-tiers · #configuration (encoder policy)
#include <functional>
#include <string>
#include <vector>

#include <gst/gst.h>

#include <fjarr/desktop_backend.hpp>

namespace fjarr::media {

enum class EncoderKind { VaApi, Software };

struct EncoderChoice {
    EncoderKind kind;
    std::string name; // "vaapi" | "software"
};

/// Resolve the configured policy: auto → vaapi when the smoke pipeline
/// passes, otherwise an error (never a silent fallback); vaapi | software as
/// named. Throws FjarrError("config", …) naming the two options.
EncoderChoice resolve_encoder(const std::string& policy);

struct TierProfile {
    std::string tier; // active | thumbnail
    int width = 0;    // 0 = source size
    int height = 0;
    int fps = 30;
    int kbps = 4000;
    int gop_frames = 60;
};

/// Where a raw source's frames are: system memory, or DMA-BUF on the device (docs/23#encoders-and-tiers,
/// The device-memory head; ADR-0041).
enum class SourceMemory { System, DmaBuf };

/// Whether caps (a source's declared or negotiated caps) carry the `memory:DMABuf` feature.
SourceMemory memory_of_caps(const std::string& caps);

/// The import list (ADR-0041, docs/23#desktop-capture-memory): the DMA-BUF formats the encoder
/// family imports without a copy. For `vaapi`, the `drm-format` entries of `vapostproc`'s sink caps
/// under `memory:DMABuf`, kept to the 8-bit RGB formats mutter produces (XR24, AR24, XB24, AB24);
/// `software` imports none. `sink_caps(factory)` returns a factory's sink caps (a new ref, or null
/// when the element is missing); the default makes the element and asks its pad template.
std::vector<DmaBufFormat> dmabuf_imports(EncoderKind kind, const std::function<GstCaps*(const char* factory)>& sink_caps = {});
/// The vaapi rule on given caps (tests feed it a fake vapostproc).
std::vector<DmaBufFormat> dmabuf_imports_from_caps(const GstCaps* sink_caps);
/// "AR24:0x0200000000000901, XR24:0x…" for logs.
std::string describe_imports(const std::vector<DmaBufFormat>& list);

/// The description of the encode branch (no element is made): what make_encode_bin parses.
/// A DMA-BUF source on vaapi gets the device-memory branch, which takes the NV12 VA memory the
/// producer's head made: no videoscale and no system-memory caps, vapostproc only to scale the
/// thumbnail. Throws FjarrError("media") for DMA-BUF on a family that imports none.
std::string encode_description(const EncoderChoice& enc, const TierProfile& tier, const std::string& prefix, SourceMemory memory = SourceMemory::System);

/// Build the encode branch "convert/scale … ! encoder ! h264parse" as a bin
/// with ghost pads `sink` and `src` (transfer floating). Elements are named
/// `<prefix>/convert`, `<prefix>/encoder`, `<prefix>/parser`.
GstElement* make_encode_bin(const EncoderChoice& enc, const TierProfile& tier, const std::string& prefix, SourceMemory memory = SourceMemory::System);

/// Passthrough (docs/06): the same ghost pads and output caps, but only a parser — a camera's own
/// H.264 is packetized as it is, so the track costs the robot no encoder at all.
GstElement* make_passthrough_bin(const std::string& prefix);
/// An audio track's one tier (docs/23#audio): Opus, 48 kHz mono, 20 ms frames, 32 kbps, in-band FEC.
/// Opus for one audio track (docs/23#audio): voice at 32 kbps, music (a desktop's sound) at 64 kbps.
GstElement* make_opus_bin(const std::string& prefix, bool music = false);

} // namespace fjarr::media
