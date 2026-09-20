#include "producer.hpp"

#include <gst/video/video.h>

#include "core/log.hpp"

namespace fjarr::media {

Producer::Producer(std::string track_id, SourceRef source, bool stamp, ProducerConfig config, FrameHub& hub,
                   GMainContext* bus_context)
    : track_id_(std::move(track_id)), source_(std::move(source)), stamp_(stamp), config_(config), hub_(hub),
      bus_context_(bus_context) {}

Producer::~Producer() { stop(); }

bool Producer::build() {
    if (!source_.source) {
        error_ = "no source";
        return false;
    }
    GstBin* bin = source_.source->create_bin();
    if (!bin) {
        error_ = "source bin failed to build";
        return false;
    }
    pipeline_ = glib::sink_element(gst_pipeline_new(name().c_str()));
    glib::ObjectCensus::instance().pipelines++;
    source_bin_ = glib::sink_element(GST_ELEMENT(bin));
    gst_object_set_name(GST_OBJECT(source_bin_.get()), (name() + "/source").c_str());
    convert_ = glib::make_element("videoconvert", name() + "/convert");
    rawcaps_ = glib::make_element("capsfilter", name() + "/rawcaps");
    glib::GstCapsPtr raw(gst_caps_from_string("video/x-raw,format=I420"));
    g_object_set(rawcaps_.get(), "caps", raw.get(), nullptr);
    tee_ = glib::make_element("tee", name() + "/tee");
    g_object_set(tee_.get(), "allow-not-linked", TRUE, nullptr);
    gst_bin_add_many(GST_BIN(pipeline_.get()), source_bin_.get(), convert_.get(), rawcaps_.get(), tee_.get(), nullptr);
    const std::string pad_name = source_.output == "src" ? "src" : "src_" + source_.output;
    glib::GstPadPtr src_pad = glib::adopt_pad(gst_element_get_static_pad(source_bin_.get(), pad_name.c_str()));
    if (!src_pad) {
        error_ = "source bin has no pad " + pad_name;
        return false;
    }
    glib::GstPadPtr conv_sink = glib::adopt_pad(gst_element_get_static_pad(convert_.get(), "sink"));
    if (gst_pad_link(src_pad.get(), conv_sink.get()) != GST_PAD_LINK_OK) {
        error_ = "cannot link source to convert";
        return false;
    }
    if (!gst_element_link_many(convert_.get(), rawcaps_.get(), tee_.get(), nullptr)) {
        error_ = "cannot link convert ! rawcaps ! tee";
        return false;
    }
    source_pad_ = glib::adopt_pad(gst_element_get_static_pad(rawcaps_.get(), "src"));
    if (stamp_) {
        stamp_probe_ = glib::PadProbe(source_pad_.get(),
                                      static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM),
                                      StampPainter::probe, &painter_);
    }
    glib::GstBusPtr bus(gst_pipeline_get_bus(GST_PIPELINE(pipeline_.get())));
    GSource* watch = gst_bus_create_watch(bus.get());
    g_source_set_callback(watch, reinterpret_cast<GSourceFunc>(&Producer::on_bus), this, nullptr);
    g_source_attach(watch, bus_context_);
    bus_watch_ = glib::SourceGuard::attached(watch);
    return true;
}

gboolean Producer::on_bus(GstBus*, GstMessage* msg, gpointer user) {
    auto* self = static_cast<Producer*>(user);
    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR: {
        GError* err = nullptr;
        gchar* dbg = nullptr;
        gst_message_parse_error(msg, &err, &dbg);
        glib::GErrorPtr e(err);
        glib::GStrPtr d(dbg);
        self->error_ = std::string(GST_OBJECT_NAME(GST_MESSAGE_SRC(msg))) + ": " + (err ? err->message : "error");
        log::error("producer", "pipeline error", {{"producer", self->name()}, {"error", self->error_}, {"debug", dbg ? dbg : ""}});
        if (self->on_error_) self->on_error_(self->error_);
        break;
    }
    case GST_MESSAGE_WARNING: {
        GError* err = nullptr;
        gst_message_parse_warning(msg, &err, nullptr);
        glib::GErrorPtr e(err);
        log::warn("producer", "pipeline warning", {{"producer", self->name()}, {"warning", err ? err->message : ""}});
        break;
    }
    default:
        break;
    }
    return G_SOURCE_CONTINUE;
}

