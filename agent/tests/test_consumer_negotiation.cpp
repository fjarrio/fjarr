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
#include <vector>

#include <gst/sdp/sdp.h>
#include <gst/webrtc/webrtc.h>
#include <gtest/gtest.h>

#include "core/loop.hpp"
#include "media/consumer.hpp"
#include "media/encoder.hpp"
#include "media/frame_hub.hpp"
#include "media/producer.hpp"
#include "media/sources.hpp"
#include "loopback_peer.hpp"
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

/// A producer feeding the hub, a consumer with track "a" negotiated and flowing to a connected
/// loopback peer — the state a real session is in when a camera is hot-plugged.
struct Harness {
    fjarr::CoreLoop loop;
    FrameHub hub{61};
    std::unique_ptr<Producer> producer;
    std::unique_ptr<ConsumerPipeline> consumer;
    fjarr::testing::LoopbackPeer peer;
    std::mutex m;
    std::string last_offer;
    int offers = 0;
    std::atomic<bool> connected{false};
    std::vector<std::string> errors; // the consumer's pipeline errors (under m)

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
        hooks.on_error = [this](const std::string& e) {
            std::lock_guard<std::mutex> l(m);
            errors.push_back(e);
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

TEST(ConsumerNegotiation, anEnableThatOvertakesItsAnswerIsHeldAndARemovalMeanwhileReturns) {
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

TEST(ConsumerNegotiation, aHeldEnableOpensWhenItsAnswerLands) {
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

TEST(ConsumerNegotiation, aTrackTheAnswerRejectedNeverOpensAndStillRemovesCleanly) {
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

// A track that leaves and comes back reuses its pooled transceiver, and with it its SSRC, under a
// new payloader. One that started its sequence numbers anywhere below the old one's made libsrtp
// refuse the packets as replays, and the session ended with a media error: a desktop resumed after
// GNOME's stop on the mini-PC, 2026-10-03. The new payloader continues where the old one stopped.
TEST(ConsumerNegotiation, aTrackThatComesBackContinuesItsSequenceNumbersUnderTheSameSsrc) {
    Harness h(true);
    ASSERT_TRUE(h.start()) << "the loopback never connected";
    std::this_thread::sleep_for(std::chrono::milliseconds(300)); // packets out, under SRTP
    guint last = 0, ssrc_before = 0;
    h.loop.call_sync([&] {
        auto* t = h.consumer->track("a");
        g_object_get(t->payloader.get(), "seqnum", &last, nullptr);
        ssrc_before = t->ssrc;
        h.hub.unsubscribe(HubKey{"t", "active"}, h.consumer->sink_for("a"));
        h.consumer->remove_track("a");
        h.consumer->create_offer();
    });
    ASSERT_TRUE(h.answer_next());
    guint offset = 0, ssrc_after = 0;
    h.loop.call_sync([&] {
        fjarr::TrackSpec a;
        a.track_id = "a";
        auto* t = h.consumer->add_track(a, "fjarr.camera");
        ASSERT_NE(t, nullptr);
        ssrc_after = t->ssrc;
        g_object_get(t->payloader.get(), "seqnum-offset", &offset, nullptr);
        h.consumer->create_offer();
    });
    ASSERT_TRUE(h.answer_next());
    EXPECT_EQ(ssrc_after, ssrc_before) << "the pooled transceiver keeps its SSRC";
    // Just ahead of the old sequence: packets still in flight when `last` was read pass the old
    // payloader before it goes, so not exactly last + 1 (CI, 2026-10-03), but never behind it.
    const guint ahead = (offset - last) & 0xffff;
    EXPECT_GE(ahead, 1u) << "offset " << offset << ", last " << last;
    EXPECT_LE(ahead, 100u) << "offset " << offset << ", last " << last;
    h.loop.call_sync([&] {
        h.consumer->set_enabled("a", true);
        h.hub.subscribe(HubKey{"t", "active"}, h.consumer->sink_for("a"));
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    {
        std::lock_guard<std::mutex> l(h.m);
        EXPECT_TRUE(h.errors.empty()) << h.errors.front();
    }
    h.stop();
}
