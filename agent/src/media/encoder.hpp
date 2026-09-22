#pragma once
// EncoderAdapter — the core owns encoding; hardware differences live here.
// spec: docs/23-agent-core-architecture.md#encoders-and-tiers · #configuration (encoder policy)
#include <string>

#include <gst/gst.h>

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

/// Build the encode branch "convert/scale … ! encoder ! h264parse" as a bin
/// with ghost pads `sink` and `src` (transfer floating). Elements are named
/// `<prefix>/convert`, `<prefix>/encoder`, `<prefix>/parser`.
GstElement* make_encode_bin(const EncoderChoice& enc, const TierProfile& tier, const std::string& prefix);

/// Passthrough (docs/06): the same ghost pads and output caps, but only a parser — a camera's own
/// H.264 is packetized as it is, so the track costs the robot no encoder at all.
GstElement* make_passthrough_bin(const std::string& prefix);

} // namespace fjarr::media
