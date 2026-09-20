#include "consumer.hpp"

#include <gst/sdp/sdp.h>
#include <gst/video/video.h>

#include "core/log.hpp"

namespace fjarr::media {

// The hub subscriber: pushes rebased buffers into this track's appsrc from the hub thread.
struct ConsumerPipeline::AppSrcSink final : public FrameSink {
    glib::GstElementPtr appsrc; // our own ref
    std::string label;
    std::atomic<unsigned long> pushed{0}, dropped{0};
    bool caps_set = false;
    std::int64_t last_push_us = 0;
    bool push(glib::GstBufferPtr buffer, GstCaps* caps) override {
        if (!appsrc) return false;
        const std::int64_t now = g_get_monotonic_time();
        if (last_push_us > 0 && now - last_push_us > 500000)
            log::warn("media", "delivery gap at appsrc", {{"sink", label}, {"gap_ms", std::to_string((now - last_push_us) / 1000)}});
        last_push_us = now;
        if (!caps_set && caps) {
            gst_app_src_set_caps(GST_APP_SRC(appsrc.get()), caps);
            caps_set = true;
        }
        const GstFlowReturn r = gst_app_src_push_buffer(GST_APP_SRC(appsrc.get()), glib::release_buffer(std::move(buffer)));
        if (r == GST_FLOW_OK) {
            pushed++;
            return true;
        }
        dropped++;
        return false;
    }
};

struct ConsumerPipeline::SignalContext {
    ConsumerPipeline* me;
    std::weak_ptr<bool> alive;
    std::function<void(std::function<void()>)> post;
};

ConsumerPipeline::ConsumerPipeline(std::string session_id, GMainContext* ctx, std::function<void(std::function<void()>)> post,
                                   ConsumerHooks hooks, int gop_seconds)
    : session_id_(std::move(session_id)), sid8_(log::short_id(session_id_)), ctx_(ctx), post_(std::move(post)),
      hooks_(std::move(hooks)) {
    gop_seconds_ = gop_seconds;}

ConsumerPipeline::~ConsumerPipeline() { stop(); }

void ConsumerPipeline::milestone(const std::string& m) {
    if (hooks_.on_milestone) hooks_.on_milestone(m);
}

bool ConsumerPipeline::build(const std::string& ice_policy, const std::optional<protocol::TurnCredentials>& turn,
                             const std::vector<ChannelParams>& channels) {
    pipeline_ = glib::sink_element(gst_pipeline_new(name().c_str()));
    glib::ObjectCensus::instance().pipelines++;
    webrtc_ = glib::make_element("webrtcbin", name() + "/webrtc");
    if (!webrtc_) {
        if (hooks_.on_error) hooks_.on_error("webrtcbin element missing (gstreamer1.0-nice?)");
        return false;
    }
    g_object_set(webrtc_.get(), "bundle-policy", GST_WEBRTC_BUNDLE_POLICY_MAX_BUNDLE, "latency", 100u, nullptr);
    if (ice_policy == "relay") g_object_set(webrtc_.get(), "ice-transport-policy", GST_WEBRTC_ICE_TRANSPORT_POLICY_RELAY, nullptr);
    if (turn) {
        for (const auto& url : turn->urls) {
            // turn(s)://user:pass@host:port[?transport=tcp] — credentials URL-escaped (docs/23, spike Q6).
            std::string u = url;
            const auto scheme_end = u.find("://");
            if (scheme_end == std::string::npos) continue;
            const std::string scheme = u.substr(0, scheme_end);
            if (scheme != "turn" && scheme != "turns") continue;
            glib::GStrPtr user(g_uri_escape_string(turn->username.c_str(), nullptr, FALSE));
            glib::GStrPtr pass(g_uri_escape_string(turn->credential.c_str(), nullptr, FALSE));
            const std::string full = scheme + "://" + user.get() + ":" + pass.get() + "@" + u.substr(scheme_end + 3);
            gboolean ok = FALSE;
            g_signal_emit_by_name(webrtc_.get(), "add-turn-server", full.c_str(), &ok);
            if (!ok) log::warn("consumer", "add-turn-server rejected a URL", {{"session", sid8_}, {"url", url}});
        }
    }
    gst_bin_add(GST_BIN(pipeline_.get()), webrtc_.get());
    glib::GstBusPtr bus(gst_pipeline_get_bus(GST_PIPELINE(pipeline_.get())));
    GSource* watch = gst_bus_create_watch(bus.get());
    g_source_set_callback(watch, reinterpret_cast<GSourceFunc>(&ConsumerPipeline::on_bus), this, nullptr);
    g_source_attach(watch, ctx_);
    bus_watch_ = glib::SourceGuard::attached(watch);
    connect_signals();
    // Channels need the element ≥ READY (spike Q1).
    if (gst_element_set_state(pipeline_.get(), GST_STATE_READY) == GST_STATE_CHANGE_FAILURE) {
        if (hooks_.on_error) hooks_.on_error("pipeline refused READY");
        return false;
    }
    for (const auto& c : channels) {
        glib::GstStructurePtr opts(gst_structure_new("options", "ordered", G_TYPE_BOOLEAN, c.ordered ? TRUE : FALSE, nullptr));
        if (c.max_retransmits >= 0) gst_structure_set(opts.get(), "max-retransmits", G_TYPE_INT, c.max_retransmits, nullptr);
        GstWebRTCDataChannel* dc = nullptr;
        g_signal_emit_by_name(webrtc_.get(), "create-data-channel", c.label.c_str(), opts.get(), &dc);
        if (!dc) {
            if (hooks_.on_error) hooks_.on_error("create-data-channel failed for " + c.label);
            return false;
        }
        channels_.emplace_back(dc);
        // Fires on webrtcbin's thread: the boxed context carries everything it may touch.
        struct OpenContext {
            ConsumerPipeline* me;
            std::weak_ptr<bool> alive;
            std::function<void(std::function<void()>)> post;
            std::string label;
        };
        auto* boxed = new OpenContext{this, alive_, post_, c.label};
        signals_.emplace_back(
            dc, "on-open",
            G_CALLBACK((+[](GstWebRTCDataChannel* ch, gpointer d) {
                const auto* c = static_cast<const OpenContext*>(d);
                auto keep = std::make_shared<glib::GObjectPtr<GstWebRTCDataChannel>>(glib::ref_object(ch));
                c->post([me = c->me, alive = c->alive, lbl = c->label, keep] {
                    if (alive.expired()) return;
                    if (me->hooks_.on_channel_open) me->hooks_.on_channel_open(keep->get(), lbl);
                });
            })),
            boxed, [](gpointer d, GClosure*) { delete static_cast<OpenContext*>(d); });
    }
    milestone("channels-created");
    return true;
}

void ConsumerPipeline::connect_signals() {
    GstElement* w = webrtc_.get();
    // Every handler runs on webrtcbin's thread: it reads the boxed context (a weak token captured
    // at connect time) and posts — never the pipeline's own members (stop() reassigns `alive_`).
    auto box = [this] { return new SignalContext{this, alive_, post_}; };
    auto free_box = [](gpointer d, GClosure*) { delete static_cast<SignalContext*>(d); };
    signals_.emplace_back(w, "on-negotiation-needed", G_CALLBACK((+[](GstElement*, gpointer d) {
                              const auto* c = static_cast<const SignalContext*>(d);
                              c->post([me = c->me, alive = c->alive] {
                                  if (!alive.expired()) me->on_negotiation_needed_cb();
                              });
                          })),
                          box(), free_box);
    signals_.emplace_back(w, "on-ice-candidate", G_CALLBACK(&ConsumerPipeline::on_ice_candidate_cb), box(), free_box);
    for (const char* prop : {"notify::connection-state", "notify::ice-connection-state", "notify::ice-gathering-state",
                             "notify::signaling-state"}) {
        signals_.emplace_back(w, prop, G_CALLBACK(&ConsumerPipeline::on_notify_state), box(), free_box);
    }
}

void ConsumerPipeline::on_negotiation_needed_cb() {
    if (hooks_.on_negotiation_needed) hooks_.on_negotiation_needed();
}

void ConsumerPipeline::on_ice_candidate_cb(GstElement*, guint mline, gchar* cand, gpointer user) {
    const auto* ctx = static_cast<const SignalContext*>(user);
    std::string c = cand ? cand : "";
    ctx->post([self = ctx->me, mline, c, alive = ctx->alive] {
        if (alive.expired()) return;
        if (self->hooks_.on_ice) self->hooks_.on_ice(mline, c);
    });
}

void ConsumerPipeline::on_notify_state(GObject* obj, GParamSpec* pspec, gpointer user) {
    const auto* ctx = static_cast<const SignalContext*>(user);
    const std::string prop = pspec->name;
    const std::string value = glib::enum_prop_nick(obj, pspec->name);
    ctx->post([self = ctx->me, prop, value, alive = ctx->alive] {
        if (alive.expired()) return;
        if (prop == "connection-state") {
            if (value == "connected") self->milestone("dtls-connected");
            if (self->hooks_.on_connection_state) self->hooks_.on_connection_state(value);
        } else if (prop == "ice-connection-state") {
            if (value == "connected" || value == "completed") self->milestone("ice-connected");
            if (self->hooks_.on_ice_state) self->hooks_.on_ice_state(value);
        } else if (prop == "ice-gathering-state") {
            if (value == "complete") self->milestone("ice-gathering-complete");
        }
    });
}

gboolean ConsumerPipeline::on_bus(GstBus*, GstMessage* msg, gpointer user) {
    auto* self = static_cast<ConsumerPipeline*>(user);
    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
        GError* err = nullptr;
        gchar* dbg = nullptr;
        gst_message_parse_error(msg, &err, &dbg);
        glib::GErrorPtr e(err);
        glib::GStrPtr d(dbg);
        const std::string src = GST_OBJECT_NAME(GST_MESSAGE_SRC(msg));
        const std::string text = src + ": " + (err ? err->message : "error");
        // Transport elements (SCTP/DTLS/ICE) fail when the peer goes away — that is
        // the operator's ladder, not a media-plane problem (docs/23 "ice-failed").
        const bool transport = src.find("sctp") != std::string::npos || src.find("dtls") != std::string::npos || src.find("nice") != std::string::npos;
        log::error("consumer", transport ? "transport error" : "pipeline error", {{"session", self->sid8_}, {"error", text}, {"debug", dbg ? dbg : ""}});
        if (transport) {
            if (self->hooks_.on_connection_state) self->hooks_.on_connection_state("failed");
        } else if (self->hooks_.on_error) self->hooks_.on_error(text);
    }
    return G_SOURCE_CONTINUE;
}

