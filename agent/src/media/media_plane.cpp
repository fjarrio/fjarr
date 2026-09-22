#include "media_plane.hpp"

#include <algorithm>

#include "core/log.hpp"

namespace fjarr::media {

MediaPlane::MediaPlane(CoreLoop& loop, const AgentConfig::MediaSection& config, EncoderChoice encoder, SourceRegistry& sources)
    : loop_(loop), config_(config), encoder_(std::move(encoder)), hub_(static_cast<std::size_t>(config.gop_seconds * 30 + 1)), sources_(sources) {
    // Hub hooks fire on the delivery thread; the plane may be gone by the time the post runs (shutdown).
    hub_.on_keyframe_request([this, alive = std::weak_ptr<bool>(alive_)](const HubKey& key) {
        loop_.post_guarded([alive] { return !alive.expired(); }, [this, key] { request_keyframe(key); });
    });
    hub_.on_demand_changed([this, alive = std::weak_ptr<bool>(alive_)](const HubKey& key, int n) {
        loop_.post_guarded([alive] { return !alive.expired(); }, [this, key, n] { on_demand(key, n); });
    });
}

MediaPlane::~MediaPlane() = default;

void MediaPlane::register_track(const TrackRegistration& reg) {
    loop_.assert_owner("MediaPlane::register_track");
    auto it = tracks_.find(reg.spec.track_id);
    if (it != tracks_.end()) {
        it->second.reg = reg; // a capability may re-declare (new label, same source)
        it->second.refs++;
        return;
    }
    Registered r;
    r.reg = reg;
    r.refs = 1;
    tracks_[reg.spec.track_id] = std::move(r);
}

void MediaPlane::unregister_track(const std::string& track_id) {
    loop_.assert_owner("MediaPlane::unregister_track");
    // Counted: a session that reconnects registers the same id before the old one's deferred
    // close unregisters it (found by fjarr-opsim ice-restart).
    auto it = tracks_.find(track_id);
    if (it == tracks_.end()) return;
    if (--it->second.refs <= 0) tracks_.erase(it);
}

bool MediaPlane::track_available(const std::string& track_id, std::string* reason) const {
    auto it = tracks_.find(track_id);
    if (it == tracks_.end()) {
        if (reason) *reason = "unknown track";
        return false;
    }
    const auto& src = it->second.reg.spec.source.source;
    if (!src) {
        if (reason) *reason = "no source";
        return false;
    }
    if (!src->available()) {
        if (reason) {
            *reason = src->unavailable_reason();
            if (reason->empty()) *reason = "source unavailable";
        }
        return false;
    }
    return true;
}

bool MediaPlane::ensure_producer(Registered& r) {
    if (r.producer && r.producer->pipeline()) return true;
    ProducerConfig pc;
    pc.encoder = encoder_;
    pc.gop_seconds = config_.gop_seconds;
    pc.active_kbps = config_.active_kbps;
    pc.active_floor_kbps = config_.active_floor_kbps;
    pc.thumbnail_kbps = config_.thumbnail_kbps;
    auto p = std::make_unique<Producer>(r.reg.spec.track_id, r.reg.spec.source, r.reg.stamp, pc, hub_, loop_.context());
    const std::string id = r.reg.spec.track_id;
    // Posted, never inline: restart_producer destroys the Producer that is emitting this callback
    // (its bus watch is mid-dispatch), so the closure and its captures must not be running.
    p->on_error([this, id, alive = std::weak_ptr<bool>(alive_)](const std::string& err) {
        loop_.post_guarded([alive] { return !alive.expired(); }, [this, id, err] { restart_producer(id, err); });
    });
    if (!p->build()) {
        log::error("media", "producer build failed", {{"track", id}, {"error", p->error()}});
        return false;
    }
    r.producer = std::move(p);
    return true;
}

void MediaPlane::on_demand(const HubKey& key, int subscribers) {
    loop_.assert_owner("MediaPlane::on_demand");
    auto it = tracks_.find(key.track_id);
    if (it == tracks_.end()) return;
    Registered& r = it->second;
    if (subscribers > 0) {
        r.grace_timers.erase(key.tier);
        if (!ensure_producer(r)) return;
        if (!r.producer->start_tier(key.tier)) {
            log::error("media", "tier start failed", {{"track", key.track_id}, {"tier", key.tier}, {"error", r.producer->error()}});
            restart_producer(key.track_id, r.producer->error());
            return;
        }
        if (producer_event_) producer_event_(key.track_id, "state-changed");
        return;
    }
    // Zero demand: linger for the grace period, then stop the tier.
    const std::string tier = key.tier;
    const std::string id = key.track_id;
    r.grace_timers[tier] = loop_.add_timeout(std::chrono::milliseconds(config_.tier_grace_ms), [this, id, tier] {
        auto it2 = tracks_.find(id);
        if (it2 == tracks_.end() || !it2->second.producer) return false;
        if (hub_.subscriber_count(HubKey{id, tier}) > 0) return false;
        it2->second.producer->stop_tier(tier);
        if (producer_event_) producer_event_(id, "state-changed");
        it2->second.grace_timers.erase(tier);
        return false;
    });
}

void MediaPlane::report_allotment(const std::string& track_id, const std::string& tier, const void* subscriber, double bps) {
    loop_.assert_owner("MediaPlane::report_allotment");
    allotments_[{track_id, tier}][subscriber] = Allotment{bps, std::chrono::steady_clock::now()};
    if (!rate_timer_.active()) {
        rate_timer_ = loop_.add_timeout(std::chrono::milliseconds(500), [this] {
            apply_targets();
            if (allotments_.empty()) return false; // nobody reports: the timer ends
            return true;
        });
    }
}

void MediaPlane::forget_allotment(const void* subscriber) {
    for (auto it = allotments_.begin(); it != allotments_.end();) {
        it->second.erase(subscriber);
        it = it->second.empty() ? allotments_.erase(it) : std::next(it);
    }
}

namespace {
/// What the track's source says about itself, before or without a producer (docs/06 passthrough):
/// is the demanded output an elementary stream, and is there a substream to demote a viewer to?
struct SourceShape {
    bool elementary = false;
    bool substream = false;
};
SourceShape shape_of(const SourceRef& src) {
    SourceShape s;
    if (!src.source) return s;
    for (const auto& o : src.source->describe().outputs) {
        if (o.name == src.output) s.elementary = o.declared_caps.rfind("video/x-h26", 0) == 0;
        if (o.name == "thumbnail") s.substream = true;
    }
    return s;
}
} // namespace

bool MediaPlane::tier_possible(const std::string& track_id, const std::string& tier) const {
    auto it = tracks_.find(track_id);
    if (it == tracks_.end() || (tier != "active" && tier != "thumbnail")) return false;
    if (it->second.producer) return it->second.producer->tier_possible(tier);
    const SourceShape s = shape_of(it->second.reg.spec.source);
    return !s.elementary || tier == "active" || s.substream;
}

bool MediaPlane::adaptive(const std::string& track_id) const {
    auto it = tracks_.find(track_id);
    if (it == tracks_.end()) return true;
    const bool passthrough = it->second.producer ? it->second.producer->passthrough() : shape_of(it->second.reg.spec.source).elementary;
    return !passthrough || tier_possible(track_id, "thumbnail");
}

std::pair<int, int> MediaPlane::band_kbps(const std::string& track_id, const std::string& tier) const {
    auto it = tracks_.find(track_id);
    if (it != tracks_.end() && it->second.producer) return it->second.producer->band_kbps(tier);
    if (tier == "thumbnail") return {std::min(config_.active_floor_kbps, config_.thumbnail_kbps), config_.thumbnail_kbps};
    return {std::max(config_.active_floor_kbps, config_.active_kbps / 2), config_.active_kbps};
}

void MediaPlane::apply_targets() {
    // docs/23: a tier encoder follows the *minimum* estimate among its current subscribers, inside
    // the tier's band; stale reports (a session that stopped reporting) drop out after 3 s.
    const auto now = std::chrono::steady_clock::now();
    for (auto it = allotments_.begin(); it != allotments_.end();) {
        auto& [key, subs] = *it;
        for (auto s = subs.begin(); s != subs.end();) s = now - s->second.at > std::chrono::seconds(3) ? subs.erase(s) : std::next(s);
        if (subs.empty()) {
            it = allotments_.erase(it);
            continue;
        }
        auto t = tracks_.find(key.first);
        if (t != tracks_.end() && t->second.producer && t->second.producer->has_tier(key.second)) {
            double min_bps = 0;
            for (const auto& [_, a] : subs) min_bps = min_bps == 0 ? a.bps : std::min(min_bps, a.bps);
            const auto band = t->second.producer->band_kbps(key.second);
            // docs/23: the band protects the *other* viewers of a shared encoder; a lone viewer gets
            // the whole range down to the floor (a tier switch takes 2 s — a lone viewer on a slow
            // link must not overload it meanwhile).
            const double low = subs.size() == 1 ? std::min<double>(band.first, config_.active_floor_kbps) : band.first;
            const int kbps = static_cast<int>(std::min<double>(band.second, std::max<double>(low, min_bps / 1000.0)));
            t->second.producer->set_bitrate(key.second, kbps);
        }
        ++it;
    }
}

void MediaPlane::request_keyframe(const HubKey& key) {
    loop_.assert_owner("MediaPlane::request_keyframe");
    auto it = tracks_.find(key.track_id);
    if (it == tracks_.end() || !it->second.producer) return;
    Registered& r = it->second;
    log::debug("media", "keyframe request", {{"track", key.track_id}, {"tier", key.tier}});
    const auto now = std::chrono::steady_clock::now();
    const auto last = r.last_keyframe.find(key.tier);
    const auto window = std::chrono::seconds(1); // ≥ 1 s apart per producer tier (docs/23)
    if (last != r.last_keyframe.end() && now - last->second < window) {
        if (r.keyframe_timers.count(key.tier)) return; // one deferred request is enough
        const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(window - (now - last->second)) + std::chrono::milliseconds(1);
        const std::string id = key.track_id, tier = key.tier;
        r.keyframe_timers[key.tier] = loop_.add_timeout(wait, [this, id, tier] {
            auto it2 = tracks_.find(id);
            if (it2 == tracks_.end()) return false;
            it2->second.keyframe_timers.erase(tier);
            request_keyframe(HubKey{id, tier});
            return false;
        });
        return;
    }
    r.last_keyframe[key.tier] = now;
    r.producer->request_keyframe(key.tier);
}

void MediaPlane::restart_producer(const std::string& track_id, const std::string& error) {
    auto it = tracks_.find(track_id);
    if (it == tracks_.end()) return;
    Registered& r = it->second;
    const auto now = std::chrono::steady_clock::now();
    if (now - r.last_error > std::chrono::minutes(1)) r.restarts = 0;
    r.last_error = now;
    r.restarts++;
    // docs/23 media-plane recovery: an error inside the source bin (a camera unplugged or
    // unreachable) is that track's problem — it is retried slowly and reported as the track's
    // reason, and it never takes the plane (and every session) down. The ladder to a plane
    // rebuild is for the encode path.
    const bool source_failure = (r.producer && r.producer->error_in_source()) || !track_available(track_id);
    std::string reason;
    if (!track_available(track_id, &reason)) {
        r.source_error = reason.empty() ? error : reason;
    } else if (source_failure) {
        r.source_error = error;
    }
    if (!source_failure && r.restarts > 5) {
        log::error("media", "producer restart budget exhausted: plane rebuild", {{"track", track_id}, {"error", error}});
        if (rebuild_needed_) rebuild_needed_("producer:" + track_id + ": " + error);
        return;
    }
    const int delay_ms = source_failure ? std::min(30000, 1000 << std::min(r.restarts - 1, 5)) : std::min(5000, 500 << (r.restarts - 1));
    log::warn("media", source_failure ? "source failed: producer retry scheduled" : "producer restart scheduled",
              {{"track", track_id}, {"attempt", std::to_string(r.restarts)}, {"delay_ms", std::to_string(delay_ms)}, {"error", error}});
    r.producer.reset();
    if (producer_event_) producer_event_(track_id, source_failure ? "source-failed" : "producer-restart");
    r.restart_timer = loop_.add_timeout(std::chrono::milliseconds(delay_ms), [this, track_id] {
        auto it2 = tracks_.find(track_id);
        if (it2 == tracks_.end()) return false;
        Registered& r2 = it2->second;
        bool demanded = false;
        for (const char* t : {"active", "thumbnail"}) demanded = demanded || hub_.subscriber_count(HubKey{track_id, t}) > 0;
        if (!demanded) return false; // nobody is waiting: the next subscription builds it (and a source that is back is tried then)
        if (!track_available(track_id, &r2.source_error)) {
            restart_producer(track_id, r2.source_error); // parked on the slow ladder until the device is back
            return false;
        }
        if (!ensure_producer(r2)) {
            restart_producer(track_id, r2.producer ? r2.producer->error() : "build failed");
            return false;
        }
        r2.source_error.clear();
        // Every tier with demand, not only those that were running: a tier whose start failed
        // before the error still has its subscribers waiting (demand only fires on 0↔1).
        for (const char* t : {"active", "thumbnail"})
            if (hub_.subscriber_count(HubKey{track_id, t}) > 0) r2.producer->start_tier(t);
        return false;
    });
}

void MediaPlane::rebuild() {
    loop_.assert_owner("MediaPlane::rebuild");
    const auto now = std::chrono::steady_clock::now();
    rebuilds_.push_back(now);
    rebuilds_.erase(std::remove_if(rebuilds_.begin(), rebuilds_.end(), [&](auto t) { return now - t > std::chrono::minutes(10); }), rebuilds_.end());
    for (auto& [_, r] : tracks_) {
        r.producer.reset();
        r.restarts = 0;
        r.grace_timers.clear();
        r.restart_timer.cancel();
    }
    log::warn("media", "media plane rebuilt", {{"rebuilds_10min", std::to_string(rebuilds_.size())}});
    if (producer_event_) producer_event_("", "plane-rebuild");
}

int MediaPlane::rebuilds_in_window() const { return static_cast<int>(rebuilds_.size()); }

std::vector<SourceStatus> MediaPlane::source_status() const {
    std::vector<SourceStatus> out;
    for (const auto& [id, r] : tracks_) {
        SourceStatus s;
        s.track_id = id;
        s.cap = r.reg.cap;
        if (r.reg.spec.source.source) s.identity = r.reg.spec.source.source->describe().identity;
        s.available = track_available(id, &s.reason);
        if (s.available && !r.source_error.empty() && !(r.producer && r.producer->pipeline())) {
            s.available = false; // the device is there but its stream failed (an unreachable camera): the reason is the bus error
            s.reason = r.source_error;
        }
        if (r.producer) {
            s.caps = r.producer->source_caps();
            s.playing = r.producer->playing();
            for (const char* t : {"active", "thumbnail"})
                if (r.producer->has_tier(t)) s.tiers.emplace_back(t);
        }
        out.push_back(std::move(s));
    }
    return out;
}

std::vector<Producer*> MediaPlane::producers() const {
    std::vector<Producer*> out;
    for (const auto& [_, r] : tracks_)
        if (r.producer && r.producer->pipeline()) out.push_back(r.producer.get());
    return out;
}

Producer* MediaPlane::producer(const std::string& track_id) const {
    auto it = tracks_.find(track_id);
    return it == tracks_.end() ? nullptr : it->second.producer.get();
}

} // namespace fjarr::media
