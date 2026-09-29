// A consumer track's valve opens only once this peer's answer accepted its m-section
// (docs/23#offer-construction-and-renegotiation, docs/08#track-control). webrtcbin holds a sink pad's
// buffers until then; a streaming thread parked there held the payloader's stream lock, and
// remove_track — waiting on it on the very loop that would apply the releasing answer — never
// returned. Reproduced 2026-09-29 on the first cycle every time, both by enabling a renegotiated
// track before its answer and by enabling one the answer rejected.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include <gst/sdp/sdp.h>
#include <gst/webrtc/webrtc.h>
#include <gtest/gtest.h>

#include "core/loop.hpp"
#include "media/consumer.hpp"
#include "media/encoder.hpp"
#include "media/frame_hub.hpp"
#include "media/producer.hpp"
#include "media/sources.hpp"
#include "watchdog.hpp"

using namespace fjarr::media;

namespace {

std::atomic<int>& criticals() {
    static std::atomic<int> n{0};
    return n;
}
void count_criticals(const gchar* domain, GLogLevelFlags level, const gchar* message, gpointer) {
    if (level & G_LOG_LEVEL_CRITICAL) criticals()++;
    g_log_default_handler(domain, level, message, nullptr);
}

/// The browser's side, reduced to what negotiation needs: a webrtcbin that answers every offer,
/// accepting each new m-line with a recvonly transceiver (or, with `bundle=false`, rejecting the
/// bundle-only ones as a peer without BUNDLE does), and trickles its candidates back.
struct LoopbackPeer {
    GstElement* pipe = gst_pipeline_new("loopback-peer");
    GstElement* wb = gst_element_factory_make("webrtcbin", "loopback-peer-wb");
    std::function<void(unsigned, std::string)> on_candidate;

    explicit LoopbackPeer(bool bundle) {
        if (bundle) g_object_set(wb, "bundle-policy", GST_WEBRTC_BUNDLE_POLICY_MAX_BUNDLE, nullptr);
        g_object_set(wb, "reuse-source-pads", TRUE, nullptr); // docs/23: every webrtcbin answerer sets it
        gst_bin_add(GST_BIN(pipe), wb);
        g_signal_connect(wb, "on-ice-candidate", G_CALLBACK(+[](GstElement*, guint mline, gchar* cand, gpointer self) {
                             auto* me = static_cast<LoopbackPeer*>(self);
                             if (me->on_candidate) me->on_candidate(mline, cand);
                         }),
                         this);
        g_signal_connect(wb, "pad-added", G_CALLBACK(+[](GstElement*, GstPad* pad, gpointer bin) {
                             if (GST_PAD_DIRECTION(pad) != GST_PAD_SRC) return;
                             GstElement* sink = gst_element_factory_make("fakesink", nullptr);
                             g_object_set(sink, "async", FALSE, "sync", FALSE, nullptr);
                             gst_bin_add(GST_BIN(bin), sink);
                             gst_element_sync_state_with_parent(sink);
                             GstPad* sp = gst_element_get_static_pad(sink, "sink");
                             gst_pad_link(pad, sp);
                             gst_object_unref(sp);
                         }),
                         pipe);
        gst_element_set_state(pipe, GST_STATE_PLAYING);
    }
    ~LoopbackPeer() {
        gst_element_set_state(pipe, GST_STATE_NULL);
        gst_object_unref(pipe);
    }
    LoopbackPeer(const LoopbackPeer&) = delete;
    LoopbackPeer& operator=(const LoopbackPeer&) = delete;