ConsumerTrack* ConsumerPipeline::add_track(const TrackSpec& spec, const std::string& cap) {
    if (tracks_.count(spec.track_id)) return tracks_[spec.track_id].get();
    // Reuse a pooled transceiver of the same kind (docs/23 removal/re-add). Parented elements
    // cannot be renamed, so the branch is rebuilt under the new track's name (docs/24 grammar).
    for (auto& [id, t] : tracks_) {
        if (t->pooled && t->kind == spec.kind) {
            std::unique_ptr<ConsumerTrack> reuse = std::move(t);
            tracks_.erase(id);
            reuse->track_id = spec.track_id;
            reuse->label = spec.label;
            reuse->monitor = spec.monitor;
            reuse->cap = cap;
            reuse->pooled = false;
            reuse->enabled = false;
            if (reuse->transceiver) g_object_set(reuse->transceiver.get(), "direction", GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_SENDONLY, nullptr);
            if (!build_branch(*reuse)) return nullptr;
            ConsumerTrack* raw = reuse.get();
            tracks_[spec.track_id] = std::move(reuse);
            return raw;
        }
    }
    auto t = std::make_unique<ConsumerTrack>();
    t->track_id = spec.track_id;
    t->cap = cap;
    t->kind = spec.kind;
    t->label = spec.label;
    t->monitor = spec.monitor;
    t->pt = next_pt_++;
    t->ssrc = g_random_int_range(1, 0x7fffffff);
    // The transceiver's codec preferences carry the caps the offer needs even
    // while the valve is closed and no data flows (docs/23: "their caps come
    // from the encoder configuration").
    t->sink_pad = glib::adopt_pad(gst_element_request_pad_simple(webrtc_.get(), "sink_%u"));
    if (!t->sink_pad) {
        if (hooks_.on_error) hooks_.on_error("webrtcbin refused a sink pad");
        return nullptr;
    }
    const std::string pad_name = glib::pad_name(t->sink_pad.get());
    t->mline = static_cast<unsigned>(std::atoi(pad_name.c_str() + 5));
    GstWebRTCRTPTransceiver* trans = nullptr;
    g_object_get(t->sink_pad.get(), "transceiver", &trans, nullptr);
    t->transceiver.reset(trans);
    if (trans) {
        // The SSRC goes into the preferences too: without it webrtcbin signals a fresh random
        // `a=ssrc` on every re-offer and Chromium recreates the receiver (decoder reset, a GOP
        // of frames lost on an untouched track during hot-plug — found by the lab).
        glib::GstCapsPtr pref(gst_caps_new_simple("application/x-rtp", "media", G_TYPE_STRING, "video", "encoding-name", G_TYPE_STRING,
                                                  "H264", "payload", G_TYPE_INT, t->pt, "clock-rate", G_TYPE_INT, 90000,
                                                  "packetization-mode", G_TYPE_STRING, "1", "ssrc", G_TYPE_UINT, t->ssrc, nullptr));
        g_object_set(trans, "codec-preferences", pref.get(), "direction", GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_SENDONLY, nullptr);
    }
    if (!build_branch(*t)) return nullptr;
    ConsumerTrack* raw = t.get();
    tracks_[spec.track_id] = std::move(t);
    return raw;
}

