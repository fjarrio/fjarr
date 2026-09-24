#include "session.hpp"

#include <malloc.h>

#include <algorithm>

#include <fjarr/blob.hpp>
#include <fjarr/errors.hpp>

#include "log.hpp"

namespace fjarr::core {

using namespace std::chrono;

namespace {
constexpr milliseconds LIVENESS_BUDGET{15000};
constexpr milliseconds NEGOTIATION_WATCHDOG{15000};
constexpr std::size_t REALTIME_DROP_THRESHOLD = 64 * 1024;
constexpr std::size_t HIGH_WATER = 4 * 1024 * 1024;
constexpr std::size_t LOW_WATER = 1 * 1024 * 1024;
constexpr std::size_t SCTP_MAX_MESSAGE = 65536;

DetachReason detach_reason_for(const std::string& reason) {
    if (reason == "peer-gone" || reason == "operator-disconnected") return DetachReason::PeerGone;
    if (reason == "heartbeat") return DetachReason::Heartbeat;
    if (reason == "operator-closed" || reason == "agent-shutdown" || reason == "ice-restart" || reason == "media-restart")
        return DetachReason::Closed;
    return DetachReason::Error;
}

void dc_send_text(GstWebRTCDataChannel* dc, const std::string& text) {
    GError* err = nullptr;
    if (!gst_webrtc_data_channel_send_string_full(dc, text.c_str(), &err)) {
        glib::GErrorPtr e(err);
        log::warn("router", "send failed", {{"error", err ? err->message : "?"}});
    }
}

std::size_t dc_buffered(GstWebRTCDataChannel* dc) {
    guint64 v = 0;
    g_object_get(dc, "buffered-amount", &v, nullptr);
    return static_cast<std::size_t>(v);
}
} // namespace

// ------------------------------------------------------------- senders

struct Session::ControlSender final : ChannelSender {
    GstWebRTCDataChannel* dc = nullptr; // owned by Session::channels_
    std::function<void()> drain;
    void send(const Envelope& msg) override {
        if (!dc) throw FjarrError(std::string(error_codes::internal), "control channel not open");
        dc_send_text(dc, protocol::serialize_envelope(msg));
    }
    bool send_binary(std::span<const std::byte>) override { return false; }
    std::size_t buffered_amount() const override { return dc ? dc_buffered(dc) : 0; }
    void on_drain(std::function<void()> fn) override { drain = std::move(fn); }
};

struct Session::RealtimeSender final : ChannelSender {
    GstWebRTCDataChannel* dc = nullptr;
    unsigned long dropped = 0;
    void send(const Envelope& msg) override {
        if (!dc) return; // lossy by design
        if (dc_buffered(dc) > REALTIME_DROP_THRESHOLD) {
            dropped++;
            return; // never a backlog on a lossy channel (docs/23)
        }
        dc_send_text(dc, protocol::serialize_envelope(msg));
    }
    bool send_binary(std::span<const std::byte>) override { return false; }
    std::size_t buffered_amount() const override { return dc ? dc_buffered(dc) : 0; }
    void on_drain(std::function<void()>) override {}
};

struct Session::BulkSender final : ChannelSender {
    GstWebRTCDataChannel* dc = nullptr;
    std::function<void()> drain;
    void send(const Envelope&) override { throw FjarrError("payload-invalid", "envelopes do not ride bulk channels (docs/08)"); }
    bool send_binary(std::span<const std::byte> frame) override {
        if (!dc) return false;
        if (frame.size() > SCTP_MAX_MESSAGE) throw FjarrError("payload-invalid", "frame exceeds the SCTP max-message-size; chunk it (docs/08)");
        if (dc_buffered(dc) >= HIGH_WATER) return false;
        glib::GBytesPtr bytes(g_bytes_new(frame.data(), frame.size()));
        GError* err = nullptr;
        if (!gst_webrtc_data_channel_send_data_full(dc, bytes.get(), &err)) {
            glib::GErrorPtr e(err);
            return false;
        }
        return true;
    }
    std::size_t buffered_amount() const override { return dc ? dc_buffered(dc) : 0; }
    void on_drain(std::function<void()> fn) override { drain = std::move(fn); }
};

struct Session::DeniedSender final : ChannelSender {
    std::string why;
    void send(const Envelope&) override { throw FjarrError(std::string(error_codes::capability_denied), why); }
    bool send_binary(std::span<const std::byte>) override { return false; }
    std::size_t buffered_amount() const override { return 0; }
    void on_drain(std::function<void()>) override {}
};

// ------------------------------------------------------------- deadman

struct Session::Deadman final : DeadmanHandle, std::enable_shared_from_this<Session::Deadman> {
    CoreLoop* loop;
    milliseconds budget;
    std::function<void()> on_expiry;
    glib::SourceGuard timer;
    steady_clock::time_point last_feed = steady_clock::now();
    bool expired_ = false;
    void arm() {
        timer = loop->add_timeout(budget, [this] {
            expire();
            return false;
        });
    }
    void expire() {
        timer.cancel();
        if (expired_) return;
        expired_ = true;
        if (on_expiry) on_expiry();
    }
    void feed() override {
        last_feed = steady_clock::now();
        expired_ = false;
        arm();
    }
    bool expired() const override { return expired_; }
    milliseconds since_feed() const override { return duration_cast<milliseconds>(steady_clock::now() - last_feed); }
};

/// The public handle wraps the shared deadman so the session can expire it on end.
struct DeadmanProxy final : DeadmanHandle {
    std::shared_ptr<Session::Deadman> d;
    ~DeadmanProxy() override { d->timer.cancel(); }
    void feed() override { d->feed(); }
    bool expired() const override { return d->expired(); }
    milliseconds since_feed() const override { return d->since_feed(); }
};

// ------------------------------------------------------- context impl

SessionContextImpl::SessionContextImpl(Session& session, std::string cap) : session_(session), cap_(std::move(cap)) {}
const SessionId& SessionContextImpl::id() const { return session_.id(); }
const OperatorInfo& SessionContextImpl::operator_info() const { return session_.operator_info(); }
const nlohmann::json& SessionContextImpl::granted_params(std::string_view cap) const { return session_.granted_params(cap); }
void SessionContextImpl::add_track(TrackSpec spec) { session_.add_track(cap_, std::move(spec)); }
void SessionContextImpl::update_tracks(std::vector<TrackSpec> full_set) { session_.update_tracks(cap_, std::move(full_set)); }
TrackState SessionContextImpl::track_state(std::string_view track_id) const { return session_.track_state(track_id); }
unsigned SessionContextImpl::manifest_version() const { return session_.manifest_version(); }
ChannelSender& SessionContextImpl::control() { return session_.sender(ChannelClass::Control, cap_); }
ChannelSender& SessionContextImpl::realtime() { return session_.sender(ChannelClass::Realtime, cap_); }
ChannelSender& SessionContextImpl::bulk() { return session_.sender(ChannelClass::Bulk, cap_); }
ChannelSender& SessionContextImpl::stream() { return session_.sender(ChannelClass::Stream, cap_); }
void SessionContextImpl::accept(const Envelope& request) {
    if (request.kind != "request") throw FjarrError("payload-invalid", "accept() on a non-request envelope");
    session_.send_control(protocol::make_envelope(request.cap, request.type, "accept", nlohmann::json::object(), request.event_id));
}
void SessionContextImpl::feedback(const Envelope& request, nlohmann::json payload) {
    if (request.kind != "request") throw FjarrError("payload-invalid", "feedback() on a non-request envelope");
    session_.send_control(protocol::make_envelope(request.cap, request.type, "feedback", std::move(payload), request.event_id));
}
void SessionContextImpl::result(const Envelope& request, nlohmann::json payload) {
    if (request.kind != "request") throw FjarrError("payload-invalid", "result() on a non-request envelope");
    if (!payload.is_object() || !payload.contains("ok")) throw FjarrError("payload-invalid", "result payload needs `ok`");
    session_.send_control(protocol::make_envelope(request.cap, request.type, "result", std::move(payload), request.event_id));
}
void SessionContextImpl::fail(const Envelope& request, std::string_view code, std::string_view message) {
    if (request.kind != "request") throw FjarrError("payload-invalid", "fail() on a non-request envelope");
    session_.send_control(protocol::make_envelope(request.cap, request.type, "result",
                                                  nlohmann::json{{"ok", false}, {"error", {{"code", std::string(code)}, {"message", std::string(message)}}}},
                                                  request.event_id));
}
blob::BlobRef SessionContextImpl::send_blob(std::string bytes, std::string media_type, std::function<void(bool)> done) {
    return session_.send_blob(cap_, std::move(bytes), std::move(media_type), std::move(done));
}
void SessionContextImpl::cancel_blob(std::string_view blob_id) { session_.cancel_blob(blob_id); }
void SessionContextImpl::event(std::string_view type, nlohmann::json payload) {
    session_.send_control(protocol::make_envelope(cap_, std::string(type), "event", std::move(payload)));
}
void SessionContextImpl::run_async(std::function<void()> job, std::function<void()> done) { session_.run_async(std::move(job), std::move(done)); }
std::unique_ptr<DeadmanHandle> SessionContextImpl::arm_deadman(milliseconds budget, std::function<void()> on_expiry) {
    return session_.arm_deadman(budget, std::move(on_expiry));
}
void SessionContextImpl::close(std::string_view reason) { session_.close(std::string(reason)); }
void SessionContextImpl::test_silence(bool pings, bool media, milliseconds ms) { session_.test_silence(pings, media, ms); }

// -------------------------------------------------------------- session

const char* Session::state_name(State s) {
    switch (s) {
    case State::Requested: return "requested";
    case State::Building: return "building";
    case State::Offered: return "offered";
    case State::Connected: return "connected";
    case State::Closing: return "closing";
    case State::Closed: return "closed";
    }
    return "?";
}

Session::Session(SessionDeps deps, SessionId id, OperatorInfo op, std::vector<protocol::CapabilityGrant> grants,
                 std::optional<protocol::TurnCredentials> turn, bool input_owner)
    : deps_(std::move(deps)), id_(std::move(id)), sid8_(log::short_id(id_)), operator_(std::move(op)), grants_(std::move(grants)),
      turn_(std::move(turn)), input_owner_(input_owner) {
    glib::ObjectCensus::instance().sessions++;
    denied_ = std::make_unique<DeniedSender>();
}

Session::~Session() {
    glib::ObjectCensus::instance().sessions--;
}

void Session::set_state(State s) {
    if (state_ == s) return;
    log::info("session", "state", {{"session", sid8_}, {"from", state_name(state_)}, {"to", state_name(s)}});
    state_ = s;
    if (consumer_ && deps_.snapshot && consumer_->pipeline()) deps_.snapshot(GST_BIN(consumer_->pipeline()), "state-changed");
}

void Session::milestone(const std::string& name) {
    const auto ms = duration_cast<milliseconds>(steady_clock::now() - attached_at_).count();
    log::info("session", "milestone", {{"session", sid8_}, {"milestone", name}, {"ms", std::to_string(ms)}});
    // Only milestones that need the PEER re-arm the watchdog and name a timeout: local steps
    // (local-description-set, ice-gathering-complete) happen whether or not an answer ever
    // comes, and an unanswered offer must read `negotiation-timeout:offer-created` (docs/23).
    static const std::set<std::string> local{"local-description-set", "ice-gathering-complete", "first-frame-sent"};
    if (!local.count(name)) {
        last_milestone_ = name;
        if (state_ == State::Building || state_ == State::Offered) arm_watchdog();
    }
    if (name == "remote-description-set" && renegotiation_pending_ && consumer_ && !consumer_->offer_in_flight() && !closing()) {
        renegotiation_pending_ = false; // the change that queued while the last offer was out
        request_offer();
    }
    if (consumer_ && deps_.snapshot && consumer_->pipeline()) deps_.snapshot(GST_BIN(consumer_->pipeline()), name);
}

void Session::arm_watchdog() {
    watchdog_ = deps_.loop->add_timeout(NEGOTIATION_WATCHDOG, [this] {
        if (state_ == State::Building || state_ == State::Offered) close("negotiation-timeout:" + last_milestone_);
        return false;
    });
}

void Session::arm_liveness() {
    liveness_ = deps_.loop->add_timeout(LIVENESS_BUDGET, [this] {
        if (state_ == State::Connected) close("heartbeat");
        return false;
    });
}

const nlohmann::json& Session::granted_params(std::string_view cap) const {
    static const nlohmann::json empty = nlohmann::json::object();
    for (const auto& c : caps_)
        if (c.manifest.name == cap) return c.params;
    return empty;
}

AttachedCapability* Session::attached(std::string_view cap) {
    for (auto& c : caps_)
        if (c.manifest.name == cap) return &c;
    return nullptr;
}

void Session::attach(std::vector<AttachedCapability> caps) {
    deps_.loop->assert_owner("Session::attach");
    caps_ = std::move(caps);
    attached_at_ = steady_clock::now();
    set_state(State::Building);
    milestone("attached");
    // Capabilities declare their tracks (add_track valid only inside session_attached).
    for (auto& c : caps_) {
        auto ctx = std::make_unique<SessionContextImpl>(*this, c.manifest.name);
        attaching_ = true;
        attaching_cap_ = c.manifest.name;
        try {
            c.capability->session_attached(*ctx, c.params);
        } catch (const std::exception& e) {
            log::error("session", "session_attached threw", {{"session", sid8_}, {"cap", c.manifest.name}, {"error", e.what()}});
        }
        attaching_ = false;
        contexts_[c.manifest.name] = std::move(ctx);
    }
    // Channels in class order: control, realtime, bulk…, stream… (docs/23).
    std::vector<media::ChannelParams> channels{{"fjarr:control", true, -1}, {"fjarr:realtime", false, 0}};
    for (const auto& c : caps_)
        for (const auto& d : c.manifest.channels)
            if (d.channel == ChannelClass::Bulk) channels.push_back({"fjarr:bulk:" + c.manifest.name, true, -1});
    for (const auto& c : caps_)
        for (const auto& d : c.manifest.channels)
            if (d.channel == ChannelClass::Stream) channels.push_back({"fjarr:stream:" + c.manifest.name, false, 0});

    media::ConsumerHooks hooks;
    std::weak_ptr<Session> weak = weak_from_this();
    const Generation gen = generation_;
    auto guard = [weak, gen](auto fn) {
        return [weak, gen, fn](auto&&... args) {
            auto self = weak.lock();
            if (!self || self->generation_ != gen || self->state_ == State::Closed) return;
            fn(*self, std::forward<decltype(args)>(args)...);
        };
    };
    hooks.on_offer = guard([](Session& s, const std::string& sdp) { s.send_offer(sdp); });
    hooks.on_ice = guard([](Session& s, unsigned mline, const std::string& cand) {
        nlohmann::json m = protocol::signaling_base("ice");
        m["session_id"] = s.id_;
        m["candidate"] = cand;
        m["sdp_mline_index"] = mline;
        s.deps_.send_signal(std::move(m));
    });
    hooks.on_connection_state = guard([](Session& s, const std::string& st) {
        s.connection_state_ = st;
        if (st == "connected") s.maybe_connected();
        else if (st == "failed") s.close("ice-failed");
    });
    hooks.on_ice_state = guard([](Session& s, const std::string& st) {
        if (st == "failed" && s.state_ != State::Closing && s.state_ != State::Closed) s.close("ice-failed");
    });
    hooks.on_channel_open = guard([](Session& s, GstWebRTCDataChannel* dc, const std::string& label) { s.on_channel_open(dc, label); });
    hooks.on_milestone = guard([](Session& s, const std::string& m) { s.milestone(m); });
    hooks.on_error = guard([](Session& s, const std::string& err) {
        log::error("session", "media error", {{"session", s.sid8_}, {"error", err}});
        s.close("media-error");
    });
    hooks.on_negotiation_needed = guard([](Session&) {}); // we drive offers ourselves (caps gate / update_tracks)
    hooks.on_keyframe_request = guard([](Session& s, const std::string& track_id) {
        // A PLI/FIR from the peer: worth a log line — a peer asking every second turns a CBR
        // hardware encoder into a frame-skipping one (diagnosed with this line in slice 3c).
        log::debug("session", "keyframe requested by peer", {{"session", s.sid8_}, {"track", track_id}});
        auto it = s.subscribed_tier_.find(track_id);
        if (it != s.subscribed_tier_.end()) s.deps_.plane->hub().request_keyframe(media::HubKey{track_id, it->second});
    });
    consumer_ = std::make_unique<media::ConsumerPipeline>(
        id_, deps_.loop->context(), [loop = deps_.loop](std::function<void()> fn) { loop->post(std::move(fn)); }, std::move(hooks),
        deps_.config->media.gop_seconds);
    if (!consumer_->build(deps_.config->agent.ice_policy, turn_, channels)) {
        // Nothing was accepted yet: reject (docs/08), do not close what never opened.
        nlohmann::json r = protocol::signaling_base("session-reject");
        r["session_id"] = id_;
        r["reason"] = "media-error";
        deps_.send_signal(std::move(r));
        closed_sent_ = true;
        close("media-error");
        return;
    }
    milestone("channels-created");
    for (auto& [cap, spec] : pending_tracks_) {
        std::string reason;
        deps_.plane->register_track({spec, cap, cap == "fjarr.test"});
        if (!deps_.plane->track_available(spec.track_id, &reason)) {
            log::warn("session", "track unavailable, not offered", {{"session", sid8_}, {"track", spec.track_id}, {"reason", reason}});
            deps_.plane->unregister_track(spec.track_id);
            continue;
        }
        if (consumer_->add_track(spec, cap)) tracks_[spec.track_id] = {cap, spec};
    }
    pending_tracks_.clear();
    nlohmann::json accept = protocol::signaling_base("session-accept");
    accept["session_id"] = id_;
    deps_.send_signal(std::move(accept));
    milestone("caps-fixed"); // codec preferences carry the caps (docs/23, consumer.cpp)
    request_offer();
}

void Session::request_offer() {
    if (!consumer_ || state_ == State::Closing || state_ == State::Closed) return;
    if (consumer_->offer_in_flight()) {
        renegotiation_pending_ = true;
        return;
    }
    consumer_->create_offer();
}

void Session::send_offer(const std::string& sdp) {
    manifest_version_++;
    nlohmann::json m = protocol::signaling_base("offer");
    m["session_id"] = id_;
    m["sdp"] = sdp;
    m["tracks"] = protocol::manifest_to_json(consumer_->manifest());
    m["manifest_version"] = manifest_version_;
    deps_.send_signal(std::move(m));
    if (state_ == State::Building) set_state(State::Offered);
    if (state_ == State::Connected && deps_.snapshot && consumer_->pipeline()) deps_.snapshot(GST_BIN(consumer_->pipeline()), "renegotiation");
    log::info("session", "offer sent", {{"session", sid8_}, {"manifest_version", std::to_string(manifest_version_)},
                                        {"tracks", std::to_string(consumer_->manifest().size())}});
}

void Session::on_signal(const protocol::SignalingMessage& msg) {
    deps_.loop->assert_owner("Session::on_signal");
    if (msg.type == "answer") {
        if (!consumer_) return;
        consumer_->set_remote_answer(msg.body.value("sdp", ""));
        answered_once_ = true;
        // A renegotiation that queued while this offer was out goes next — once the
        // answer is applied (the remote-description-set milestone), not now: the
        // offer is still in flight until that promise settles.
    } else if (msg.type == "ice") {
        if (consumer_) consumer_->add_ice_candidate(static_cast<unsigned>(msg.body.value("sdp_mline_index", 0)), msg.body.value("candidate", ""));
    } else if (msg.type == "ice-restart") {
        // No webrtcbin release restarts ICE in place (docs/23): a fresh session, now.
        close("ice-restart", /*retry=*/true);
    } else if (msg.type == "session-close") {
        close(msg.body.value("reason", "operator-closed"), false, /*from_server=*/true);
    } else if (msg.type == "peer-gone") {
        close("peer-gone", false, true);
    }
}

void Session::on_channel_open(GstWebRTCDataChannel* dc, const std::string& label) {
    channels_[label] = glib::GObjectPtr<GstWebRTCDataChannel>(glib::ref_object(dc));
    // The handler runs on the SCTP thread: it may touch only the boxed context, never the session.
    auto* boxed = new ChannelContext{weak_from_this(), generation_, deps_.loop, label};
    dc_signals_.emplace_back(
        dc, "on-message-string",
        G_CALLBACK((+[](GstWebRTCDataChannel*, gchar* text, gpointer d) {
            const auto* c = static_cast<const ChannelContext*>(d);
            std::string t = text ? text : "";
            c->loop->post([w = c->session, g = c->generation, label = c->label, t = std::move(t)] {
                auto self = w.lock();
                if (!self || self->generation_ != g) return;
                self->on_channel_text(label, t);
            });
        })),
        boxed, [](gpointer d, GClosure*) { delete static_cast<ChannelContext*>(d); });
    if (label == "fjarr:control") {
        auto s = std::make_unique<ControlSender>();
        s->dc = dc;
        senders_[label] = std::move(s);
        control_open_ = true;
        milestone("control-open");
        maybe_connected();
    } else if (label == "fjarr:realtime") {
        auto s = std::make_unique<RealtimeSender>();
        s->dc = dc;
        senders_[label] = std::move(s);
    } else if (label.rfind("fjarr:bulk:", 0) == 0) {
        auto s = std::make_unique<BulkSender>();
        s->dc = dc;
        g_object_set(dc, "buffered-amount-low-threshold", static_cast<guint64>(LOW_WATER), nullptr);
        senders_[label] = std::move(s);
        // Fires on webrtcbin's thread: the core's blob pump and then the capability's drain callback
        // run on the loop, looked up by label.
        auto* drain_ctx = new ChannelContext{weak_from_this(), generation_, deps_.loop, label};
        dc_signals_.emplace_back(
            dc, "on-buffered-amount-low",
            G_CALLBACK((+[](GstWebRTCDataChannel*, gpointer d) {
                const auto* c = static_cast<const ChannelContext*>(d);
                c->loop->post([w = c->session, g = c->generation, label = c->label] {
                    auto self = w.lock();
                    if (!self || self->generation_ != g || self->closing()) return;
                    self->pump_blobs(label);
                    auto it = self->senders_.find(label);
                    if (it == self->senders_.end()) return;
                    if (auto* bulk = dynamic_cast<BulkSender*>(it->second.get()); bulk && bulk->drain) bulk->drain();
                });
            })),
            drain_ctx, [](gpointer d, GClosure*) { delete static_cast<ChannelContext*>(d); });
        // Inbound binary (docs/08#blob-frames): copied off the SCTP thread, framed on the loop.
        auto* data_ctx = new ChannelContext{weak_from_this(), generation_, deps_.loop, label};
        dc_signals_.emplace_back(
            dc, "on-message-data",
            G_CALLBACK((+[](GstWebRTCDataChannel*, GBytes* data, gpointer d) {
                const auto* c = static_cast<const ChannelContext*>(d);
                gsize n = 0;
                const auto* p = static_cast<const char*>(data ? g_bytes_get_data(data, &n) : nullptr);
                std::string bytes(p ? p : "", p ? n : 0);
                c->loop->post([w = c->session, g = c->generation, label = c->label, bytes = std::move(bytes)] {
                    auto self = w.lock();
                    if (!self || self->generation_ != g) return;
                    self->on_channel_data(label, bytes);
                });
            })),
            data_ctx, [](gpointer d, GClosure*) { delete static_cast<ChannelContext*>(d); });
        pump_blobs(label); // blobs queued before the channel opened go out now
    }
}

void Session::on_channel_data(const std::string& label, const std::string& bytes) {
    if (closing()) {
        dropped_binary_++;
        return;
    }
    const std::string cap_name = label.substr(std::string("fjarr:bulk:").size());
    AttachedCapability* cap = attached(cap_name);
    auto ctx = contexts_.find(cap_name);
    if (!cap || ctx == contexts_.end()) {
        dropped_binary_++;
        return;
    }
    BulkFraming framing = BulkFraming::Raw;
    for (const auto& d : cap->manifest.channels)
        if (d.channel == ChannelClass::Bulk) framing = d.framing;
    const std::span<const std::byte> view(reinterpret_cast<const std::byte*>(bytes.data()), bytes.size());
    try {
        if (framing == BulkFraming::Raw) {
            cap->capability->on_binary(*ctx->second, view);
            return;
        }
        const auto chunk = blob::parse_chunk(view);
        if (!chunk) { // docs/08: a bad header is dropped and counted, never delivered
            dropped_binary_++;
            log::debug("router", "dropped blob chunk", {{"session", sid8_}, {"cap", cap_name}, {"bytes", std::to_string(bytes.size())}});
            return;
        }
        cap->capability->on_blob_chunk(*ctx->second, *chunk);
    } catch (const std::exception& e) {
        log::error("router", "capability threw on binary", {{"session", sid8_}, {"cap", cap_name}, {"error", e.what()}});
    }
}

blob::BlobRef Session::send_blob(const std::string& cap, std::string bytes, std::string media_type, std::function<void(bool)> done) {
    deps_.loop->assert_owner("Session::send_blob");
    AttachedCapability* c = attached(cap);
    bool declared = false;
    if (c)
        for (const auto& d : c->manifest.channels)
            if (d.channel == ChannelClass::Bulk) declared = true;
    if (!declared) throw FjarrError(std::string(error_codes::payload_invalid), cap + " declares no bulk channel; blobs need one (docs/08#blob-frames)");
    if (closing()) throw FjarrError(std::string(error_codes::payload_invalid), "session is closing");
    blob::BlobRef ref{protocol::new_event_id(), bytes.size(), std::move(media_type)};
    const std::string label = "fjarr:bulk:" + cap;
    auto it = blob_pumps_.find(label);
    if (it == blob_pumps_.end()) it = blob_pumps_.emplace(label, BlobPump(SCTP_MAX_MESSAGE - blob::HEADER_BYTES)).first;
    it->second.enqueue(BlobTransfer{ref.id, std::move(bytes), 0, std::move(done)});
    // docs/08: the referencing envelope goes out first — the caller sends it right after this
    // returns, so the first chunks leave on the next loop turn.
    deps_.loop->post([w = weak_from_this(), g = generation_, label] {
        auto self = w.lock();
        if (!self || self->generation_ != g) return;
        self->pump_blobs(label);
    });
    return ref;
}

void Session::cancel_blob(std::string_view blob_id) {
    for (auto& [_, pump] : blob_pumps_)
        if (pump.cancel(blob_id)) return;
}

void Session::pump_blobs(const std::string& label) {
    auto pump = blob_pumps_.find(label);
    if (pump == blob_pumps_.end()) return;
    auto s = senders_.find(label);
    pump->second.pump(s == senders_.end() ? nullptr : s->second.get());
}

void Session::maybe_connected() {
    if (state_ != State::Offered && state_ != State::Building) return;
    if (connection_state_ != "connected" || !control_open_) return;
    watchdog_.cancel();
    set_state(State::Connected);
    arm_liveness();
    stats_timer_ = deps_.loop->add_timeout(milliseconds(1000), [this] {
        sample_stats();
        return true;
    });
    rate_timer_ = deps_.loop->add_timeout(milliseconds(200), [this] {
        rate_tick();
        return true;
    });
    if (deps_.emit) deps_.emit(SessionEvent{"started", id_, operator_, "", ""});
}

void Session::on_channel_text(const std::string& label, const std::string& text) {
    // Once closing, capabilities are detached: nothing inbound may reach them (the control
    // channel stays open only to flush what the close emitted).
    if (closing() || text.size() > protocol::MAX_ENVELOPE_BYTES) {
        dropped_envelopes_++;
        return;
    }
    auto env = protocol::parse_envelope(text);
    if (!env) {
        dropped_envelopes_++;
        return;
    }
    try {
        route(*env, label);
    } catch (const std::exception& e) {
        // Malformed-but-parseable payloads must never unwind through the loop (any granted operator can send them).
        dropped_envelopes_++;
        log::warn("router", "dropped envelope", {{"session", sid8_}, {"cap", env->cap}, {"type", env->type}, {"error", e.what()}});
        if (env->kind == "request") reply_error(*env, error_codes::payload_invalid, e.what());
    }
}

void Session::route(const Envelope& env, const std::string& label) {
    if (env.cap == "fjarr.core") {
        handle_core(env);
        return;
    }
    AttachedCapability* cap = attached(env.cap);
    if (!cap) {
        // docs/08#envelope: registered but not in this session's grant → denied; unheard of → unknown.
        const bool known = deps_.known_capability && deps_.known_capability(env.cap);
        if (env.kind == "request") {
            if (known) reply_error(env, error_codes::capability_denied, "capability not granted to this session");
            else reply_error(env, error_codes::capability_unknown, "capability not attached to this session");
        }
        return;
    }
    if (env.type == "select-tracks" && env.kind == "request") {
        handle_select_tracks(env, *cap);
        return;
    }
    if (cap->manifest.input_bearing && !input_owner_) {
        // docs/10: later owners are read-only until transfer.
        if (env.kind == "request") reply_error(env, error_codes::capability_denied, "input is owned by another operator");
        dropped_envelopes_++;
        return;
    }
    (void)label;
    try {
        cap->capability->on_message(*contexts_[env.cap], env);
    } catch (const FjarrError& e) {
        if (env.kind == "request") reply_error(env, e.code(), e.message());
    } catch (const std::exception& e) {
        log::error("router", "capability threw", {{"session", sid8_}, {"cap", env.cap}, {"error", e.what()}});
        if (env.kind == "request") reply_error(env, error_codes::internal, e.what());
    }
}

void Session::handle_core(const Envelope& env) {
    if (env.kind != "request") return;
    if (env.type == "ping" || env.type == "time-sync") {
        arm_liveness();
        if (deps_.on_ping) deps_.on_ping(id_);
        if (silent_pings_) return; // fjarr.test silence hook
        const std::int64_t t1 = protocol::now_ms();
        // t0 is unix milliseconds (int64): an `int` default would truncate it (found by fjarr-opsim).
        const std::int64_t t0 = env.payload.contains("t0") && env.payload["t0"].is_number() ? env.payload["t0"].get<std::int64_t>() : 0;
        nlohmann::json p{{"ok", true}, {"t0", t0}, {"t1", t1}, {"t2", protocol::now_ms()}};
        send_control(protocol::make_envelope("fjarr.core", env.type == "ping" ? "pong" : "time-sync", "result", std::move(p), env.event_id));
    }
}

void Session::handle_select_tracks(const Envelope& env, const AttachedCapability& cap) {
    const auto& tracks = env.payload.value("tracks", nlohmann::json::array());
    if (!tracks.is_array()) {
        reply_error(env, error_codes::payload_invalid, "tracks must be an array");
        return;
    }
    // Validate everything before applying anything (docs/08#track-control).
    for (const auto& t : tracks) {
        if (!t.is_object() || !t.contains("track_id") || !t["track_id"].is_string()) {
            reply_error(env, error_codes::payload_invalid, "tracks[] entries need a string track_id");
            return;
        }
        const std::string id = t["track_id"].get<std::string>();
        auto it = tracks_.find(id);
        if (id.empty() || it == tracks_.end() || it->second.first != cap.manifest.name) {
            reply_error(env, error_codes::payload_invalid, "unknown track " + id);
            return;
        }
        if (t.contains("tier") && !t["tier"].is_string()) {
            reply_error(env, error_codes::payload_invalid, "tier must be a string");
            return;
        }
        if (!t.contains("enabled") || !t["enabled"].is_boolean()) {
            reply_error(env, error_codes::payload_invalid, "enabled must be a boolean");
            return;
        }
        const std::string tier = t.value("tier", "active");
        if (tier != "active" && tier != "thumbnail") {
            reply_error(env, error_codes::payload_invalid, "tier must be active|thumbnail");
            return;
        }
    }
    for (const auto& t : tracks) apply_demand(t.value("track_id", ""), t["enabled"].get<bool>(), t.value("tier", "active"));
    reply(env, nlohmann::json{{"ok", true}});
    if (deps_.snapshot && consumer_ && consumer_->pipeline()) deps_.snapshot(GST_BIN(consumer_->pipeline()), "select-tracks");
}

void Session::apply_demand(const std::string& track_id, bool enabled, const std::string& demanded) {
    auto* ct = consumer_ ? consumer_->track(track_id) : nullptr;
    if (!ct) return;
    auto sink = consumer_->sink_for(track_id);
    auto sub = subscribed_tier_.find(track_id);
    // The client's demand and the agent's override (docs/23 rate control) resolve to what is sent.
    auto& policy = tier_policy_[track_id];
    policy.set_demanded(demanded);
    ct->demanded_tier = demanded;
    const std::string tier = policy.effective();
    if (!enabled) {
        consumer_->set_enabled(track_id, false);
        if (sub != subscribed_tier_.end()) {
            deps_.plane->hub().unsubscribe(media::HubKey{track_id, sub->second}, sink);
            subscribed_tier_.erase(sub);
        }
        deps_.plane->forget_allotment(sink.get());
        return;
    }
    if (sub != subscribed_tier_.end() && sub->second != tier) {
        deps_.plane->hub().unsubscribe(media::HubKey{track_id, sub->second}, sink);
        subscribed_tier_.erase(sub);
        sub = subscribed_tier_.end();
    }
    ct->tier = tier;
    if (!silent_media_) consumer_->set_enabled(track_id, true);
    if (sub == subscribed_tier_.end()) {
        // (Re)enable: subscribe — the hub starts this sink at a keyframe (a retained one or a requested one).
        deps_.plane->hub().subscribe(media::HubKey{track_id, tier}, sink);
        subscribed_tier_[track_id] = tier;
    }
    // Already enabled at this tier: an unchanged demand (the client re-flushes after every
    // manifest change) is a no-op — a resync here made the viewer wait for the next keyframe
    // and stalled the untouched track for up to a GOP during hot-plug.
}

void Session::unsubscribe_all() {
    for (auto& [id, tier] : subscribed_tier_) {
        if (!consumer_) break;
        auto sink = consumer_->sink_for(id);
        if (sink) {
            deps_.plane->hub().unsubscribe(media::HubKey{id, tier}, sink);
            deps_.plane->forget_allotment(sink.get());
        }
    }
    subscribed_tier_.clear();
    rate_timer_.cancel();
}

void Session::rate_tick() {
    if (!consumer_ || state_ != State::Connected) return;
    // The ceiling is what this peer's enabled tracks could take at their targets; the floor is the config's.
    double sum_targets = 0;
    std::vector<media::ConsumerTrack*> enabled;
    for (auto& [id, _] : tracks_) {
        auto* ct = consumer_->track(id);
        if (!ct || !ct->enabled || !subscribed_tier_.count(id)) continue;
        enabled.push_back(ct);
        sum_targets += deps_.plane->band_kbps(id, ct->tier).second * 1000.0;
    }
    const media::RateLimits limits{deps_.config->media.active_floor_kbps * 1000.0, std::max(sum_targets, deps_.config->media.active_kbps * 1000.0) * 1.2};
    if (!estimator_) estimator_.emplace(limits, std::max(sum_targets, limits.floor_bps));
    else estimator_->set_limits(limits);
    const auto now = steady_clock::now();
    media::TwccSample twcc = consumer_->twcc_sample();
    // rtpsession keeps the last window until the next feedback arrives: the same numbers again mean
    // no new feedback, not a window with these numbers (a receiver without feedback would otherwise
    // read as a lossless link and be driven to the ceiling).
    const bool same = twcc.packets == last_twcc_.packets && twcc.packets_recv == last_twcc_.packets_recv && twcc.bitrate_sent == last_twcc_.bitrate_sent &&
                      twcc.bitrate_recv == last_twcc_.bitrate_recv && twcc.avg_delta_of_delta_ns == last_twcc_.avg_delta_of_delta_ns;
    last_twcc_ = twcc;
    if (same) twcc.packets = 0;
    estimator_->update(twcc, now);
    // Is the estimate a measurement or a guess? The estimator credits no more than 1.5x what
    // arrived, so a source that compresses well (a static scene, the test pattern) pins it low
    // while the link is perfect. Demoting on that is demoting on nothing — the peer has to be
    // pushing against the estimate for it to mean anything (docs/23#rate-control-and-tier-switching).
    const bool estimate_tested = twcc.packets > 0 && twcc.bitrate_sent >= media::TierPolicy::TESTED_RATIO * estimator_->estimate_bps();
    if (enabled.empty() || sum_targets <= 0) return;
    // Share the peer's estimate across its tracks in proportion to their tier targets; report
    // each share to the plane (the encoder follows the minimum over its viewers) and tick the
    // per-track tier policy with the *active* band as the reference.
    for (auto* ct : enabled) {
        const double target = deps_.plane->band_kbps(ct->track_id, ct->tier).second * 1000.0;
        ct->allotment_bps = estimator_->estimate_bps() * target / sum_targets;
        auto sink = consumer_->sink_for(ct->track_id);
        deps_.plane->report_allotment(ct->track_id, ct->tier, sink.get(), ct->allotment_bps);
        const double active_low = deps_.plane->band_kbps(ct->track_id, "active").first * 1000.0;
        // A demoted viewer's share is computed against the thumbnail target: judge promotion on what
        // the active tier would need instead.
        const double judged = ct->tier == "active" ? ct->allotment_bps : estimator_->estimate_bps() * (deps_.plane->band_kbps(ct->track_id, "active").second * 1000.0) / (sum_targets - target + deps_.plane->band_kbps(ct->track_id, "active").second * 1000.0);
        auto& policy = tier_policy_[ct->track_id];
        if (auto changed = policy.update(judged, active_low, deps_.plane->tier_possible(ct->track_id, "thumbnail"), estimate_tested, now)) {
            log::info("session", policy.demoted() ? "tier reduced for this viewer" : "tier restored for this viewer",
                      {{"session", sid8_}, {"track", ct->track_id}, {"tier", *changed}, {"estimate_bps", std::to_string(static_cast<long long>(estimator_->estimate_bps()))}});
            apply_demand(ct->track_id, true, ct->demanded_tier); // resolves to the new effective tier
            if (deps_.snapshot && consumer_->pipeline()) deps_.snapshot(GST_BIN(consumer_->pipeline()), "tier-changed");
        }
    }
}

void Session::sample_stats() {
    if (!consumer_ || state_ != State::Connected) return;
    consumer_->get_stats([this](media::StatsSample sample) {
        if (state_ != State::Connected) return;
        std::map<std::string, nlohmann::json> per_cap;
        for (const auto& t : sample.tracks) {
            auto* ct = consumer_->track(t.track_id);
            auto it = tracks_.find(t.track_id);
            if (!ct || it == tracks_.end()) continue;
            const std::uint64_t delta_bytes = t.bytes_sent >= ct->last_bytes_sent ? t.bytes_sent - ct->last_bytes_sent : 0;
            ct->last_bytes_sent = t.bytes_sent;
            auto stats = deps_.plane->hub().subscriber_stats(media::HubKey{t.track_id, ct->tier}, ct->sink);
            const unsigned long frames = stats.delivered - ct->last_frames;
            const unsigned long dropped = stats.dropped - ct->last_dropped;
            ct->last_frames = stats.delivered;
            ct->last_dropped = stats.dropped;
            const std::uint64_t nacks = t.nacks >= ct->last_nacks ? t.nacks - ct->last_nacks : 0;
            const std::uint64_t kfr = t.keyframe_requests >= ct->last_keyframe_requests ? t.keyframe_requests - ct->last_keyframe_requests : 0;
            ct->last_nacks = t.nacks;
            ct->last_keyframe_requests = t.keyframe_requests;
            auto& arr = per_cap[it->second.first];
            if (!arr.is_array()) arr = nlohmann::json::array();
            arr.push_back({{"track_id", t.track_id}, {"enabled", ct->enabled}, {"tier", ct->demanded_tier}, {"effective_tier", ct->tier},
                           {"estimate_bps", static_cast<std::uint64_t>(ct->allotment_bps)}, {"adaptive", deps_.plane->adaptive(t.track_id)},
                           {"bitrate_bps", delta_bytes * 8}, {"frames", frames}, {"dropped", dropped}, {"nacks", nacks}, {"keyframe_requests", kfr}});
        }
        last_stats_ = nlohmann::json::object();
        for (auto& [cap, arr] : per_cap) last_stats_[cap] = arr;
        last_stats_["selected_pair"] = sample.selected_pair;
        if (estimator_)
            last_stats_["rate"] = nlohmann::json{{"estimate_bps", static_cast<std::uint64_t>(estimator_->estimate_bps())}, {"state", estimator_->state()},
                                                 {"decreases", estimator_->decreases()}, {"last_good_bps", static_cast<std::uint64_t>(estimator_->last_good_bps())},
                                                 {"loss_pct", estimator_->loss_pct()}, {"carried_ratio", estimator_->carried_ratio()}};
        last_stats_["twcc"] = sample.twcc; // docs/23#rate-control-and-tier-switching: the estimator's input, visible in /stats
        last_stats_["rtx"] = nlohmann::json{{"requests", sample.rtx_requests}, {"packets", sample.rtx_packets}};
        for (auto& [cap, arr] : per_cap) {
            bool any_enabled = false;
            for (const auto& t : arr) any_enabled = any_enabled || t.value("enabled", false);
            if (!any_enabled) continue;
            send_control(protocol::make_envelope(cap, "bandwidth-stats", "event", nlohmann::json{{"interval_ms", 1000}, {"tracks", arr}}));
        }
    });
}

void Session::send_control(const Envelope& env) {
    auto it = senders_.find("fjarr:control");
    if (it == senders_.end()) return; // not open yet / gone: dropped (docs/23 counts it)
    try {
        it->second->send(env);
    } catch (const FjarrError& e) {
        // The router's own replies and stats are best effort: a refused send is logged, never propagated.
        dropped_envelopes_++;
        log::warn("router", "control send refused", {{"session", sid8_}, {"error", e.what()}});
    }
}

void Session::reply(const Envelope& request, nlohmann::json payload) {
    send_control(protocol::make_envelope(request.cap, request.type, "result", std::move(payload), request.event_id));
}

void Session::reply_error(const Envelope& request, std::string_view code, std::string_view message) {
    // Error text may echo operator input: clamp it so the reply itself stays under the envelope limit.
    const std::string msg(message.substr(0, 256));
    reply(request, nlohmann::json{{"ok", false}, {"error", {{"code", std::string(code)}, {"message", msg}}}});
}

ChannelSender& Session::sender(ChannelClass cls, const std::string& cap) {
    std::string label;
    switch (cls) {
    case ChannelClass::Control: label = "fjarr:control"; break;
    case ChannelClass::Realtime: label = "fjarr:realtime"; break;
    case ChannelClass::Bulk: label = "fjarr:bulk:" + cap; break;
    case ChannelClass::Stream: label = "fjarr:stream:" + cap; break;
    }
    auto it = senders_.find(label);
    if (it != senders_.end()) return *it->second;
    static_cast<DeniedSender*>(denied_.get())->why = label + " is not open (declare the channel in the manifest, and wait for the session to connect)";
    return *denied_;
}

void Session::add_track(const std::string& cap, TrackSpec spec) {
    if (!attaching_ || attaching_cap_ != cap) throw FjarrError("payload-invalid", "add_track is only valid inside session_attached; use update_tracks");
    pending_tracks_.emplace_back(cap, std::move(spec));
}

void Session::update_tracks(const std::string& cap, std::vector<TrackSpec> full_set) {
    deps_.loop->assert_owner("Session::update_tracks");
    if (!consumer_ || state_ == State::Closing || state_ == State::Closed) return;
    std::set<std::string> wanted;
    for (const auto& s : full_set) wanted.insert(s.track_id);
    // Removals: valve first, transceiver inactive, out of the manifest (docs/23).
    std::vector<std::string> removed;
    for (const auto& [id, owned] : tracks_)
        if (owned.first == cap && !wanted.count(id)) removed.push_back(id);
    for (const auto& id : removed) {
        apply_demand(id, false, "active");
        consumer_->remove_track(id);
        tracks_.erase(id);
        deps_.plane->unregister_track(id);
    }
    bool changed = !removed.empty();
    for (auto& s : full_set) {
        if (tracks_.count(s.track_id)) continue;
        std::string reason;
        deps_.plane->register_track({s, cap, cap == "fjarr.test"});
        if (!deps_.plane->track_available(s.track_id, &reason)) {
            log::warn("session", "track unavailable", {{"session", sid8_}, {"track", s.track_id}, {"reason", reason}});
            deps_.plane->unregister_track(s.track_id);
            continue;
        }
        if (consumer_->add_track(s, cap)) {
            tracks_[s.track_id] = {cap, s};
            changed = true;
        }
    }
    if (changed) request_offer();
}

TrackState Session::track_state(std::string_view track_id) const {
    TrackState st;
    auto it = tracks_.find(std::string(track_id));
    if (it == tracks_.end() || !consumer_) {
        st.available = false;
        return st;
    }
    auto* ct = const_cast<media::ConsumerPipeline*>(consumer_.get())->track(std::string(track_id));
    if (ct) {
        st.enabled = ct->enabled;
        st.tier = ct->tier;
        st.mid = ct->mid;
        st.subscribers = deps_.plane->hub().subscriber_count(media::HubKey{ct->track_id, ct->tier});
    }
    return st;
}

void Session::run_async(std::function<void()> job, std::function<void()> done) {
    std::weak_ptr<Session> weak = weak_from_this();
    const Generation gen = generation_;
    deps_.loop->run_async(std::move(job), std::move(done), [weak, gen] {
        auto s = weak.lock();
        return s && s->generation_ == gen && s->state_ != State::Closed;
    });
}

std::unique_ptr<DeadmanHandle> Session::arm_deadman(milliseconds budget, std::function<void()> on_expiry) {
    auto d = std::make_shared<Deadman>();
    d->loop = deps_.loop;
    d->budget = budget;
    d->on_expiry = std::move(on_expiry);
    d->arm();
    deadmans_.push_back(d);
    auto proxy = std::make_unique<DeadmanProxy>();
    proxy->d = d;
    return proxy;
}

void Session::test_silence(bool pings, bool media, milliseconds ms) {
    if (!deps_.test_hooks) throw FjarrError("capability-denied", "test hooks are disabled (fjarr.test test_hooks = false)");
    silent_pings_ = pings;
    silent_media_ = media;
    if (media)
        for (auto& [id, _] : tracks_) consumer_->set_enabled(id, false);
    silence_timer_ = deps_.loop->add_timeout(ms, [this] {
        silent_pings_ = false;
        const bool was_media = silent_media_;
        silent_media_ = false;
        if (was_media)
            for (auto& [id, tier] : subscribed_tier_) {
                consumer_->set_enabled(id, true);
                deps_.plane->hub().resync(media::HubKey{id, tier}, consumer_->sink_for(id));
            }
        return false;
    });
}

nlohmann::json Session::describe() const {
    return nlohmann::json{{"session_id", id_}, {"state", state_name(state_)}, {"operator", {{"id", operator_.id}, {"label", operator_.label}}},
                          {"manifest_version", manifest_version_}, {"input_owner", input_owner_}, {"dropped_envelopes", dropped_envelopes_}};
}

std::size_t Session::buffered_bytes() const {
    std::size_t n = 0;
    for (const auto& [_, s] : senders_) n += s->buffered_amount();
    for (const auto& [_, p] : blob_pumps_) n += p.queued_bytes();
    return n;
}

void Session::close(const std::string& reason, bool retry, bool from_server) {
    deps_.loop->assert_owner("Session::close");
    if (state_ == State::Closing || state_ == State::Closed) return;
    for (auto& [_, pump] : blob_pumps_) pump.fail_all(); // blobs never complete on a closing session (done(false) while the capability is still attached)
    set_state(State::Closing);
    log::info("session", "closing", {{"session", sid8_}, {"reason", reason}, {"retry", retry ? "true" : "false"}});
    watchdog_.cancel();
    liveness_.cancel();
    stats_timer_.cancel();
    silence_timer_.cancel();
    // 1. Safety first (docs/15): release input before anything can block.
    for (auto& c : caps_) {
        if (!c.manifest.input_bearing) continue;
        try {
            c.capability->release_all_input(id_);
        } catch (const std::exception& e) {
            log::error("session", "release_all_input threw", {{"session", sid8_}, {"cap", c.manifest.name}, {"error", e.what()}});
        }
    }
    for (auto& w : deadmans_)
        if (auto d = w.lock()) d->expire();
    deadmans_.clear();
    // 2. Capabilities.
    const DetachReason dr = detach_reason_for(reason);
    for (auto& c : caps_) {
        try {
            c.capability->session_detached(id_, dr, reason);
        } catch (const std::exception& e) {
            log::error("session", "session_detached threw", {{"session", sid8_}, {"cap", c.manifest.name}, {"error", e.what()}});
        }
    }
    // 3. Signal the operator (unless the server/operator told us). Signaling does not wait for
    //    the media teardown below: an ice-restart answer must be prompt (docs/23).
    if (!from_server && !closed_sent_) {
        nlohmann::json m = protocol::signaling_base("session-close");
        m["session_id"] = id_;
        m["reason"] = reason;
        if (retry) m["retry"] = true;
        deps_.send_signal(std::move(m));
        closed_sent_ = true;
    }
    // 4. Media stops flowing now: valves closed, hub subscriptions dropped. The pipeline itself
    //    is torn down a moment later so the control channel can transmit what step 1 emitted
    //    (`deadman{expired}` is how docs/15's release-on-session-end is observed); a NULL state
    //    change in the same loop turn discarded it (found by fjarr-opsim). Bounded, never waited on.
    unsubscribe_all();
    for (auto& [id, _] : tracks_)
        if (consumer_) consumer_->set_enabled(id, false);
    if (deps_.snapshot && consumer_ && consumer_->pipeline()) deps_.snapshot(GST_BIN(consumer_->pipeline()), "closing");
    const bool immediate = from_server; // the peer is gone: nothing to flush
    closing_reason_ = reason;
    closing_retry_ = retry;
    std::weak_ptr<Session> weak = weak_from_this();
    auto finish = [weak, reason, retry, from_server] {
        auto self = weak.lock();
        if (!self) return;
        self->finish_close(reason, retry, from_server);
    };
    if (immediate || !consumer_) finish();
    else {
        close_timer_ = deps_.loop->add_timeout(milliseconds(150), [finish] {
            finish();
            return false;
        });
    }
}

void Session::finish_close(const std::string& reason, bool retry, bool from_server) {
    if (state_ == State::Closed) return;
    close_timer_.cancel();
    unsubscribe_all(); // nothing may have re-subscribed during the flush window, but the hub must agree
    dc_signals_.clear();
    senders_.clear();
    blob_pumps_.clear();
    channels_.clear();
    if (consumer_) {
        consumer_->stop();
        consumer_.reset();
    }
    for (auto& [id, _] : tracks_) deps_.plane->unregister_track(id);
    tracks_.clear();
    (void)reason;
    (void)retry;
    (void)from_server;
    generation_++;
    contexts_.clear();
    set_state(State::Closed);
    if (deps_.emit) deps_.emit(SessionEvent{"ended", id_, operator_, reason, ""});
    if (deps_.on_closed) deps_.on_closed(id_);
    // A session's pipeline is freed across several threads' arenas; glibc keeps those pages
    // unless asked. The soak measures RSS (docs/16), so give them back at every session end.
    malloc_trim(0);
}

void Session::flush_close() {
    if (state_ == State::Closing) {
        close_timer_.cancel();
        finish_close(closing_reason_, closing_retry_, false);
    }
}

} // namespace fjarr::core
