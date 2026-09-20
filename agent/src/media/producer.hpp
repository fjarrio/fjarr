#pragma once
// Producer — one pipeline per track output: source bin → (stamp) → tee →
// one encode branch per demanded tier → appsink → FrameHub.
// spec: docs/23-agent-core-architecture.md#encoders-and-tiers · #media-plane-recovery
#include <functional>
#include <map>
#include <memory>
#include <string>

#include <gst/app/gstappsink.h>
#include <gst/gst.h>

#include <fjarr/session_context.hpp>

#include "core/glib/raii.hpp"
#include "encoder.hpp"
#include "frame_hub.hpp"
#include "frame_stamp.hpp"

namespace fjarr::media {

struct ProducerConfig {
    EncoderChoice encoder{EncoderKind::Software, "software"};
    int gop_seconds = 2;
    int active_kbps = 4000;
    int thumbnail_kbps = 300;
};

class Producer {
  public:
    Producer(std::string track_id, SourceRef source, bool stamp, ProducerConfig config, FrameHub& hub,
             GMainContext* bus_context);
    ~Producer();
    Producer(const Producer&) = delete;
    Producer& operator=(const Producer&) = delete;

    const std::string& track_id() const { return track_id_; }
    /// Build the pipeline (source bin, tee). Returns false with `error()` set.
    bool build();
    /// Add/remove a tier's encode branch (idempotent).
    bool start_tier(const std::string& tier);
    void stop_tier(const std::string& tier);
    bool has_tier(const std::string& tier) const { return tiers_.count(tier) > 0; }
    std::size_t tier_count() const { return tiers_.size(); }
    bool playing() const { return playing_; }
    void stop();
    /// Force a keyframe on a tier's encoder.
    void request_keyframe(const std::string& tier);
    GstPipeline* pipeline() const { return GST_PIPELINE(pipeline_.get()); }
    const std::string& error() const { return error_; }
    /// True when the last bus error originated inside the source bin: a device or network problem, the source's to recover from, never a plane rebuild.
    bool error_in_source() const { return error_in_source_; }
    /// Bus error hook (core loop).
    void on_error(std::function<void(const std::string&)> fn) { on_error_ = std::move(fn); }
    /// Negotiated caps at the source output (after PLAYING), for /sources.
    std::string source_caps() const;
    std::string name() const { return "producer:" + track_id_; }

  private:
    struct Tier {
        glib::GstElementPtr queue;
        glib::GstElementPtr encode;
        glib::GstElementPtr sink;
        glib::GstPadPtr tee_pad;
        HubKey key;
        FrameHub* hub = nullptr;
    };
    TierProfile profile_for(const std::string& tier) const;
    static GstFlowReturn on_new_sample(GstAppSink* sink, gpointer user);
    static gboolean on_bus(GstBus* bus, GstMessage* msg, gpointer user);

    std::string track_id_;
    SourceRef source_;
    bool stamp_;
    ProducerConfig config_;
    FrameHub& hub_;
    GMainContext* bus_context_;
    glib::GstElementPtr pipeline_;
    glib::GstElementPtr source_bin_;
    glib::GstElementPtr convert_;
    glib::GstElementPtr rawcaps_;
    glib::GstElementPtr tee_;
    glib::GstPadPtr source_pad_;
    glib::PadProbe stamp_probe_;
    glib::PadProbe alloc_probe_; // stamped sources stay on system memory (see build())
    StampPainter painter_;
    glib::SourceGuard bus_watch_;
    std::map<std::string, std::unique_ptr<Tier>> tiers_;
    std::function<void(const std::string&)> on_error_;
    std::string error_;
    bool error_in_source_ = false; // the bus error came from inside the source bin (device/network), not the encode path
    bool playing_ = false;
    int source_width_ = 0, source_height_ = 0, source_fps_ = 30;
};

} // namespace fjarr::media