bool ConsumerPipeline::build_branch(ConsumerTrack& t) {
    // `appsrc ! queue(leaky) ! valve ! rtph264pay ! webrtcbin.sink_%u` under session:<sid8>/<track>/<role>.
    const std::string p = prefix(t.track_id);
    t.appsrc = glib::make_element("appsrc", p + "/appsrc");
    g_object_set(t.appsrc.get(), "is-live", TRUE, "format", GST_FORMAT_TIME, "do-timestamp", FALSE, "block", FALSE,
                 "max-bytes", static_cast<guint64>(0), "min-latency", static_cast<gint64>(0), nullptr);
    glib::GstCapsPtr caps(gst_caps_from_string("video/x-h264,stream-format=byte-stream,alignment=au"));
    gst_app_src_set_caps(GST_APP_SRC(t.appsrc.get()), caps.get());
    t.queue = glib::make_element("queue", p + "/queue");
    g_object_set(t.queue.get(), "leaky", 2, "max-size-buffers", 0u, "max-size-bytes", 0u, "max-size-time",
                 static_cast<guint64>(gop_seconds_) * GST_SECOND /* one GOP (docs/23 constants) */, nullptr);
    t.valve = glib::make_element("valve", p + "/valve");
    g_object_set(t.valve.get(), "drop", TRUE, nullptr);
    t.payloader = glib::make_element("rtph264pay", p + "/payloader");
    g_object_set(t.payloader.get(), "pt", static_cast<guint>(t.pt), "ssrc", static_cast<guint>(t.ssrc), "config-interval", -1,
                 "aggregate-mode", 1 /* zero-latency */, nullptr);
    gst_bin_add_many(GST_BIN(pipeline_.get()), t.appsrc.get(), t.queue.get(), t.valve.get(), t.payloader.get(), nullptr);
    if (!gst_element_link_many(t.appsrc.get(), t.queue.get(), t.valve.get(), t.payloader.get(), nullptr)) {
        if (hooks_.on_error) hooks_.on_error("cannot link track branch " + t.track_id);
        return false;
    }
    glib::GstPadPtr pay_src = glib::adopt_pad(gst_element_get_static_pad(t.payloader.get(), "src"));
    if (gst_pad_link(pay_src.get(), t.sink_pad.get()) != GST_PAD_LINK_OK) {
        if (hooks_.on_error) hooks_.on_error("cannot link payloader to webrtcbin");
        return false;
    }
    auto sink = std::make_shared<AppSrcSink>();
    sink->appsrc = glib::ref_element(t.appsrc.get());
    sink->label = p + "/appsrc";
    t.sink = sink;
    t.pay_counter = std::make_unique<PadCounter>(pay_src.get(), p + "/payloader:src");
    // PLI/FIR from the peer arrive as upstream force-key-unit events; appsrc is the end of
    // this pipeline, so they must be relayed to the producer through the hub (docs/23 keyframe policy).
    // The probe is rebuilt with the branch, so the id it relays is always the current track's.
    {
        struct KeyUnitContext {
            std::weak_ptr<bool> alive;
            std::function<void(std::function<void()>)> post;
            ConsumerPipeline* me;
            std::string track_id;
        };
        glib::GstPadPtr appsrc_src = glib::adopt_pad(gst_element_get_static_pad(t.appsrc.get(), "src"));
        auto* boxed = new KeyUnitContext{alive_, post_, this, t.track_id};
        t.keyunit_probe = glib::PadProbe(
            appsrc_src.get(), GST_PAD_PROBE_TYPE_EVENT_UPSTREAM,
            [](GstPad*, GstPadProbeInfo* info, gpointer d) -> GstPadProbeReturn {
                GstEvent* ev = GST_PAD_PROBE_INFO_EVENT(info);
                if (gst_video_event_is_force_key_unit(ev)) {
                    const auto* ctx = static_cast<const KeyUnitContext*>(d);
                    ctx->post([me = ctx->me, id = ctx->track_id, alive = ctx->alive] {
                        if (alive.expired()) return;
                        if (me->hooks_.on_keyframe_request) me->hooks_.on_keyframe_request(id);
                    });
                    return GST_PAD_PROBE_DROP; // consumed here: appsrc would only warn
                }
                return GST_PAD_PROBE_OK;
            },
            boxed, [](gpointer d) { delete static_cast<KeyUnitContext*>(d); });
    }
    for (auto* e : {t.appsrc.get(), t.queue.get(), t.valve.get(), t.payloader.get()}) gst_element_sync_state_with_parent(e);
    return true;
}

