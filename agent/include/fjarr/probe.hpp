#pragma once
// Bring one video source up standalone and report what it negotiates —
// `fjarr-agent --probe-source`, also available to embedders.
// spec: docs/09-interfaces.md#the-video-source-contract · docs/26 (setup / --check)
#include <chrono>
#include <string>

namespace fjarr {

struct ProbeResult {
    bool ok = false;
    std::string description; // the GStreamer description that was run
    std::string caps;        // negotiated caps at the sink
    int frames = 0;
    double fps = 0.0;
    std::string error;
};

/// A GStreamer description string (tier 1) or the built-in `test`.
ProbeResult probe_source(const std::string& spec, std::chrono::milliseconds duration = std::chrono::milliseconds(2000));

} // namespace fjarr
