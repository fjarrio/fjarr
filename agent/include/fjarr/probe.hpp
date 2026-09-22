#pragma once
// Bring one video source up standalone and report what it negotiates —
// `fjarr-agent --probe-source`, also available to embedders.
// spec: docs/09-interfaces.md#the-video-source-contract · docs/26 (setup / --check)
#include <chrono>
#include <string>

#include <fjarr/video_source.hpp>

namespace fjarr {

struct ProbeResult {
    bool ok = false;
    std::string description; // the GStreamer description that was run
    std::string caps;        // negotiated caps at the sink
    std::string memory;      // "system" | "VAMemory" | "DMABuf" … from the caps features (zero-copy tells)
    /// Passthrough (docs/06): the source hands over an encoded stream, so the track needs no encoder.
    /// `codec` is what a browser must accept, e.g. "H.264 profile=main level=3.1".
    bool passthrough = false;
    std::string codec;
    int frames = 0;
    double fps = 0.0;
    std::string error;
};

/// A GStreamer description string (tier 1), the word `test`, or an inline TOML table of a
/// registered type — `{type = "v4l2", device = "/dev/video0"}` — exactly as `fjarr.toml` writes it.
ProbeResult probe_source(const std::string& spec, std::chrono::milliseconds duration = std::chrono::milliseconds(2000));
/// The same with a caller's factory (an embedding application's registered types).
ProbeResult probe_source(const SourceFactory& sources, const std::string& spec, std::chrono::milliseconds duration = std::chrono::milliseconds(2000));

} // namespace fjarr
