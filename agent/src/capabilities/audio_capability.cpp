// fjarr.audio — the robot's microphones (M3.5 slice 3.5.1).
// spec: docs/06-capabilities.md#fjarraudio--two-way-audio-planned · docs/23-agent-core-architecture.md#audio
#include <fjarr/audio_capability.hpp>

#include <map>
#include <vector>

#include <fjarr/errors.hpp>
#include <fjarr/session_context.hpp>
#include <fjarr/video_source.hpp>

#include "core/log.hpp"

namespace fjarr {

struct AudioCapability::Impl {
    struct Mic {
        std::string track_id, label;
        std::shared_ptr<VideoSource> source;
    };
    std::vector<Mic> mics; // config order
    std::map<SessionId, SessionContext*> listening; // sessions granted `listen`: the only ones with the tracks

    std::vector<TrackSpec> specs(bool available_only = false) const {
        std::vector<TrackSpec> out;
        for (const auto& m : mics) {
            if (available_only && !m.source->available()) continue;
            TrackSpec s;
            s.track_id = m.track_id;
            s.label = m.label;
            s.kind = TrackKind::Audio;
            s.source = SourceRef{m.source, "src"};
            out.push_back(std::move(s));
        }
        return out;
    }
};

AudioCapability::AudioCapability() : impl_(std::make_unique<Impl>()) {}
AudioCapability::~AudioCapability() = default;

CapabilityManifest AudioCapability::manifest() const {
    CapabilityManifest m;
    m.name = "fjarr.audio";
    m.version = {0, 1, 0};
    for (const auto& mic : impl_->mics) m.tracks.push_back({mic.track_id, mic.label, TrackKind::Audio});
    m.channels = {{ChannelClass::Control}}; // select-tracks: the core's generic track control
    m.config_schema = nlohmann::json{
        {"type", "object"},
        {"properties",
         {{"enabled", {{"type", "boolean"}}},
          {"mics",
           {{"type", "object"},
            {"additionalProperties",
             {{"type", "object"},
              {"properties", {{"label", {{"type", "string"}}}, {"source", {{"type", {"string", "object"}}}}}},
              {"required", {"source"}},
              {"additionalProperties", false}}}}}}},
        {"additionalProperties", false}};
    m.consumers = {.peer = true, .backend = false};
    m.input_bearing = false;
    return m;
}

void AudioCapability::configure(const nlohmann::json& config, const SourceFactory& sources) {
    std::vector<Impl::Mic> configured;
    const nlohmann::json mics = config.value("mics", nlohmann::json::object());
    for (auto it = mics.begin(); it != mics.end(); ++it) {
        Impl::Mic mic;
        mic.track_id = it.key();
        mic.label = it.value().value("label", it.key());
        try {
            mic.source = sources.create(it.value().at("source"));
        } catch (const FjarrError& e) {
            throw FjarrError("config", "capabilities.fjarr.audio.mics." + mic.track_id + ": " + e.message());
        }
        const auto info = mic.source->describe();
        if (info.outputs.empty() || info.outputs.front().kind != TrackKind::Audio)
            throw FjarrError("config", "capabilities.fjarr.audio.mics." + mic.track_id + ": the source is not an audio source (alsa, tone)");
        log::info("audio", "microphone configured", {{"track", mic.track_id}, {"identity", info.identity}, {"available", mic.source->available() ? "yes" : "no"}});
        configured.push_back(std::move(mic));
    }
    impl_->mics = std::move(configured);
    for (auto& mic : impl_->mics) {
        const std::string id = mic.track_id;
        mic.source->on_availability_changed([this, id](bool now) {
            log::info("audio", now ? "microphone available" : "microphone unavailable", {{"track", id}});
            for (auto& [_, ctx] : impl_->listening) ctx->update_tracks(impl_->specs(true));
        });
    }
}

void AudioCapability::session_attached(SessionContext& ctx, const nlohmann::json& granted) {
    // docs/10: a microphone is heard only with `listen`; a session without it is offered none, and
    // the core refuses its select-tracks for a track it was not offered (docs/08).
    if (!granted.is_object() || !granted.value("listen", false)) return;
    impl_->listening[ctx.id()] = &ctx;
    for (auto& spec : impl_->specs()) ctx.add_track(std::move(spec)); // unavailable ones are held back by the core
}

void AudioCapability::session_detached(const SessionId& id, DetachReason, std::string_view) { impl_->listening.erase(id); }

void AudioCapability::on_message(SessionContext& ctx, const Envelope& msg) {
    if (msg.kind == "request") ctx.fail(msg, "payload-invalid", "fjarr.audio has no request " + msg.type);
}

std::vector<Capability::ConfiguredSource> AudioCapability::configured_sources() const {
    std::vector<ConfiguredSource> out;
    for (const auto& m : impl_->mics) {
        const bool available = m.source->available();
        out.push_back({m.track_id, m.label, m.source->describe().identity, available, false, available ? "" : m.source->unavailable_reason()});
    }
    return out;
}

void AudioCapability::shutdown() { impl_->listening.clear(); }

} // namespace fjarr