void ConsumerPipeline::teardown_branch(ConsumerTrack& t) {
    if (!t.appsrc) return;
    t.keyunit_probe = glib::PadProbe();
    t.pay_counter.reset();
    t.sink.reset(); // the hub subscription was dropped by the session before this
    for (auto* e : {t.payloader.get(), t.valve.get(), t.queue.get(), t.appsrc.get()}) gst_element_set_state(e, GST_STATE_NULL);
    // Removal unlinks every pad, including payloader → webrtcbin.sink_%u; the transceiver stays.
    gst_bin_remove_many(GST_BIN(pipeline_.get()), t.appsrc.get(), t.queue.get(), t.valve.get(), t.payloader.get(), nullptr);
    t.appsrc.reset();
    t.queue.reset();
    t.valve.reset();
    t.payloader.reset();
}

void ConsumerPipeline::remove_track(const std::string& track_id) {
    auto it = tracks_.find(track_id);
    if (it == tracks_.end()) return;
    ConsumerTrack* t = it->second.get();
    g_object_set(t->valve.get(), "drop", TRUE, nullptr); // valve first (docs/23)
    t->enabled = false;
    if (t->transceiver) g_object_set(t->transceiver.get(), "direction", GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_INACTIVE, nullptr);
    t->pooled = true;
    // Only the transceiver (and its m-line) is pooled; the branch elements go, so the
    // introspection grammar never shows a track that left the manifest.
    std::unique_ptr<ConsumerTrack> moved = std::move(it->second);
    tracks_.erase(it);
    teardown_branch(*moved);
    const std::string pool_id = "_pool:" + std::to_string(moved->mline);
    moved->track_id = pool_id;
    tracks_[pool_id] = std::move(moved);
}