TierProfile Producer::profile_for(const std::string& tier) const {
    TierProfile p;
    p.tier = tier;
    int w = source_width_, h = source_height_;
    if (w == 0) {
        // Negotiated caps once flowing; before that the source's declared caps (a thumbnail
        // demanded first, on a pipeline still in NULL).
        glib::GstCapsPtr caps(source_pad_ ? gst_pad_get_current_caps(source_pad_.get()) : nullptr);
        if (!caps && source_.source) {
            for (const auto& o : source_.source->describe().outputs)
                if (o.name == source_.output && !o.declared_caps.empty()) caps.reset(gst_caps_from_string(o.declared_caps.c_str()));
        }
        if (caps && gst_caps_is_fixed(caps.get())) {
            GstVideoInfo info;
            if (gst_video_info_from_caps(&info, caps.get())) {
                w = GST_VIDEO_INFO_WIDTH(&info);
                h = GST_VIDEO_INFO_HEIGHT(&info);
            }
        }
    }
    if (tier == "thumbnail") {
        // Half the source, capped at 960×540 (docs/23), 5 fps.
        int tw = w > 0 ? w / 2 : 640, th = h > 0 ? h / 2 : 360;
        if (tw > 960) {
            th = th * 960 / tw;
            tw = 960;
        }
        p.width = (tw / 2) * 2;
        p.height = (th / 2) * 2;
        p.fps = 5;
        p.kbps = config_.thumbnail_kbps;
    } else {
        p.width = 0;
        p.height = 0;
        p.fps = 30;
        p.kbps = config_.active_kbps;
    }
    p.gop_frames = config_.gop_seconds * p.fps;
    return p;
}

GstFlowReturn Producer::on_new_sample(GstAppSink* sink, gpointer user) {
    auto* t = static_cast<Tier*>(user);
    glib::GstSamplePtr sample = glib::adopt_sample(gst_app_sink_pull_sample(sink));
    if (!sample) return GST_FLOW_OK;
    t->hub->push(t->key, std::move(sample));
    return GST_FLOW_OK;
}

