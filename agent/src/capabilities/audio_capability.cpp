// fjarr.audio — the robot's microphones (M3.5 slice 3.5.1) and talking through its speaker (3.5.3).
// spec: docs/06-capabilities.md#fjarraudio--two-way-audio-planned · docs/23-agent-core-architecture.md#audio
// spec: docs/23-agent-core-architecture.md#audio-uplink · docs/08-protocol.md#talk
#include <fjarr/audio_capability.hpp>

#include <cmath>
#include <cstdlib>
#include <map>
#include <mutex>
#include <vector>

#include <gst/app/gstappsrc.h>

#include <fjarr/errors.hpp>
#include <fjarr/session_context.hpp>
#include <fjarr/video_source.hpp>

#include "core/glib/raii.hpp"
#include "core/log.hpp"
#include "media/sources.hpp"

namespace fjarr {

namespace {
constexpr std::chrono::milliseconds LEVEL_PERIOD{250}; // talk-level, docs/08#talk
constexpr int RATE = 48000;
constexpr std::size_t FRAME_SAMPLES = RATE / 50; // one 20 ms frame of mono
constexpr guint64 QUEUE_BYTES = RATE * 2 * 60 / 1000; // the speaker's appsrc holds at most 60 ms

double to_db(double linear) { return linear > 0 ? 20.0 * std::log10(linear) : -127.0; }
} // namespace

/// The robot's speaker, shared by every session's uplink: the streaming threads that decode the uplinks
/// push into `appsrc` only for the session that holds the floor (docs/23#audio-uplink). Guarded by `mu`.
struct Speaker {
    std::mutex mu;
    glib::GstElementPtr appsrc; // set while someone talks
    SessionId talker;
    double cap = 0.5;           // speaker.max_volume: the `volume` element's fixed value
    double sumsq = 0;           // since the last talk-level, after the cap (normalized to 1.0)
    double peak = 0;
    std::size_t samples = 0;
    std::uint64_t played = 0;   // samples since talk-start

    /// On a streaming thread (docs/09: on_pcm).
    void push(const SessionId& from, const std::int16_t* pcm, std::size_t n) {
        std::lock_guard<std::mutex> lk(mu);
        if (!appsrc || talker != from || n == 0) return; // a closed gate: never played (docs/08#talk)
        for (std::size_t i = 0; i < n; i++) {
            const double v = pcm[i] / 32768.0 * cap;
            sumsq += v * v;
            peak = std::max(peak, std::fabs(v));
        }
        samples += n;
        played += n;
        GstBuffer* buf = gst_buffer_new_memdup(pcm, n * sizeof(std::int16_t));
        gst_app_src_push_buffer(GST_APP_SRC(appsrc.get()), buf); // takes it
    }
};

struct AudioCapability::Impl {
    struct Mic {
        std::string track_id, label;
        std::shared_ptr<VideoSource> source;
    };
    struct Session {
        SessionContext* ctx = nullptr;
        bool listen = false, talk = false;
    };
    std::vector<Mic> mics; // config order
    std::map<SessionId, Session> sessions;

    // The speaker (3.5.3): configured, or not at all.
    bool has_speaker = false;
    std::string output = "alsa", device = "default";
    std::shared_ptr<Speaker> speaker = std::make_shared<Speaker>();
    glib::GstElementPtr pipeline; // the talker's, built at talk-start and gone at talk-stop
    std::unique_ptr<Timer> level_timer;

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