ConsumerTrack* ConsumerPipeline::track(const std::string& track_id) {
    auto it = tracks_.find(track_id);
    return it == tracks_.end() ? nullptr : it->second.get();
}

std::vector<protocol::ManifestEntry> ConsumerPipeline::manifest() const {
    std::vector<const ConsumerTrack*> ordered;
    for (const auto& [_, t] : tracks_)
        if (!t->pooled) ordered.push_back(t.get());
    std::sort(ordered.begin(), ordered.end(), [](const ConsumerTrack* a, const ConsumerTrack* b) { return a->mline < b->mline; });
    std::vector<protocol::ManifestEntry> out;
    for (const auto* t : ordered) {
        protocol::ManifestEntry e;
        e.track_id = t->track_id;
        e.cap = t->cap;
        e.kind = t->kind;
        e.label = t->label;
        e.codec = t->kind == TrackKind::Video ? "H264" : "OPUS";
        e.pt = t->pt;
        e.mid = t->mid;
        e.monitor = t->monitor;
        out.push_back(std::move(e));
    }
    return out;
}

void ConsumerPipeline::create_offer() {
    if (offer_in_flight_ || stopped_) return;
    offer_in_flight_ = true;
    if (GST_STATE(pipeline_.get()) < GST_STATE_PLAYING) gst_element_set_state(pipeline_.get(), GST_STATE_PLAYING);
    std::weak_ptr<bool> alive = alive_;
    GstPromise* promise = glib::make_promise([this, alive](GstPromiseResult res, glib::GstStructurePtr reply) {
        GstWebRTCSessionDescription* offer = nullptr;
        if (res == GST_PROMISE_RESULT_REPLIED && reply) gst_structure_get(reply.get(), "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, nullptr);
        glib::SdpPtr sdp(offer);
        std::shared_ptr<glib::SdpPtr> shared = std::make_shared<glib::SdpPtr>(std::move(sdp));
        post_([this, alive, shared] {
            if (alive.expired()) return;
            if (!*shared) {
                offer_in_flight_ = false;
                if (hooks_.on_error) hooks_.on_error("create-offer failed");
                return;
            }
            milestone("offer-created");
            GstWebRTCSessionDescription* d = shared->get();
            // mids from the offer SDP, by m-line index (spike Q2).
            for (auto& [_, t] : tracks_) {
                if (t->mline < gst_sdp_message_medias_len(d->sdp)) {
                    const GstSDPMedia* m = gst_sdp_message_get_media(d->sdp, t->mline);
                    const gchar* mid = gst_sdp_media_get_attribute_val(m, "mid");
                    if (mid) t->mid = mid;
                }
            }
            std::weak_ptr<bool> alive2 = alive_;
            GstPromise* p2 = glib::make_promise([this, alive2, shared](GstPromiseResult, glib::GstStructurePtr) {
                post_([this, alive2, shared] {
                    if (alive2.expired()) return;
                    milestone("local-description-set");
                    remote_described_ = false;
                    glib::GStrPtr text(gst_sdp_message_as_text(shared->get()->sdp));
                    if (hooks_.on_offer) hooks_.on_offer(text.get());
                });
            });
            g_signal_emit_by_name(webrtc_.get(), "set-local-description", d, p2);
        });
    });
    g_signal_emit_by_name(webrtc_.get(), "create-offer", nullptr, promise);
}

void ConsumerPipeline::set_remote_answer(const std::string& sdp_text) {
    GstSDPMessage* msg = nullptr;
    if (gst_sdp_message_new_from_text(sdp_text.c_str(), &msg) != GST_SDP_OK) {
        if (hooks_.on_error) hooks_.on_error("answer SDP unparseable");
        return;
    }
    glib::SdpPtr answer(gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_ANSWER, msg));
    milestone("answer-received");
    std::weak_ptr<bool> alive = alive_;
    GstPromise* promise = glib::make_promise([this, alive](GstPromiseResult, glib::GstStructurePtr) {
        post_([this, alive] {
            if (alive.expired()) return;
            offer_in_flight_ = false;
            remote_described_ = true;
            milestone("remote-description-set");
            for (auto& [mline, cand] : ice_queue_) add_ice_candidate(mline, cand);
            ice_queue_.clear();
            // mids are final after the answer (spike Q2): refresh from the transceivers.
            for (auto& [_, t] : tracks_) {
                if (!t->transceiver) continue;
                const std::string mid = glib::str_prop(t->transceiver.get(), "mid");
                if (!mid.empty()) t->mid = mid;
            }
        });
    });
    g_signal_emit_by_name(webrtc_.get(), "set-remote-description", answer.get(), promise);
}