    std::string answer(const std::string& offer_text) {
        GstSDPMessage* sdp = nullptr;
        if (gst_sdp_message_new_from_text(offer_text.c_str(), &sdp) != GST_SDP_OK) return "";
        GArray* have = nullptr;
        g_signal_emit_by_name(wb, "get-transceivers", &have);
        const guint n_have = have ? have->len : 0;
        if (have) g_array_unref(have);
        for (guint i = n_have; i < gst_sdp_message_medias_len(sdp); i++) {
            GstCaps* caps = gst_caps_from_string("application/x-rtp,media=video,encoding-name=H264,clock-rate=90000");
            GstWebRTCRTPTransceiver* tr = nullptr;
            g_signal_emit_by_name(wb, "add-transceiver", GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_RECVONLY, caps, &tr);
            gst_caps_unref(caps);
            if (tr) gst_object_unref(tr);
        }
        GstWebRTCSessionDescription* offer = gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_OFFER, sdp);
        GstPromise* p = gst_promise_new();
        g_signal_emit_by_name(wb, "set-remote-description", offer, p);
        gst_promise_wait(p);
        gst_promise_unref(p);
        gst_webrtc_session_description_free(offer);
        p = gst_promise_new();
        g_signal_emit_by_name(wb, "create-answer", nullptr, p);
        gst_promise_wait(p);
        GstWebRTCSessionDescription* ans = nullptr;
        if (const GstStructure* reply = gst_promise_get_reply(p)) gst_structure_get(reply, "answer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &ans, nullptr);
        gst_promise_unref(p);
        if (!ans) return "";
        p = gst_promise_new();
        g_signal_emit_by_name(wb, "set-local-description", ans, p);
        gst_promise_wait(p);
        gst_promise_unref(p);
        gchar* text = gst_sdp_message_as_text(ans->sdp);
        std::string out = text;
        g_free(text);
        gst_webrtc_session_description_free(ans);
        return out;
    }
};

/// A producer feeding the hub, a consumer with track "a" negotiated and flowing to a connected
/// loopback peer — the state a real session is in when a camera is hot-plugged.
struct Harness {
    fjarr::CoreLoop loop;
    FrameHub hub{61};
    std::unique_ptr<Producer> producer;
    std::unique_ptr<ConsumerPipeline> consumer;
    LoopbackPeer peer;
    std::mutex m;
    std::string last_offer;
    int offers = 0;
    std::atomic<bool> connected{false};

    explicit Harness(bool bundle) : peer(bundle) {}

