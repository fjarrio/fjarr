#pragma once
#include <string>

namespace fjarr {

/// Fjarr library version (semver).
std::string version();

/// Runtime GStreamer version string.
std::string gstreamer_version();

/// True if the VA-API H.264 encode smoke pipeline
/// (videotestsrc ! vapostproc ! vah264enc ! fakesink) runs on this machine.
/// The environment doctor is the authoritative check; this mirrors it for
/// embedders. // spec: docs/12-development-environment.md#doctor
bool hardware_encode_available();

} // namespace fjarr
