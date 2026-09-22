#pragma once
// MediaPlane — the restartable media subsystem: FrameHub, source registry,
// lazy producers per (track, tier) with a grace period, and the recovery
// ladder (producer restart → plane rebuild → exit 2).
// spec: docs/23-agent-core-architecture.md#media-plane · #media-plane-recovery
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <fjarr/agent.hpp>

#include "core/loop.hpp"
#include "encoder.hpp"
#include "frame_hub.hpp"
#include "producer.hpp"
#include "sources.hpp"

namespace fjarr::media {

struct TrackRegistration {
    TrackSpec spec;
    std::string cap;
    bool stamp = false;
};

struct SourceStatus {
    std::string track_id;
    std::string cap;
    std::string identity;
    bool available = true;
    std::string reason;
    std::string caps; // negotiated, when a producer runs
    std::vector<std::string> tiers;
    bool playing = false;
};

class MediaPlane {
  public:
    MediaPlane(CoreLoop& loop, const AgentConfig::MediaSection& config, EncoderChoice encoder, SourceRegistry& sources);
    ~MediaPlane();

    FrameHub& hub() { return hub_; }
    SourceRegistry& sources() { return sources_; }
    const EncoderChoice& encoder() const { return encoder_; }

    /// Declare a track's source (from a session attach). Idempotent per track_id.
    void register_track(const TrackRegistration& reg);
    void unregister_track(const std::string& track_id);
    bool track_available(const std::string& track_id, std::string* reason = nullptr) const;

    /// Demand hooks from the hub: start/stop producers with grace.
    void on_demand(const HubKey& key, int subscribers);
    void request_keyframe(const HubKey& key);

    std::vector<SourceStatus> source_status() const;
    std::vector<Producer*> producers() const;
    Producer* producer(const std::string& track_id) const;

    // Rate control (docs/23#rate-control-and-tier-switching). A session reports, per subscribed
    // (track, tier), the share of its peer's estimate this track may use; every 500 ms each running
    // tier encoder is set to the minimum over its subscribers, clamped to the tier's band. A
    // subscriber is identified by an opaque pointer (its hub sink) and forgotten on unsubscribe.
    void report_allotment(const std::string& track_id, const std::string& tier, const void* subscriber, double bps);
    void forget_allotment(const void* subscriber);
    /// Can this track serve `tier` for a demoted viewer? (a lower tier exists; passthrough without a substream says no)
    bool tier_possible(const std::string& track_id, const std::string& tier) const;
    /// False when nothing about this track can follow a viewer's link: a passthrough stream the
    /// agent cannot re-encode, with no lower tier to move to (docs/16, `bandwidth-stats.adaptive`).
    bool adaptive(const std::string& track_id) const;
    /// The band a tier adapts within, for the session's demotion rule.
    std::pair<int, int> band_kbps(const std::string& track_id, const std::string& tier) const;

    /// Plane rebuild escalation: the caller closes sessions and calls rebuild().
    void on_rebuild_needed(std::function<void(const std::string& reason)> fn) { rebuild_needed_ = std::move(fn); }
    void rebuild();
    int rebuilds_in_window() const;
    /// Snapshot trigger (introspection).
    void on_producer_event(std::function<void(const std::string& track_id, const std::string& event)> fn) { producer_event_ = std::move(fn); }

  private:
    struct Registered {
        int refs = 0; // sessions holding this registration: the last one out unregisters (a re-connect shares the id)
        TrackRegistration reg;
        std::unique_ptr<Producer> producer;
        int restarts = 0;
        std::chrono::steady_clock::time_point last_error{};
        std::string source_error; // the source failed (device gone, camera unreachable): retried slowly, never a plane rebuild
        glib::SourceGuard restart_timer;
        std::map<std::string, glib::SourceGuard> grace_timers; // tier → stop timer
        std::map<std::string, std::chrono::steady_clock::time_point> last_keyframe; // tier → last request
        std::map<std::string, glib::SourceGuard> keyframe_timers;                 // tier → deferred request
    };
    bool ensure_producer(Registered& r);
    void restart_producer(const std::string& track_id, const std::string& error);
    void apply_targets();

    CoreLoop& loop_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true); // guards every post that could outlive the plane
    AgentConfig::MediaSection config_;
    EncoderChoice encoder_;
    FrameHub hub_;
    SourceRegistry& sources_;
    std::map<std::string, Registered> tracks_;
    std::function<void(const std::string&)> rebuild_needed_;
    std::function<void(const std::string&, const std::string&)> producer_event_;
    std::vector<std::chrono::steady_clock::time_point> rebuilds_;
    struct Allotment {
        double bps = 0;
        std::chrono::steady_clock::time_point at{};
    };
    std::map<std::pair<std::string, std::string>, std::map<const void*, Allotment>> allotments_; // (track, tier) → subscriber → share
    glib::SourceGuard rate_timer_;
};

} // namespace fjarr::media
