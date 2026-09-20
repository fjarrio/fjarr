// fjarr.camera — the built-in camera capability.
// spec: docs/06-capabilities.md#fjarrcamera--camera-video-m1-reference-implementation
#include <fjarr/camera_capability.hpp>

#include <map>
#include <vector>

#include <fjarr/errors.hpp>
#include <fjarr/session_context.hpp>
#include <fjarr/video_source.hpp>

#include "core/log.hpp"

namespace fjarr {

struct CameraCapability::Impl {
    struct Track {
        std::string track_id, label, output;
        bool required = false;
        std::shared_ptr<VideoSource> source;
    };
    std::vector<Track> tracks; // config order
    std::map<SessionId, SessionContext*> sessions;

    /// The full set (session_attached: the core holds back what is unavailable) or only what is
    /// available now (hot-plug: the core's diff removes a track that left, by renegotiation).
    std::vector<TrackSpec> specs(bool available_only = false) const {
        std::vector<TrackSpec> out;
        for (const auto& t : tracks) {
            if (available_only && !t.source->available()) continue;
            TrackSpec s;
            s.track_id = t.track_id;
            s.label = t.label;
            s.kind = TrackKind::Video;
            s.source = SourceRef{t.source, t.output};
            out.push_back(std::move(s));
        }
        return out;
    }
};

CameraCapability::CameraCapability() : impl_(std::make_unique<Impl>()) {}
CameraCapability::~CameraCapability() = default;

CapabilityManifest CameraCapability::manifest() const {
    CapabilityManifest m;
    m.name = "fjarr.camera";
    m.version = {0, 1, 0};
    for (const auto& t : impl_->tracks) m.tracks.push_back({t.track_id, t.label, TrackKind::Video});
    m.channels = {{ChannelClass::Control}}; // select-tracks / bandwidth-stats: the core's generic track control
    m.config_schema = nlohmann::json{
        {"type", "object"},
        {"properties",
         {{"enabled", {{"type", "boolean"}}},
          {"tracks",
           {{"type", "object"},
            {"additionalProperties",
             {{"type", "object"},
              {"properties",
               {{"label", {{"type", "string"}}},
                {"source", {{"type", {"string", "object"}}}},
                {"output", {{"type", "string"}}},
                {"required", {{"type", "boolean"}}}}},
              {"required", {"source"}},
              {"additionalProperties", false}}}}}}},
        {"additionalProperties", false}};
    m.consumers = {.peer = true, .backend = false};
    m.input_bearing = false;
    return m;
}

void CameraCapability::configure(const nlohmann::json& config, const SourceFactory& sources) {
    // Built aside and swapped in at the end: a failing track leaves the previous configuration intact.
    std::vector<Impl::Track> configured;
    const nlohmann::json tracks = config.value("tracks", nlohmann::json::object());
    for (auto it = tracks.begin(); it != tracks.end(); ++it) {
        const std::string id = it.key();
        const nlohmann::json& t = it.value();
        Impl::Track track;
        track.track_id = id;
        track.label = t.value("label", id);
        track.output = t.value("output", "src");
        track.required = t.value("required", false);
        if (!t.contains("source")) throw FjarrError("config", "capabilities.fjarr.camera.tracks." + id + ": source is required");
        try {
            track.source = sources.create(t.at("source"));
        } catch (const FjarrError& e) {
            throw FjarrError("config", "capabilities.fjarr.camera.tracks." + id + ": " + e.message());
        }
        const bool available = track.source->available();
        log::info("camera", "track configured", {{"track", id}, {"identity", track.source->describe().identity}, {"available", available ? "yes" : "no"},
                                                 {"required", track.required ? "yes" : "no"}});
        if (track.required && !available)
            // docs/23 / ADR-0020: a required track with a missing driver or device does not start.
            throw FjarrError("config", "capabilities.fjarr.camera.tracks." + id + " is required but its source is unavailable (" +
                                           track.source->describe().identity + ")");
        configured.push_back(std::move(track));
    }
    impl_->tracks = std::move(configured);
    // Hot-plug: a source that arrives or leaves re-offers every session with what is available
    // now (docs/23: the core diffs by track_id — a track that left is removed by renegotiation,
    // valve first; one that arrived is added; the callback lands on the core loop).
    for (auto& t : impl_->tracks) {
        VideoSource* src = t.source.get();
        const std::string id = t.track_id;
        src->on_availability_changed([this, id](bool now) {
            log::info("camera", now ? "track available" : "track unavailable", {{"track", id}});
            for (auto& [_, ctx] : impl_->sessions) ctx->update_tracks(impl_->specs(true));
        });
    }
}

void CameraCapability::session_attached(SessionContext& ctx, const nlohmann::json&) {
    impl_->sessions[ctx.id()] = &ctx;
    for (auto& spec : impl_->specs()) ctx.add_track(std::move(spec)); // unavailable ones are held back by the core
}

void CameraCapability::session_detached(const SessionId& id, DetachReason, std::string_view) { impl_->sessions.erase(id); }

void CameraCapability::on_message(SessionContext& ctx, const Envelope& msg) {
    // select-tracks and bandwidth-stats are served by the core (docs/08 track control); nothing else exists.
    if (msg.kind == "request") ctx.fail(msg, "payload-invalid", "fjarr.camera has no request " + msg.type);
}

void CameraCapability::shutdown() { impl_->sessions.clear(); }

std::vector<Capability::ConfiguredSource> CameraCapability::configured_sources() const {
    std::vector<ConfiguredSource> out;
    for (const auto& t : impl_->tracks) {
        const bool available = t.source->available();
        out.push_back({t.track_id, t.label, t.source->describe().identity, available, t.required, available ? "" : t.source->unavailable_reason()});
    }
    return out;
}

} // namespace fjarr