    bool start() {
        loop.start();
        ProducerConfig cfg;
        cfg.encoder = {EncoderKind::Software, "software"};
        cfg.gop_seconds = 1;
        cfg.active_kbps = 1000;
        cfg.thumbnail_kbps = 200;
        fjarr::SourceRef src{std::make_shared<TestPatternSource>("smpte", 320, 240, 30), "src"};
        producer = std::make_unique<Producer>("t", src, true, cfg, hub, loop.context());
        ConsumerHooks hooks;
        hooks.on_offer = [this](const std::string& sdp) {
            std::lock_guard<std::mutex> l(m);
            last_offer = sdp;
            offers++;
        };
        hooks.on_ice = [this](unsigned mline, const std::string& cand) {
            if (!cand.empty()) g_signal_emit_by_name(peer.wb, "add-ice-candidate", mline, cand.c_str());
        };
        hooks.on_connection_state = [this](const std::string& st) {
            if (st == "connected") connected = true;
        };
        peer.on_candidate = [this](unsigned mline, std::string cand) {
            loop.post([this, mline, cand] {
                if (consumer) consumer->add_ice_candidate(mline, cand);
            });
        };
        bool ok = false;
        loop.call_sync([&] {
            ok = producer->build() && producer->start_tier("active");
            consumer = std::make_unique<ConsumerPipeline>(
                "negotiat-0000-0000-0000-000000000001", loop.context(), [this](std::function<void()> fn) { loop.post(std::move(fn)); },
                std::move(hooks), 1);
            ok = ok && consumer->build("all", std::nullopt, {});
            fjarr::TrackSpec a;
            a.track_id = "a";
            ok = ok && consumer->add_track(a, "fjarr.camera") != nullptr;
            consumer->create_offer();
        });
        if (!ok) return false;
        if (!answer_next()) return false;
        loop.call_sync([&] {
            consumer->set_enabled("a", true);
            hub.subscribe(HubKey{"t", "active"}, consumer->sink_for("a"));
        });
        for (int i = 0; i < 6000 && !connected; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return connected;
    }

    std::string wait_offer(int n) {
        for (int i = 0; i < 4000; i++) {
            {
                std::lock_guard<std::mutex> l(m);
                if (offers >= n) return last_offer;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return "";
    }
    int offer_count() {
        std::lock_guard<std::mutex> l(m);
        return offers;
    }
    /// Answer the newest offer and apply it on the agent side.
    bool answer_next() {
        const std::string offer = wait_offer(offer_count() == 0 ? 1 : offer_count());
        if (offer.empty()) return false;
        const std::string ans = peer.answer(offer);
        if (ans.empty()) return false;
        loop.call_sync([&] { consumer->set_remote_answer(ans); });
        // the answer is applied through webrtcbin's promise, then posted back to the loop
        for (int i = 0; i < 4000; i++) {
            bool described = false;
            loop.call_sync([&] { described = !consumer->offer_in_flight(); });
            if (described) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }
    bool valve_open(const std::string& id) {
        bool open = false;
        loop.call_sync([&] {
            if (auto* t = consumer->track(id); t && t->valve) {
                gboolean drop = TRUE;
                g_object_get(t->valve.get(), "drop", &drop, nullptr);
                open = !drop;
            }
        });
        return open;
    }
    void stop() {
        loop.call_sync([&] {
            consumer.reset();
            producer->stop_tier("active");
            producer.reset();
        });
        loop.stop();
    }
};

} // namespace

TEST(LoopMedia, anEnableThatOvertakesItsAnswerIsHeldAndARemovalMeanwhileReturns) {
    criticals() = 0;
    GLogFunc previous = g_log_set_default_handler(&count_criticals, nullptr);
    {
        Harness h(true);
        ASSERT_TRUE(h.start()) << "the loopback never connected";
        for (int i = 0; i < 20; i++) {
            std::shared_ptr<FrameSink> sink;
            const int before = h.offer_count();
            h.loop.call_sync([&] {
                fjarr::TrackSpec b;
                b.track_id = "b";
                h.consumer->add_track(b, "fjarr.camera");
                h.consumer->create_offer();
            });
            ASSERT_FALSE(h.wait_offer(before + 1).empty());
            // select-tracks arrives on fjarr:control while the answer is still on its way
            h.loop.call_sync([&] {
                h.consumer->set_enabled("b", true);
                sink = h.consumer->sink_for("b");
                h.hub.subscribe(HubKey{"t", "active"}, sink);
            });
            EXPECT_FALSE(h.valve_open("b")) << "cycle " << i << ": the valve opened before the answer";
            std::this_thread::sleep_for(std::chrono::milliseconds(20 + 5 * (i % 10)));
            {
                fjarr::testing::Watchdog dog("remove_track before the answer");
                h.loop.call_sync([&] {
                    h.hub.unsubscribe(HubKey{"t", "active"}, sink);
                    h.consumer->remove_track("b");
                });
            }
            ASSERT_TRUE(h.answer_next()) << "cycle " << i;
            h.loop.call_sync([&] { h.consumer->create_offer(); }); // the re-offer without b
            ASSERT_TRUE(h.answer_next()) << "cycle " << i;
        }
        h.stop();
    }
    g_log_set_default_handler(previous, nullptr);
    EXPECT_EQ(criticals().load(), 0);
}

TEST(LoopMedia, aHeldEnableOpensWhenItsAnswerLands) {
    Harness h(true);
    ASSERT_TRUE(h.start()) << "the loopback never connected";
    EXPECT_TRUE(h.valve_open("a")) << "the negotiated track is not flowing";
    h.loop.call_sync([&] {
        fjarr::TrackSpec b;
        b.track_id = "b";
        h.consumer->add_track(b, "fjarr.camera");
        h.consumer->create_offer();
        h.consumer->set_enabled("b", true);
    });
    ASSERT_FALSE(h.wait_offer(2).empty());
    EXPECT_FALSE(h.valve_open("b"));
    ASSERT_TRUE(h.answer_next());
    EXPECT_TRUE(h.valve_open("b")) << "the held enable did not take effect when the answer was applied";
    bool negotiated = false;
    h.loop.call_sync([&] { negotiated = h.consumer->track("b")->negotiated && !h.consumer->track("b")->rejected; });
    EXPECT_TRUE(negotiated);
    h.stop();
}

TEST(LoopMedia, aTrackTheAnswerRejectedNeverOpensAndStillRemovesCleanly) {
    // A peer without BUNDLE refuses the agent's bundle-only m-lines (port 0, no group): the first
    // m-line is accepted, a second one is rejected.
    Harness h(false);
    ASSERT_TRUE(h.start()) << "the loopback never connected";
    h.loop.call_sync([&] {
        fjarr::TrackSpec b;
        b.track_id = "b";
        h.consumer->add_track(b, "fjarr.camera");
        h.consumer->create_offer();
    });
    ASSERT_FALSE(h.wait_offer(2).empty());
    ASSERT_TRUE(h.answer_next());
    bool rejected = false;
    std::shared_ptr<FrameSink> sink;
    h.loop.call_sync([&] {
        rejected = h.consumer->track("b")->rejected;
        h.consumer->set_enabled("b", true);
        sink = h.consumer->sink_for("b");
        h.hub.subscribe(HubKey{"t", "active"}, sink);
    });
    EXPECT_TRUE(rejected) << "the answer's port-0 m-line was not recorded as rejected";
    EXPECT_FALSE(h.valve_open("b"));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    {
        fjarr::testing::Watchdog dog("remove_track of a rejected track");
        h.loop.call_sync([&] {
            h.hub.unsubscribe(HubKey{"t", "active"}, sink);
            h.consumer->remove_track("b");
        });
    }
    h.stop();
}
