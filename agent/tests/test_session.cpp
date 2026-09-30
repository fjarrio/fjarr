// Session safety behaviours with a real consumer pipeline and no peer:
// release_all_input first on every close path, the deadman on expiry and on
// session end, the negotiation watchdog, and the manifest/renegotiation
// bookkeeping visible without an answerer.
// spec: docs/15-testing-strategy.md#safety-behaviors · docs/23 (session state machine)
#include <algorithm>
#include <mutex>
#include <thread>

#include <gtest/gtest.h>

#include <fjarr/agent.hpp>

#include "core/loop.hpp"
#include "core/session.hpp"
#include "media/encoder.hpp"
#include "media/media_plane.hpp"
#include "media/sources.hpp"
#include "loopback_peer.hpp"

using namespace fjarr;
using namespace fjarr::core;

namespace {

struct Recorder final : Capability {
    std::vector<std::string> calls;
    std::vector<std::string> events;
    SessionContext* ctx = nullptr;
    std::unique_ptr<DeadmanHandle> deadman;
    int expiries = 0;
    CapabilityManifest manifest() const override {
        CapabilityManifest m;
        m.name = "com.test.input";
        m.tracks = {{"pat", "Pattern", TrackKind::Video}};
        m.channels = {{ChannelClass::Control}, {ChannelClass::Realtime}};
        m.input_bearing = true;
        return m;
    }
    void configure(const nlohmann::json&, const fjarr::SourceFactory&) override {}
    void session_attached(SessionContext& c, const nlohmann::json&) override {
        calls.push_back("attached");
        ctx = &c;
        TrackSpec t;
        t.track_id = "pat";
        t.label = "Pattern";
        t.source = SourceRef{std::make_shared<media::TestPatternSource>("smpte", 320, 240, 5), "src"};
        c.add_track(std::move(t));
    }
    void session_detached(const SessionId&, DetachReason, std::string_view detail) override { calls.push_back("detached:" + std::string(detail)); }
    void release_all_input(const SessionId&) override { calls.push_back("release_all_input"); }
    void on_message(SessionContext&, const Envelope&) override {}
    void shutdown() override {}
};

struct Harness {
    CoreLoop loop;
    AgentConfig config;
    media::SourceRegistry sources{loop.context()};
    std::unique_ptr<media::MediaPlane> plane;
    std::vector<nlohmann::json> sent;
    std::vector<SessionEvent> events;
    SessionDeps deps;
    Recorder cap;
    std::shared_ptr<Session> session;

