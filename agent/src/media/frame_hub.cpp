#include "frame_hub.hpp"

#include <algorithm>

#include "core/log.hpp"

namespace fjarr::media {

FrameHub::FrameHub(std::size_t ring_size) : ring_size_(ring_size), thread_([this] { thread_main(); }) {}

FrameHub::~FrameHub() {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        stop_ = true;
    }
    queue_cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

bool FrameHub::is_keyframe(GstSample* s) {
    GstBuffer* b = gst_sample_get_buffer(s);
    return b && !GST_BUFFER_FLAG_IS_SET(b, GST_BUFFER_FLAG_DELTA_UNIT);
}

void FrameHub::push(const HubKey& key, glib::GstSamplePtr sample) {
    if (!sample) return;
    const bool kf = is_keyframe(sample.get());
    glib::GstSamplePtr for_delivery = glib::ref_sample(sample.get());
    std::uint64_t seq = 0;
    std::shared_ptr<const HubKey> e_key;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Entry& e = entries_[key];
        if (!e.key) e.key = std::make_shared<const HubKey>(key); // interned once: no per-frame string copies (docs/16)
        seq = next_seq_++;
        e.stats.frames++;
        if (kf) {
            e.stats.keyframes++;
            e.retained_keyframe = glib::ref_sample(sample.get());
            e.stats.has_keyframe = true;
            e.ring.clear(); // a new GOP starts at the keyframe
        }
        e.ring.push_back(RingItem{std::move(sample), seq, kf});
        while (e.ring.size() > ring_size_) e.ring.pop_front();
        e.stats.ring = e.ring.size();
        e_key = e.key;
    }
    std::size_t depth = 0;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        queue_.push_back(Work{e_key, std::move(for_delivery), kf, seq, nullptr});
        depth = queue_.size();
    }
    queue_cv_.notify_one();
    if (depth > ring_size_ * 4) log::warn("hub", "delivery queue deep", {{"key", key.str()}, {"depth", std::to_string(depth)}});
}

void FrameHub::thread_main() {
    for (;;) {
        Work w;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [&] { return stop_ || !queue_.empty(); });
            if (stop_) return;
            w = std::move(queue_.front());
            queue_.pop_front();
            // Overrun: a backlog longer than one ring is stale — skip to the next keyframe.
            if (queue_.size() > ring_size_) {
                while (!queue_.empty() && !queue_.front().keyframe) queue_.pop_front();
            }
        }
        deliver(w);
    }
}

void FrameHub::deliver(const Work& w) {
    // `targets_` is reused across deliveries (delivery thread only): the docs/16 hot-path budget
    // allows one GstBuffer header per subscriber per frame and no other per-frame allocation —
    // heaptrack caught this vector being allocated per frame in slice 3c.
    auto& targets = targets_;
    targets.clear();
    GstCaps* caps = gst_sample_get_caps(w.sample.get());
    GstBuffer* src = gst_sample_get_buffer(w.sample.get());
    if (!src) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entries_.find(*w.key);
        if (it == entries_.end()) return;
        Entry& e = it->second;
        for (Sub& s : e.subs) {
            auto sink = s.lock();
            if (!sink) continue;
            if (w.only && sink != w.only) continue; // a late joiner's catch-up: nobody else
            if (w.seq <= s.last_seq) continue;      // already delivered (the ring catch-up ran ahead of the queue)
            if (s.stats.waiting_keyframe) {
                if (!w.keyframe) {
                    s.stats.dropped++;
                    continue;
                }
                s.stats.waiting_keyframe = false;
                if (!GST_CLOCK_TIME_IS_VALID(s.base_pts)) {
                    // docs/23: the first delivered keyframe's PTS becomes 0 — once; a resync keeps the timeline monotonic.
                    s.base_pts = GST_BUFFER_PTS(src);
                    s.base_dts = GST_BUFFER_DTS_IS_VALID(src) ? GST_BUFFER_DTS(src) : GST_BUFFER_PTS(src);
                }
            }
            s.last_seq = w.seq;
            // Metadata-only copy: memory is shared by reference (docs/23 fan-out: no pixel copy, ever).
            GstBuffer* copy = gst_buffer_copy_region(src, GST_BUFFER_COPY_ALL, 0, static_cast<gsize>(-1));
            if (GST_BUFFER_PTS_IS_VALID(copy) && GST_CLOCK_TIME_IS_VALID(s.base_pts))
                GST_BUFFER_PTS(copy) = GST_BUFFER_PTS(copy) >= s.base_pts ? GST_BUFFER_PTS(copy) - s.base_pts : 0;
            if (GST_BUFFER_DTS_IS_VALID(copy) && GST_CLOCK_TIME_IS_VALID(s.base_dts))
                GST_BUFFER_DTS(copy) = GST_BUFFER_DTS(copy) >= s.base_dts ? GST_BUFFER_DTS(copy) - s.base_dts : 0;
            targets.push_back(Target{std::move(sink), glib::adopt_buffer(copy), caps});
        }
    }
    for (auto& t : targets) {
        const bool ok = t.sink->push(std::move(t.buffer), t.caps);
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entries_.find(*w.key);
        if (it == entries_.end()) continue;
        // By identity: subscribe/unsubscribe may have moved entries while the lock was dropped.
        for (Sub& s : it->second.subs) {
            if (s.lock() != t.sink) continue;
            if (ok) s.stats.delivered++;
            else {
                s.stats.dropped++;
                s.stats.waiting_keyframe = true; // a slow consumer resyncs at the next keyframe
            }
            break;
        }
    }
    targets.clear(); // capacity stays; the sinks (and their appsrc refs) must not outlive their sessions
}