bool Producer::start_tier(const std::string& tier) {
    if (!pipeline_) return false;
    if (tiers_.count(tier)) return true;
    const TierProfile profile = profile_for(tier);
    const std::string prefix = name() + ":" + tier;
    auto t = std::make_unique<Tier>();
    t->key = HubKey{track_id_, tier};
    t->hub = &hub_;
    t->queue = glib::make_element("queue", prefix + "/queue");
    g_object_set(t->queue.get(), "leaky", 2 /* downstream */, "max-size-buffers", 2u, "max-size-bytes", 0u, "max-size-time",
                 static_cast<guint64>(0), nullptr);
    try {
        t->encode = glib::sink_element(make_encode_bin(config_.encoder, profile, prefix));
    } catch (const std::exception& e) {
        error_ = e.what();
        return false;
    }
    t->sink = glib::make_element("appsink", prefix + "/sink");
    g_object_set(t->sink.get(), "sync", FALSE, "emit-signals", FALSE, "max-buffers", 4u, "drop", FALSE, nullptr);
    GstAppSinkCallbacks cbs{};
    cbs.new_sample = &Producer::on_new_sample;
    gst_app_sink_set_callbacks(GST_APP_SINK(t->sink.get()), &cbs, t.get(), nullptr);
    gst_element_set_name(t->encode.get(), (prefix + "/encode").c_str()); // docs/24 grammar, before it is parented
    gst_bin_add_many(GST_BIN(pipeline_.get()), t->queue.get(), t->encode.get(), t->sink.get(), nullptr);
    if (!gst_element_link_many(t->queue.get(), t->encode.get(), t->sink.get(), nullptr)) {
        error_ = "cannot link tier branch";
        return false;
    }
    // Downstream first, and only then onto the tee: a queue that is PLAYING before the encode
    // bin is READY pushes into an inactive pad, takes FLUSHING and parks its task for good —
    // the whole producer stalls without a bus error (second-tier start on a running pipeline).
    gst_element_sync_state_with_parent(t->sink.get());
    gst_element_sync_state_with_parent(t->encode.get());
    gst_element_sync_state_with_parent(t->queue.get());
    t->tee_pad = glib::adopt_pad(gst_element_request_pad_simple(tee_.get(), "src_%u"));
    glib::GstPadPtr qsink = glib::adopt_pad(gst_element_get_static_pad(t->queue.get(), "sink"));
    if (gst_pad_link(t->tee_pad.get(), qsink.get()) != GST_PAD_LINK_OK) {
        error_ = "cannot link tee to tier";
        return false;
    }
    tiers_[tier] = std::move(t);
    if (!playing_) {
        if (gst_element_set_state(pipeline_.get(), GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
            error_ = "pipeline refused PLAYING";
            return false;
        }
        playing_ = true;
    }
    log::info("producer", "tier started", {{"producer", name()}, {"tier", tier}, {"kbps", std::to_string(profile.kbps)},
                                            {"size", std::to_string(profile.width) + "x" + std::to_string(profile.height)},
                                            {"fps", std::to_string(profile.fps)}});
    return true;
}

void Producer::stop_tier(const std::string& tier) {
    auto it = tiers_.find(tier);
    if (it == tiers_.end()) return;
    Tier* t = it->second.get();
    // Unlink first (the tee stops pushing into this branch), then tear it down: deactivating
    // the queue's pad waits on its stream lock, so no probe is needed to fence the streaming thread.
    glib::GstPadPtr qsink = glib::adopt_pad(gst_element_get_static_pad(t->queue.get(), "sink"));
    gst_pad_unlink(t->tee_pad.get(), qsink.get());
    gst_element_set_state(t->sink.get(), GST_STATE_NULL);
    gst_element_set_state(t->encode.get(), GST_STATE_NULL);
    gst_element_set_state(t->queue.get(), GST_STATE_NULL);
    gst_bin_remove_many(GST_BIN(pipeline_.get()), t->queue.get(), t->encode.get(), t->sink.get(), nullptr);
    gst_element_release_request_pad(tee_.get(), t->tee_pad.get());
    hub_.clear(t->key);
    tiers_.erase(it);
    log::info("producer", "tier stopped", {{"producer", name()}, {"tier", tier}});
    if (tiers_.empty() && playing_) {
        gst_element_set_state(pipeline_.get(), GST_STATE_NULL);
        playing_ = false;
    }
}

void Producer::request_keyframe(const std::string& tier) {
    auto it = tiers_.find(tier);
    if (it == tiers_.end()) return;
    GstEvent* ev = gst_video_event_new_upstream_force_key_unit(GST_CLOCK_TIME_NONE, TRUE, 0);
    gst_element_send_event(it->second->sink.get(), ev); // upstream from the sink into the encoder
}

std::string Producer::source_caps() const {
    if (!source_pad_) return {};
    glib::GstCapsPtr caps(gst_pad_get_current_caps(source_pad_.get()));
    return glib::caps_to_string(caps.get());
}

void Producer::stop() {
    if (!pipeline_) return;
    for (auto& [_, t] : tiers_) {
        GstAppSinkCallbacks none{};
        gst_app_sink_set_callbacks(GST_APP_SINK(t->sink.get()), &none, nullptr, nullptr);
    }
    gst_element_set_state(pipeline_.get(), GST_STATE_NULL);
    playing_ = false;
    for (auto& [_, t] : tiers_) hub_.clear(t->key);
    tiers_.clear();
    bus_watch_.cancel();
    stamp_probe_.remove();
    source_pad_.reset();
    tee_.reset();
    rawcaps_.reset();
    convert_.reset();
    source_bin_.reset();
    pipeline_.reset();
    glib::ObjectCensus::instance().pipelines--;
}

} // namespace fjarr::media