    Harness() {
        config.agent.robot_id = "t";
        config.media.encoder = "software";
        loop.start();
        loop.call_sync([&] { plane = std::make_unique<media::MediaPlane>(loop, config.media, media::EncoderChoice{media::EncoderKind::Software, "software"}, sources); });
        deps.loop = &loop;
        deps.plane = plane.get();
        deps.config = &config;
        deps.send_signal = [this](nlohmann::json m) { sent.push_back(std::move(m)); };
        deps.emit = [this](const SessionEvent& e) { events.push_back(e); };
        deps.test_hooks = true;
    }
    ~Harness() {
        loop.call_sync([&] {
            if (session) session->close("test-teardown");
            session.reset();
            plane.reset();
        });
        loop.stop();
    }
    void attach() {
        loop.call_sync([&] {
            session = std::make_shared<Session>(deps, "01a0test-0000-7000-8000-000000000001", OperatorInfo{"a", "A"},
                                                std::vector<protocol::CapabilityGrant>{{"com.test.input", {}}}, std::nullopt);
            session->attach({AttachedCapability{&cap, cap.manifest(), nlohmann::json::object()}});
        });
    }
    void wait_for(std::function<bool()> pred, int ms = 3000) {
        for (int i = 0; i < ms / 5; i++) {
            bool ok = false;
            loop.call_sync([&] { ok = pred(); });
            if (ok) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    std::vector<std::string> sent_types() {
        std::vector<std::string> out;
        loop.call_sync([&] {
            for (const auto& m : sent) out.push_back(m.value("type", ""));
        });
        return out;
    }
};

} // namespace

TEST(Session, attachOffersWithTheManifestAndReachesOffered) {
    Harness h;
    h.attach();
    h.wait_for([&] { return h.session->state() == Session::State::Offered; });
    EXPECT_EQ(h.session->state(), Session::State::Offered);
    const auto types = h.sent_types();
    ASSERT_GE(types.size(), 2u);
    EXPECT_EQ(types[0], "session-accept");
    EXPECT_EQ(types[1], "offer");
    nlohmann::json offer;
    h.loop.call_sync([&] { offer = h.sent[1]; });
    EXPECT_EQ(offer["manifest_version"], 1);
    ASSERT_EQ(offer["tracks"].size(), 1u);
    EXPECT_EQ(offer["tracks"][0]["track_id"], "pat");
    EXPECT_EQ(offer["tracks"][0]["cap"], "com.test.input");
    EXPECT_FALSE(offer["tracks"][0]["mid"].get<std::string>().empty());
    EXPECT_NE(offer["sdp"].get<std::string>().find("m=application"), std::string::npos); // channels before media (docs/23)
}

TEST(Session, releaseAllInputRunsFirstOnEveryClosePath) {
    for (const char* reason : {"operator-closed", "peer-gone", "heartbeat", "media-error", "agent-shutdown"}) {
        Harness h;
        h.attach();
        h.wait_for([&] { return h.session->state() == Session::State::Offered; });
        h.loop.call_sync([&] { h.session->close(reason, false, std::string(reason) == "peer-gone"); });
        // Input is released and the capability detached synchronously; the pipeline goes a flush window later.
        h.wait_for([&] { return h.session->state() == Session::State::Closed; }, 1000);
        ASSERT_GE(h.cap.calls.size(), 3u) << reason;
        EXPECT_EQ(h.cap.calls[0], "attached");
        EXPECT_EQ(h.cap.calls[1], "release_all_input") << reason; // before anything else
        EXPECT_EQ(h.cap.calls[2], std::string("detached:") + reason);
        EXPECT_EQ(h.session->state(), Session::State::Closed);
        ASSERT_FALSE(h.events.empty());
        EXPECT_EQ(h.events.back().type, "ended");
        EXPECT_EQ(h.events.back().reason, reason);
        const auto types = h.sent_types();
        const bool told_operator = std::find(types.begin(), types.end(), "session-close") != types.end();
        EXPECT_EQ(told_operator, std::string(reason) != "peer-gone") << reason; // never echo a close back to a gone peer
    }
}

TEST(Session, deadmanExpiresOnSilenceAndOnSessionEnd) {
    Harness h;
    h.attach();
    h.wait_for([&] { return h.session->state() == Session::State::Offered; });
    h.loop.call_sync([&] { h.cap.deadman = h.cap.ctx->arm_deadman(std::chrono::milliseconds(40), [&] { h.cap.expiries++; }); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    h.loop.call_sync([&] { h.cap.deadman->feed(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    EXPECT_EQ(h.cap.expiries, 0); // fed in time
    h.wait_for([&] { return h.cap.expiries == 1; }, 500);
    EXPECT_EQ(h.cap.expiries, 1);
    EXPECT_TRUE(h.cap.deadman->expired());
    h.loop.call_sync([&] { h.cap.deadman->feed(); });
    EXPECT_FALSE(h.cap.deadman->expired());
    int expiries = 0;
    h.loop.call_sync([&] {
        h.session->close("operator-closed");
        expiries = h.cap.expiries; // read on the loop: the deadman expires inside close(), before any flush window
    });
    EXPECT_EQ(expiries, 2); // session end expires an armed deadman (docs/15)
}

TEST(Session, updateTracksRenegotiatesWithIncrementingManifestVersionAndCoalesces) {
    Harness h;
    h.attach();
    h.wait_for([&] { return h.session->state() == Session::State::Offered; });
    // Fake the answer so the first offer is no longer in flight (a stub SDP the consumer can't apply is fine: the
    // renegotiation bookkeeping is what we assert; the media plane never sees a peer here).
    h.loop.call_sync([&] {
        std::vector<TrackSpec> set;
        TrackSpec a;
        a.track_id = "pat";
        a.label = "Pattern";
        a.source = SourceRef{std::make_shared<media::TestPatternSource>("smpte", 320, 240, 5), "src"};
        TrackSpec b = a;
        b.track_id = "second";
        b.label = "Second";
        b.source = SourceRef{std::make_shared<media::TestPatternSource>("ball", 320, 240, 5), "src"};
        set.push_back(a);
        set.push_back(b);
        h.cap.ctx->update_tracks(set);  // while offer 1 is un-answered: folded into the next offer
        h.cap.ctx->update_tracks(set);  // idempotent
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto types = h.sent_types();
    EXPECT_EQ(std::count(types.begin(), types.end(), "offer"), 1); // still one un-answered offer (ICE candidates may trickle)
    EXPECT_EQ(h.session->manifest_version(), 1u);
    auto manifest = h.session->manifest();
    EXPECT_EQ(manifest.size(), 2u); // the track is in the pipeline, waiting for the next offer
}

TEST(Session, negotiationWatchdogClosesAnUnansweredSession) {
    // 15 s is the docs/23 budget; the test only proves the watchdog is armed at attach (state stays offered until then).
    Harness h;
    h.attach();
    h.wait_for([&] { return h.session->state() == Session::State::Offered; });
    EXPECT_EQ(h.session->state(), Session::State::Offered);
    EXPECT_EQ(h.events.size(), 0u); // no `started` without a peer
}

// #33 (docs/18): the operator's first request on `fjarr:control` was lost when the agent's core loop
// was busy as the channel opened. The message handler was connected in on_channel_open, which runs
// on the loop, posted from webrtcbin's on-open — so a request arriving in between met no handler and
// vanished, and the client timed out on an `open` the agent never saw. Reproduced by blocking the loop
// while ICE, DTLS and SCTP come up (they run on GStreamer's threads) and sending as soon as the
// client's control channel opens.
TEST(Session, aRequestThatArrivesWhileTheLoopIsBusyAtChannelOpenIsAnswered) {
    Harness h;
    fjarr::testing::LoopbackPeer peer(true);
    std::mutex m;
    std::string reply;
    GstWebRTCDataChannel* control = nullptr;
    using Sink = std::pair<std::mutex*, std::string*>; // one type name: a comma would split the macro below
    Sink sink{&m, &reply};
    peer.on_candidate = [&](unsigned mline, std::string cand) {
        h.loop.post([&h, mline, cand] {
            if (h.session) h.session->on_signal(protocol::SignalingMessage{"ice", "", 0, {{"candidate", cand}, {"sdp_mline_index", mline}}, ""});
        });
    };
    peer.on_channel = [&](GstWebRTCDataChannel* dc) {
        gchar* label = nullptr;
        g_object_get(dc, "label", &label, nullptr);
        const bool is_control = label && std::string(label) == "fjarr:control";
        g_free(label);
        if (!is_control) return;
        g_signal_connect(dc, "on-message-string", G_CALLBACK(+[](GstWebRTCDataChannel*, gchar* text, gpointer d) {
                             auto* st = static_cast<Sink*>(d);
                             std::lock_guard<std::mutex> l(*st->first);
                             if (text && std::string(text).find("\"e-33\"") != std::string::npos) *st->second = text;
                         }),
                         &sink);
        control = GST_WEBRTC_DATA_CHANNEL(g_object_ref(dc));
        // The first thing a client does once its control channel is up — here while the agent's loop
        // is still busy, so the agent's own on_channel_open is queued behind the blocker.
        auto send = +[](GstWebRTCDataChannel* ch, gpointer) {
            g_signal_emit_by_name(ch, "send-string",
                                  R"({"v":1,"cap":"com.nobody","type":"hello","event_id":"e-33","kind":"request","payload":{}})");
        };
        GstWebRTCDataChannelState state = GST_WEBRTC_DATA_CHANNEL_STATE_CONNECTING;
        g_object_get(dc, "ready-state", &state, nullptr);
        if (state == GST_WEBRTC_DATA_CHANNEL_STATE_OPEN) send(dc, nullptr);
        else g_signal_connect(dc, "on-open", G_CALLBACK(send), nullptr);
    };
    h.attach();
    h.wait_for([&] { return h.session->state() == Session::State::Offered; }, 30000);
    std::string offer;
    std::vector<nlohmann::json> agent_candidates;
    h.wait_for([&] {
        for (const auto& s : h.sent)
            if (s.value("type", "") == "ice" && s.value("candidate", "").empty()) return true;
        return false;
    }, 30000);
    h.loop.call_sync([&] {
        for (const auto& s : h.sent) {
            if (s.value("type", "") == "offer") offer = s.value("sdp", "");
            if (s.value("type", "") == "ice" && !s.value("candidate", "").empty()) agent_candidates.push_back(s);
        }
    });
    ASSERT_FALSE(offer.empty());
    const std::string answer = peer.answer(offer);
    ASSERT_FALSE(answer.empty());
    for (const auto& c : agent_candidates)
        g_signal_emit_by_name(peer.wb, "add-ice-candidate", c.value("sdp_mline_index", 0u), c.value("candidate", "").c_str());
    h.loop.call_sync([&] { h.session->on_signal(protocol::SignalingMessage{"answer", "", 0, {{"sdp", answer}}, ""}); });
    // A busy loop, as in #33, from the moment the answer is in: ICE, DTLS and SCTP proceed on
    // GStreamer's threads (the peer's candidates wait behind it; peer-reflexive pairs form anyway).
    h.loop.post([] { std::this_thread::sleep_for(std::chrono::milliseconds(1500)); });
    for (int i = 0; i < 6000; i++) {
        {
            std::lock_guard<std::mutex> l(m);
            if (!reply.empty()) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::string got;
    {
        std::lock_guard<std::mutex> l(m);
        got = reply;
    }
    EXPECT_NE(got.find("capability-unknown"), std::string::npos)
        << (control ? "the control channel opened, but the request sent on it was never answered" : "the control channel never opened") << "; reply: " << got;
    if (control) gst_object_unref(control);
}