void ConsumerPipeline::add_ice_candidate(unsigned mline, const std::string& candidate) {
    if (candidate.empty()) return; // end-of-candidates: nothing to add on webrtcbin
    if (!remote_described_) {
        ice_queue_.emplace_back(mline, candidate);
        return;
    }
    g_signal_emit_by_name(webrtc_.get(), "add-ice-candidate", mline, candidate.c_str());
}

void ConsumerPipeline::set_enabled(const std::string& track_id, bool enabled) {
    ConsumerTrack* t = track(track_id);
    if (!t || !t->valve) return; // pooled: no branch
    t->enabled = enabled;
    g_object_set(t->valve.get(), "drop", enabled ? FALSE : TRUE, nullptr);
}

std::shared_ptr<FrameSink> ConsumerPipeline::sink_for(const std::string& track_id) {
    ConsumerTrack* t = track(track_id);
    return t ? t->sink : nullptr;
}

void ConsumerPipeline::get_stats(std::function<void(StatsSample)> cb) {
    // Not webrtcbin's `get-stats`: in 1.28.2 its `_get_data_channel_transport_stats` takes the RTP
    // session element through rtpbin's `get-session` and never releases it, one reference per
    // call — a session that sampled stats for 18 s kept 18 references and ~18 MB after it closed
    // (found by the 3c lab tests and a refcount trace). rtpbin's per-source counters carry what
    // the per-second sample needs and are read synchronously on the loop, no promise, no thread.
    StatsSample sample;
    std::map<std::uint32_t, std::string> by_ssrc;
    for (const auto& [_, t] : tracks_) by_ssrc[t->ssrc] = t->track_id;
    glib::GstElementPtr rtpbin = glib::adopt_element(webrtc_ ? gst_bin_get_by_name(GST_BIN(webrtc_.get()), "rtpbin") : nullptr); // transfer full
    for (guint sid = 0; rtpbin && sid < 16; sid++) {
        GstElement* raw = nullptr;
        g_signal_emit_by_name(rtpbin.get(), "get-session", sid, &raw); // transfer full
        if (!raw) break;
        glib::GstElementPtr session = glib::adopt_element(raw);
        GstStructure* st = nullptr;
        g_object_get(session.get(), "stats", &st, nullptr);
        glib::GstStructurePtr stats(st);
        const GValue* sources = stats ? gst_structure_get_value(stats.get(), "source-stats") : nullptr;
        if (!sources || !G_VALUE_HOLDS(sources, G_TYPE_VALUE_ARRAY)) continue;
        G_GNUC_BEGIN_IGNORE_DEPRECATIONS // GValueArray is deprecated in GLib; rtpsession's stats still use it
        auto* arr = static_cast<GValueArray*>(g_value_get_boxed(sources));
        for (guint i = 0; arr && i < arr->n_values; i++) {
            const GValue* v = g_value_array_get_nth(arr, i);
            if (!GST_VALUE_HOLDS_STRUCTURE(v)) continue;
            const GstStructure* src = gst_value_get_structure(v);
            gboolean internal = FALSE, sender = FALSE;
            guint ssrc = 0;
            guint64 octets = 0, packets = 0;
            gst_structure_get(src, "internal", G_TYPE_BOOLEAN, &internal, "is-sender", G_TYPE_BOOLEAN, &sender, "ssrc", G_TYPE_UINT, &ssrc, nullptr);
            if (!internal || !sender) continue;
            gst_structure_get(src, "octets-sent", G_TYPE_UINT64, &octets, "packets-sent", G_TYPE_UINT64, &packets, nullptr);
            auto it = by_ssrc.find(ssrc);
            if (it != by_ssrc.end()) sample.tracks.push_back({it->second, octets, packets});
        }
        G_GNUC_END_IGNORE_DEPRECATIONS
    }
    cb(std::move(sample));
}

std::string ConsumerPipeline::connection_state() const {
    return webrtc_ ? glib::enum_prop_nick(webrtc_.get(), "connection-state") : "closed";
}

void ConsumerPipeline::stop() {
    if (stopped_) return;
    stopped_ = true;
    *alive_ = false;
    alive_ = std::make_shared<bool>(true); // detach every pending callback
    // The appsrc refs held by the sinks stay: a delivery in flight on the hub thread pushes into
    // a NULL-state appsrc and gets FLUSHING, instead of racing a reset (the sink dies with the track).
    for (auto& [_, t] : tracks_)
        if (t->valve) g_object_set(t->valve.get(), "drop", TRUE, nullptr);
    signals_.clear();
    channels_.clear();
    if (pipeline_) {
        gst_element_set_state(pipeline_.get(), GST_STATE_NULL);
        bus_watch_.cancel();
    }
    tracks_.clear();
    webrtc_.reset();
    if (pipeline_) {
        pipeline_.reset();
        glib::ObjectCensus::instance().pipelines--;
    }
}

} // namespace fjarr::media