void FrameHub::subscribe(const HubKey& key, std::shared_ptr<FrameSink> sink) {
    int count = 0;
    std::vector<Work> catch_up;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Entry& e = entries_[key];
        e.subs.erase(std::remove_if(e.subs.begin(), e.subs.end(), [](const Sub& s) { return s.sink.expired(); }), e.subs.end());
        e.subs.push_back(Sub{sink, {}, GST_CLOCK_TIME_NONE, GST_CLOCK_TIME_NONE});
        count = e.stats.subscribers = static_cast<int>(e.subs.size());
        // Late joiner: the ring is the current GOP — keyframe first, then its deltas — and every
        // frame of it is decodable in order (docs/23). A ring that lost its keyframe to overflow
        // is not a catch-up; the joiner then waits for the requested keyframe.
        if (!e.key) e.key = std::make_shared<const HubKey>(key);
        if (!e.ring.empty() && e.ring.front().keyframe)
            for (const auto& item : e.ring) catch_up.push_back(Work{e.key, glib::ref_sample(item.sample.get()), item.keyframe, item.seq, sink});
    }
    if (demand_changed_ && count == 1) demand_changed_(key, count);
    if (!catch_up.empty()) {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        queue_.insert(queue_.begin(), std::make_move_iterator(catch_up.begin()), std::make_move_iterator(catch_up.end()));
        queue_cv_.notify_one();
    }
    // The producer is asked for a fresh keyframe so the joiner's first GOP is short.
    request_keyframe(key);
}

void FrameHub::unsubscribe(const HubKey& key, const std::shared_ptr<FrameSink>& sink) {
    int count = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entries_.find(key);
        if (it == entries_.end()) return;
        auto& subs = it->second.subs;
        subs.erase(std::remove_if(subs.begin(), subs.end(), [&](const Sub& s) { return s.sink.expired() || s.lock() == sink; }), subs.end());
        count = it->second.stats.subscribers = static_cast<int>(subs.size());
    }
    if (demand_changed_ && count == 0) demand_changed_(key, 0);
}

void FrameHub::resync(const HubKey& key, const std::shared_ptr<FrameSink>& sink) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entries_.find(key);
        if (it == entries_.end()) return;
        for (auto& s : it->second.subs)
            if (s.lock() == sink) s.stats.waiting_keyframe = true;
    }
    request_keyframe(key);
}

int FrameHub::subscriber_count(const HubKey& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(key);
    return it == entries_.end() ? 0 : it->second.stats.subscribers;
}

void FrameHub::on_keyframe_request(std::function<void(const HubKey&)> fn) { keyframe_request_ = std::move(fn); }
void FrameHub::on_demand_changed(std::function<void(const HubKey&, int)> fn) { demand_changed_ = std::move(fn); }

void FrameHub::request_keyframe(const HubKey& key) {
    // Rate limiting (≥ 1 s per producer, docs/23) lives in the media plane, which can defer
    // a request to the end of its window instead of dropping it — a waiting subscriber must
    // never sit out a whole GOP.
    if (keyframe_request_) keyframe_request_(key);
}

HubEntryStats FrameHub::stats(const HubKey& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(key);
    return it == entries_.end() ? HubEntryStats{} : it->second.stats;
}

SubscriberStats FrameHub::subscriber_stats(const HubKey& key, const std::shared_ptr<FrameSink>& sink) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(key);
    if (it == entries_.end()) return {};
    for (const auto& s : it->second.subs)
        if (s.lock() == sink) return s.stats;
    return {};
}

std::vector<HubKey> FrameHub::keys() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<HubKey> out;
    for (const auto& [k, _] : entries_) out.push_back(k);
    return out;
}

void FrameHub::clear(const HubKey& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(key);
    if (it == entries_.end()) return;
    it->second.ring.clear();
    it->second.retained_keyframe.reset();
    it->second.stats.has_keyframe = false;
    it->second.stats.ring = 0;
    for (auto& s : it->second.subs) s.stats.waiting_keyframe = true;
}

std::size_t FrameHub::buffers_held() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t n = 0;
    for (const auto& [_, e] : entries_) n += e.ring.size() + (e.retained_keyframe ? 1 : 0);
    return n;
}

} // namespace fjarr::media
