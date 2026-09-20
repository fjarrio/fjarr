#pragma once
// fjarr.camera — camera video, the M1 reference capability: tracks from config, any source
// through the video source contract, hot-plug by renegotiation.
// spec: docs/06-capabilities.md#fjarrcamera--camera-video-m1-reference-implementation
// spec: docs/23-agent-core-architecture.md#video-sources-one-contract-three-ways-to-provide-one
#include <memory>

#include <fjarr/capability.hpp>

namespace fjarr {

class CameraCapability final : public Capability {
  public:
    CameraCapability();
    ~CameraCapability() override;

    CapabilityManifest manifest() const override;
    /// Config: `tracks.<id> = { label, source (string | {type = …}), output = "src", required = false }`.
    /// A `required` track whose source is unavailable is a startup error (docs/23, ADR-0020).
    void configure(const nlohmann::json& validated_config, const SourceFactory& sources) override;
    void session_attached(SessionContext& ctx, const nlohmann::json& granted_params) override;
    void session_detached(const SessionId& id, DetachReason reason, std::string_view detail) override;
    void on_message(SessionContext& ctx, const Envelope& msg) override;
    /// The configured tracks with their availability and reason (GET /sources, `--check`, tests).
    std::vector<ConfiguredSource> configured_sources() const override;
    void shutdown() override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fjarr
