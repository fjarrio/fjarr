#pragma once
// FrameHub — produce once, fan out: one entry per (track_id, tier) holding
// the retained keyframe, a ring of the last GOP+1 encoded samples and the
// subscribers; delivery on the hub's worker thread with a keyframe gate and
// PTS rebase, never a pixel copy.
// spec: docs/23-agent-core-architecture.md#framehub · #fan-out
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gst/gst.h>

#include "core/glib/raii.hpp"

namespace fjarr::media {

struct HubKey {
    std::string track_id;
    std::string tier; // active | thumbnail
    bool operator<(const HubKey& o) const { return track_id < o.track_id || (track_id == o.track_id && tier < o.tier); }
    bool operator==(const HubKey& o) const { return track_id == o.track_id && tier == o.tier; }
    std::string str() const { return track_id + ":" + tier; }
};

/// A consumer of encoded frames (a session's appsrc). `push` runs on the hub
/// thread and must not block; return false when the frame was not accepted.
class FrameSink {
  public:
    virtual ~FrameSink() = default;
    virtual bool push(glib::GstBufferPtr buffer, GstCaps* caps) = 0;
};

struct SubscriberStats {
    unsigned long delivered = 0;
    unsigned long dropped = 0; // skipped while waiting for a keyframe or overrun
    bool waiting_keyframe = true;
};

struct HubEntryStats {
    unsigned long frames = 0;
    unsigned long keyframes = 0;
    std::size_t ring = 0;
    bool has_keyframe = false;
    int subscribers = 0;
};

class FrameHub {
  public:
    explicit FrameHub(std::size_t ring_size = 61);
    ~FrameHub();
    FrameHub(const FrameHub&) = delete;
    FrameHub& operator=(const FrameHub&) = delete;

    /// Producer side (streaming thread): takes the sample.
    void push(const HubKey& key, glib::GstSamplePtr sample);

    /// Consumer side: subscribe a sink; the first frame it sees is a keyframe.
    void subscribe(const HubKey& key, std::shared_ptr<FrameSink> sink);
    void unsubscribe(const HubKey& key, const std::shared_ptr<FrameSink>& sink);
    int subscriber_count(const HubKey& key) const;
    /// Resync a sink to the next keyframe (select-tracks enable, tier switch).
    void resync(const HubKey& key, const std::shared_ptr<FrameSink>& sink);

    /// The producer's keyframe hook (the media plane rate-limits and defers).
    void on_keyframe_request(std::function<void(const HubKey&)> fn);
    void request_keyframe(const HubKey& key);
    /// Demand changes (subscriber count 0 ↔ >0), for lazy producers.
    void on_demand_changed(std::function<void(const HubKey&, int)> fn);

    HubEntryStats stats(const HubKey& key) const;
    SubscriberStats subscriber_stats(const HubKey& key, const std::shared_ptr<FrameSink>& sink) const;
    std::vector<HubKey> keys() const;
    /// Drop an entry's ring and retained keyframe (producer gone).
    void clear(const HubKey& key);
    std::size_t buffers_held() const;

  private:
    struct Sub {
        std::weak_ptr<FrameSink> sink;
        SubscriberStats stats;
        GstClockTime base_pts = GST_CLOCK_TIME_NONE;
        GstClockTime base_dts = GST_CLOCK_TIME_NONE;
        std::uint64_t last_seq = 0; // a late joiner never sees a ring sample twice (sequence, not address: no ABA)
        std::shared_ptr<FrameSink> lock() const { return sink.lock(); }
    };
    struct RingItem {
        glib::GstSamplePtr sample;
        std::uint64_t seq;
        bool keyframe;
    };
    struct Entry {
        glib::GstSamplePtr retained_keyframe;
        std::deque<RingItem> ring;
        std::vector<Sub> subs;
        HubEntryStats stats;
    };
    struct Work {
        HubKey key;
        glib::GstSamplePtr sample;
        bool keyframe;
        std::uint64_t seq = 0;
        std::shared_ptr<FrameSink> only; // set: catch-up for one late joiner (the ring), not a fan-out
    };
    std::uint64_t next_seq_ = 1;

    void deliver(const Work& w);
    void thread_main();
    static bool is_keyframe(GstSample* s);

    mutable std::mutex mutex_;
    std::map<HubKey, Entry> entries_;
    std::size_t ring_size_;
    std::function<void(const HubKey&)> keyframe_request_;
    std::function<void(const HubKey&, int)> demand_changed_;

    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<Work> queue_;
    bool stop_ = false;
    std::thread thread_;
};

} // namespace fjarr::media