    /// `appsrc ! audioconvert ! volume ! alsasink | fakesink`, PLAYING; empty `error` when it is.
    glib::GstElementPtr build_output(std::string& error) {
        auto pipe = glib::sink_element(gst_pipeline_new("speaker"));
        auto src = glib::make_element("appsrc", "speaker/appsrc");
        auto conv = glib::make_element("audioconvert", "speaker/convert");
        auto vol = glib::make_element("volume", "speaker/volume");
        auto sink = glib::make_element(output == "alsa" ? "alsasink" : "fakesink", "speaker/sink");
        if (!pipe || !src || !conv || !vol || !sink) {
            error = output == "alsa" ? "elements missing (gstreamer1.0-alsa?)" : "elements missing";
            return {};
        }
        glib::GstCapsPtr caps(gst_caps_from_string("audio/x-raw,format=S16LE,rate=48000,channels=1,layout=interleaved"));
        g_object_set(src.get(), "caps", caps.get(), "is-live", TRUE, "format", GST_FORMAT_TIME, "do-timestamp", TRUE, "block", FALSE, "max-bytes",
                     QUEUE_BYTES, "leaky-type", 2 /* downstream: drop the oldest */, nullptr);
        g_object_set(vol.get(), "volume", speaker->cap, nullptr); // the hard cap: nothing on the wire changes it
        if (output == "alsa")
            // 40 ms of buffer: a release goes quiet well inside docs/06's 200 ms.
            g_object_set(sink.get(), "device", device.c_str(), "buffer-time", static_cast<gint64>(40000), "latency-time", static_cast<gint64>(10000), nullptr);
        else
            g_object_set(sink.get(), "sync", FALSE, nullptr);
        gst_bin_add_many(GST_BIN(pipe.get()), src.get(), conv.get(), vol.get(), sink.get(), nullptr);
        if (!gst_element_link_many(src.get(), conv.get(), vol.get(), sink.get(), nullptr)) {
            error = "the speaker pipeline did not link";
            return {};
        }
        if (gst_element_set_state(pipe.get(), GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
            gst_element_set_state(pipe.get(), GST_STATE_NULL);
            error = output == "alsa" ? "the speaker " + device + " could not be opened" : "the speaker pipeline would not start";
            return {};
        }
        {
            std::lock_guard<std::mutex> lk(speaker->mu);
            speaker->appsrc = std::move(src);
        }
        return pipe;
    }

    /// Close the gate: the floor is free, nothing queued plays on (docs/08#talk). Audited.
    void stop_talking(const SessionId& id) {
        {
            std::lock_guard<std::mutex> lk(speaker->mu);
            if (speaker->talker != id || !speaker->appsrc) return;
            speaker->appsrc.reset();
            speaker->talker.clear();
        }
        level_timer.reset();
        if (pipeline) {
            gst_element_set_state(pipeline.get(), GST_STATE_NULL);
            pipeline.reset();
        }
        auto it = sessions.find(id);
        if (it != sessions.end() && it->second.ctx) {
            it->second.ctx->session_event("audio-uplink", "talk-stop");
            log::info("audio", "talk stopped", {{"session", log::short_id(id)}, {"operator", it->second.ctx->operator_info().label}});
        }
    }

    void talk_start(SessionContext& ctx, const Envelope& msg) {
        auto& s = sessions[ctx.id()];
        if (!s.talk) return ctx.fail(msg, error_codes::capability_denied, "this session's grant does not allow talking (fjarr.audio talk)");
        if (!has_speaker) return ctx.fail(msg, error_codes::unavailable, "this robot has no speaker configured");
        {
            std::lock_guard<std::mutex> lk(speaker->mu);
            if (speaker->appsrc && speaker->talker == ctx.id()) {
                ctx.result(msg, {{"ok", true}}); // idempotent for the session that talks
                return;
            }
            if (speaker->appsrc) {
                const auto holder = sessions.find(speaker->talker);
                const OperatorInfo who = holder != sessions.end() && holder->second.ctx ? holder->second.ctx->operator_info() : OperatorInfo{};
                ctx.fail(msg, error_codes::busy, "someone else is talking through the robot's speaker",
                         {{"holder", {{"id", who.id}, {"label", who.label}}}});
                return;
            }
        }
        std::string error;
        pipeline = build_output(error);
        if (!pipeline) {
            log::warn("audio", "speaker unavailable", {{"error", error}});
            return ctx.fail(msg, error_codes::unavailable, error);
        }
        {
            std::lock_guard<std::mutex> lk(speaker->mu);
            speaker->talker = ctx.id();
            speaker->sumsq = speaker->peak = 0;
            speaker->samples = 0;
            speaker->played = 0;
        }
        // What reaches the speaker, after the cap, to the talking session (docs/08 talk-level).
        level_timer = ctx.every(LEVEL_PERIOD, [this, ctxp = &ctx]() -> bool {
            double rms = 0, peak = 0;
            std::uint64_t played = 0;
            {
                std::lock_guard<std::mutex> lk(speaker->mu);
                if (speaker->samples) rms = std::sqrt(speaker->sumsq / static_cast<double>(speaker->samples));
                peak = speaker->peak;
                played = speaker->played;
                speaker->sumsq = speaker->peak = 0;
                speaker->samples = 0;
            }
            ctxp->event("talk-level", {{"rms_db", std::round(to_db(rms) * 10) / 10}, {"peak_db", std::round(to_db(peak) * 10) / 10},
                                       {"frames", played / FRAME_SAMPLES}});
            return true;
        });
        ctx.session_event("audio-uplink", "talk-start");
        log::info("audio", "talk started", {{"session", log::short_id(ctx.id())}, {"operator", ctx.operator_info().label}, {"output", output}});
        ctx.result(msg, {{"ok", true}});
    }
};

AudioCapability::AudioCapability() : impl_(std::make_unique<Impl>()) {}
AudioCapability::~AudioCapability() = default;

CapabilityManifest AudioCapability::manifest() const {
    CapabilityManifest m;
    m.name = "fjarr.audio";
    m.version = {0, 2, 0};
    for (const auto& mic : impl_->mics) m.tracks.push_back({mic.track_id, mic.label, TrackKind::Audio});
    m.channels = {{ChannelClass::Control}}; // select-tracks, talk-start/stop
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
              {"additionalProperties", false}}}}},
          // docs/23#audio-uplink: where an operator's voice is played, and its hard volume cap.
          {"speaker",
           {{"type", "object"},
            {"properties",
             {{"output", {{"type", "string"}, {"enum", {"alsa", "null"}}}},
              {"device", {{"type", "string"}}},
              {"max_volume", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}}}},
            {"required", {"output"}},
            {"additionalProperties", false}}}}},
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
            for (auto& [_, s] : impl_->sessions)
                if (s.listen) s.ctx->update_tracks(impl_->specs(true));
        });
    }
    if (config.contains("speaker")) {
        const auto& sp = config["speaker"];
        impl_->has_speaker = true;
        impl_->output = sp.value("output", std::string{"alsa"});
        impl_->device = sp.value("device", std::string{"default"});
        impl_->speaker->cap = sp.value("max_volume", 0.5);
        log::info("audio", "speaker configured",
                  {{"output", impl_->output}, {"device", impl_->output == "alsa" ? impl_->device : "-"}, {"max_volume", std::to_string(impl_->speaker->cap)}});
    }
}

