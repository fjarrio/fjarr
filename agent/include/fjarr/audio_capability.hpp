#pragma once
// fjarr.audio — the robot's microphones as audio tracks (M3.5 slice 3.5.1), offered only to a
// session whose grant says `listen`; push-to-talk to the robot follows in slice 3.5.3.
// spec: docs/06-capabilities.md#fjarraudio--two-way-audio-planned · docs/23-agent-core-architecture.md#audio
#include <memory>

#include <fjarr/capability.hpp>

namespace fjarr {

class AudioCapability final : public Capability {
  public:
    AudioCapability();
    ~AudioCapability() override;

    CapabilityManifest manifest() const override;
    /// Config: `mics.<id> = { label, source ({type = "alsa", device} | {type = "tone", freq} | …) }`.
    void configure(const nlohmann::json& validated_config, const SourceFactory& sources) override;
    /// The microphones are this session's only when its grant carries `listen: true` (docs/10).
    void session_attached(SessionContext& ctx, const nlohmann::json& granted_params) override;
    void session_detached(const SessionId& id, DetachReason reason, std::string_view detail) override;
    void on_message(SessionContext& ctx, const Envelope& msg) override;
    std::vector<ConfiguredSource> configured_sources() const override;
    void shutdown() override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fjarr
