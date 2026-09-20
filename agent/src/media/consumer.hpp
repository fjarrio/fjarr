#pragma once
// ConsumerPipeline — one session's GstPipeline: one webrtcbin plus, per
// track, appsrc ! queue ! valve ! payloader ! webrtcbin.sink_%u. The only
// code that talks to webrtcbin: offer builder, renegotiation, ICE, DTLS/
// connection state, data channels, get-stats.
// spec: docs/23-agent-core-architecture.md#offer-construction-and-renegotiation · #consumer-pipeline-per-session
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/webrtc/webrtc.h>

#include <fjarr/capability.hpp>
#include <fjarr/session_context.hpp>

#include "core/glib/raii.hpp"
#include "core/protocol.hpp"
#include "frame_hub.hpp"
#include "pad_counter.hpp"

namespace fjarr::media {

struct ChannelParams {
    std::string label;
    bool ordered = true;
    int max_retransmits = -1; // -1 = reliable
};

/// A track branch in the consumer pipeline.
struct ConsumerTrack {
    std::string track_id;
    std::string cap;
    TrackKind kind = TrackKind::Video;
    std::string label;
    std::optional<MonitorInfo> monitor;
    int pt = 96;
    std::uint32_t ssrc = 0;
    std::string mid;  // from the offer SDP
    unsigned mline = 0;
    bool enabled = false;
    std::string tier = "active";
    bool pooled = false; // removed from the manifest, transceiver kept inactive
    glib::GstElementPtr appsrc, queue, valve, payloader;
    glib::GstPadPtr sink_pad; // webrtcbin.sink_%u
    glib::GObjectPtr<GstWebRTCRTPTransceiver> transceiver;
    std::shared_ptr<FrameSink> sink; // the hub subscriber (appsrc pusher)
    std::unique_ptr<PadCounter> pay_counter; // buffers leaving the payloader (introspection + stall diagnostics)
    glib::PadProbe keyunit_probe;            // PLI/FIR from the peer → keyframe request
    HubKey hub_key() const { return HubKey{track_id, tier}; }
    unsigned long frames_pushed = 0;
    unsigned long frames_dropped = 0;
    unsigned long last_frames = 0, last_dropped = 0;
    std::uint64_t last_bytes_sent = 0;
};

struct StatsSample {
    struct Track {
        std::string track_id;
        std::uint64_t bytes_sent = 0;
        std::uint64_t packets_sent = 0;
    };
    std::vector<Track> tracks;
    std::string selected_pair;
};

/// Everything the wrapper reports upward runs on the core loop.
struct ConsumerHooks {
    std::function<void(const std::string& sdp)> on_offer;            // local description set: send it
    std::function<void(unsigned mline, const std::string& cand)> on_ice; // "" = end of candidates
    std::function<void(const std::string& state)> on_connection_state; // new|connecting|connected|disconnected|failed|closed
    std::function<void(const std::string& state)> on_ice_state;
    std::function<void(const std::string& state)> on_dtls_state;
    std::function<void(GstWebRTCDataChannel* dc, const std::string& label)> on_channel_open;
    std::function<void(const std::string& milestone)> on_milestone;
    std::function<void(const std::string& error)> on_error;
    std::function<void()> on_negotiation_needed;
    /** The peer asked for a keyframe (PLI/FIR) on this track. */
    std::function<void(const std::string& track_id)> on_keyframe_request;
};

class ConsumerPipeline {
  public:
    ConsumerPipeline(std::string session_id, GMainContext* ctx, std::function<void(std::function<void()>)> post,
                     ConsumerHooks hooks, int gop_seconds = 2);
    ~ConsumerPipeline();
    ConsumerPipeline(const ConsumerPipeline&) = delete;
    ConsumerPipeline& operator=(const ConsumerPipeline&) = delete;

    /// Build the pipeline to READY, add TURN, create the data channels in
    /// order (docs/08 classes), then add the initial tracks and go PLAYING.
    bool build(const std::string& ice_policy, const std::optional<protocol::TurnCredentials>& turn,
               const std::vector<ChannelParams>& channels);
    /// Add a track branch (initial or by renegotiation). Reuses a pooled transceiver of the same kind.
    ConsumerTrack* add_track(const TrackSpec& spec, const std::string& cap);
    /// Removal the docs/23 way: valve closed first, transceiver inactive, kept in the pool.
    void remove_track(const std::string& track_id);
    ConsumerTrack* track(const std::string& track_id);
    const std::map<std::string, std::unique_ptr<ConsumerTrack>>& tracks() const { return tracks_; }
    std::vector<protocol::ManifestEntry> manifest() const;

    /// Create an offer, set it as local description, parse mids, report via on_offer.
    void create_offer();
    void set_remote_answer(const std::string& sdp);
    void add_ice_candidate(unsigned mline, const std::string& candidate);
    bool offer_in_flight() const { return offer_in_flight_; }
    bool remote_described() const { return remote_described_; }

    /// Valve control.
    void set_enabled(const std::string& track_id, bool enabled);
    /// The hub sink for a track (an appsrc pusher).
    std::shared_ptr<FrameSink> sink_for(const std::string& track_id);

    void get_stats(std::function<void(StatsSample)> cb);
    GstPipeline* pipeline() const { return GST_PIPELINE(pipeline_.get()); }
    GstElement* webrtc() const { return webrtc_.get(); }
    const std::vector<glib::GObjectPtr<GstWebRTCDataChannel>>& channels() const { return channels_; }
    std::string connection_state() const;
    void stop();
    std::string name() const { return "session:" + sid8_; }

  private:
    struct AppSrcSink;
    void connect_signals();
    void on_negotiation_needed_cb();
    static void on_ice_candidate_cb(GstElement*, guint mline, gchar* cand, gpointer user);
    static void on_notify_state(GObject*, GParamSpec* pspec, gpointer user);
    static gboolean on_bus(GstBus* bus, GstMessage* msg, gpointer user);
    void milestone(const std::string& name);
    std::string prefix(const std::string& track_id) const { return name() + "/" + track_id; }
    struct SignalContext;
    bool build_branch(ConsumerTrack& t);
    void teardown_branch(ConsumerTrack& t);
    int gop_seconds_ = 2;

    std::string session_id_;
    std::string sid8_;
    GMainContext* ctx_;
    std::function<void(std::function<void()>)> post_;
    ConsumerHooks hooks_;
    glib::GstElementPtr pipeline_;
    glib::GstElementPtr webrtc_;
    glib::SourceGuard bus_watch_;
    std::vector<glib::SignalConnection> signals_;
    std::vector<glib::GObjectPtr<GstWebRTCDataChannel>> channels_;
    std::map<std::string, std::unique_ptr<ConsumerTrack>> tracks_;
    int next_pt_ = 96;
    bool offer_in_flight_ = false;
    bool remote_described_ = false;
    std::vector<std::pair<unsigned, std::string>> ice_queue_;
    bool stopped_ = false;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

} // namespace fjarr::media
