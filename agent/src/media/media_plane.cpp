#include "media_plane.hpp"

#include <algorithm>

#include "core/log.hpp"

namespace fjarr::media {

MediaPlane::MediaPlane(CoreLoop& loop, const AgentConfig::MediaSection& config, EncoderChoice encoder)
    : loop_(loop), config_(config), encoder_(std::move(encoder)), hub_(static_cast<std::size_t>(config.gop_seconds * 30 + 1)) {
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
            if (auto* d = dynamic_cast<GstDescriptionSource*>(src.get())) *reason = d->last_error();
            else *reason = "source unavailable";
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

void MediaPlane::request_keyframe(const HubKey& key) {
    loop_.assert_owner("MediaPlane::request_keyframe");
    auto it = tracks_.find(key.track_id);
    if (it == tracks_.end() || !it->second.producer) return;
    Registered& r = it->second;
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
    if (r.restarts > 5) {
        log::error("media", "producer restart budget exhausted: plane rebuild", {{"track", track_id}, {"error", error}});
        if (rebuild_needed_) rebuild_needed_("producer:" + track_id + ": " + error);
        return;
    }
    const int delay_ms = std::min(5000, 500 << (r.restarts - 1));
    log::warn("media", "producer restart scheduled", {{"track", track_id}, {"attempt", std::to_string(r.restarts)}, {"delay_ms", std::to_string(delay_ms)}});
    r.producer.reset();
    if (producer_event_) producer_event_(track_id, "producer-restart");
    r.restart_timer = loop_.add_timeout(std::chrono::milliseconds(delay_ms), [this, track_id] {
        auto it2 = tracks_.find(track_id);
        if (it2 == tracks_.end()) return false;
        Registered& r2 = it2->second;
        if (!ensure_producer(r2)) {
            restart_producer(track_id, r2.producer ? r2.producer->error() : "build failed");
            return false;
        }
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
