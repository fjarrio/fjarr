#pragma once
// The video source contract: one way every track enters the media plane.
// spec: docs/09-interfaces.md#the-video-source-contract
// spec: docs/23-agent-core-architecture.md#video-sources-one-contract-three-ways-to-provide-one
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <gst/gst.h>
#include <nlohmann/json.hpp>

#include <fjarr/capability.hpp>

namespace fjarr {

struct SourceOutput {
    std::string name = "src";           // "src" for single-output sources; "left"/"right"/"depth" …
    TrackKind kind = TrackKind::Video;  // Video | Audio
    std::string declared_caps;          // what the output will produce (raw, DMABuf/VAMemory, or x-h264…)
};

struct SourceInfo {
    std::string identity; // stable device identity (serial, by-id path, url)
    std::vector<SourceOutput> outputs;
};

class VideoSource {
  public:
    virtual ~VideoSource() = default;
    virtual SourceInfo describe() const = 0;
    /// A floating GstBin exposing ghost pad "src" (or "src_<name>" per
    /// output). Created on first demand, disposed after idle; may be called
    /// again later. Ownership transfers to the caller (a floating ref).
    virtual GstBin* create_bin() = 0;
    virtual bool available() const = 0;
    virtual void on_availability_changed(std::function<void(bool)> cb) = 0; // hot-plug
    virtual void on_unavailable(std::function<void(std::string reason)> /*cb*/) {} // permanent failure
};

/// Registered source types: config `source = { type = "acme.stereo", … }`
/// → factory(params validated against schema). Built-ins: gst, test.
struct SourceType {
    std::string name; // reverse-DNS for third parties
    nlohmann::json params_schema;
    std::function<std::unique_ptr<VideoSource>(const nlohmann::json& params)> create;
};

/// A track's source: a VideoSource plus which of its outputs (owned by the
/// media plane through the shared_ptr; capabilities hand it over at attach).
struct SourceRef {
    std::shared_ptr<VideoSource> source;
    std::string output = "src";
};

} // namespace fjarr