void AudioCapability::session_attached(SessionContext& ctx, const nlohmann::json& granted) {
    // docs/10: a microphone is heard only with `listen`, and the speaker used only with `talk`.
    auto& s = impl_->sessions[ctx.id()];
    s.ctx = &ctx;
    s.listen = granted.is_object() && granted.value("listen", false);
    s.talk = granted.is_object() && granted.value("talk", false);
    // A session without `listen` is offered no microphone, and the core refuses its select-tracks for
    // a track it was not offered (docs/08).
    if (s.listen)
        for (auto& spec : impl_->specs()) ctx.add_track(std::move(spec)); // unavailable ones are held back by the core
    // A session without `talk`, or a robot without a speaker, is offered no uplink (docs/08#talk).
    if (s.talk && impl_->has_speaker)
        ctx.add_inbound_audio({"talk", [speaker = impl_->speaker, id = ctx.id()](const std::int16_t* pcm, std::size_t n) { speaker->push(id, pcm, n); }});
}

void AudioCapability::session_detached(const SessionId& id, DetachReason, std::string_view) {
    impl_->stop_talking(id); // every end closes the gate (docs/15 safety)
    impl_->sessions.erase(id);
}

void AudioCapability::on_message(SessionContext& ctx, const Envelope& msg) {
    if (msg.kind != "request") return;
    if (msg.type == "talk-start") return impl_->talk_start(ctx, msg);
    if (msg.type == "talk-stop") {
        impl_->stop_talking(ctx.id());
        return ctx.result(msg, {{"ok", true}}); // idempotent
    }
    ctx.fail(msg, "payload-invalid", "fjarr.audio has no request " + msg.type);
}

std::vector<Capability::ConfiguredSource> AudioCapability::configured_sources() const {
    std::vector<ConfiguredSource> out;
    for (const auto& m : impl_->mics) {
        const bool available = m.source->available();
        out.push_back({m.track_id, m.label, m.source->describe().identity, available, false, available ? "" : m.source->unavailable_reason()});
    }
    return out;
}

void AudioCapability::shutdown() {
    // The sessions' contexts may be gone already: close the gate without reaching for them.
    for (auto& [_, s] : impl_->sessions) s.ctx = nullptr;
    std::string talker;
    {
        std::lock_guard<std::mutex> lk(impl_->speaker->mu);
        talker = impl_->speaker->talker;
    }
    if (!talker.empty()) impl_->stop_talking(talker);
    impl_->sessions.clear();
}

} // namespace fjarr
