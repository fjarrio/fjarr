/**
 * fjarr.audio talking (M3.5 3.5.3; docs/08#talk, docs/23#audio-uplink): the uplink is offered only
 * with `talk` and a speaker, one session talks at a time, audio is played only between talk-start
 * and talk-stop, after the volume cap, and every end of a session closes the gate — audited.
 */
#include <cmath>
#include <vector>

#include <gtest/gtest.h>

#include <fjarr/audio_capability.hpp>

#include "core/loop.hpp"
#include "media/sources.hpp"

#include "recording_context.hpp"

using namespace fjarr;

namespace {
struct Sources {
    fjarr::CoreLoop loop;
    fjarr::media::SourceRegistry reg;
    Sources() : reg(loop.context()) {}
};

Envelope request(const std::string& type) {
    Envelope e;
    e.cap = "fjarr.audio";
    e.type = type;
    e.kind = "request";
    e.event_id = "ev-" + type;
    e.payload = nlohmann::json::object();
    return e;
}

const nlohmann::json NULL_SPEAKER = {{"speaker", {{"output", "null"}, {"max_volume", 0.5}}}};
const nlohmann::json TALK = {{"talk", true}};

/// 20 ms of a full-scale square wave: RMS and peak 1.0 before the cap.
std::vector<std::int16_t> loud() { std::vector<std::int16_t> v(960); for (std::size_t i = 0; i < v.size(); i++) v[i] = i % 2 ? 32767 : -32768; return v; }

std::string code(const fjarr::testing::RecordingContext& ctx) { return ctx.sent.back().payload["error"].value("code", ""); }
bool ok(const fjarr::testing::RecordingContext& ctx) { return ctx.sent.back().kind == "result" && ctx.sent.back().payload.value("ok", false); }

struct Rig {
    AudioCapability cap;
    Sources sources;
    explicit Rig(const nlohmann::json& config = NULL_SPEAKER) { cap.configure(config, sources.reg); }
    ~Rig() { cap.shutdown(); }
};
} // namespace

TEST(AudioTalk, theUplinkIsOfferedOnlyWithTalkAndASpeaker) {
    Rig with;
    fjarr::testing::RecordingContext granted, ungranted;
    ungranted.sid = "01a0-other";
    with.cap.session_attached(granted, TALK);
    with.cap.session_attached(ungranted, nlohmann::json{{"listen", true}});
    EXPECT_EQ(granted.inbound.size(), 1u);
    EXPECT_TRUE(ungranted.inbound.empty()) << "docs/08#talk: a session without talk is offered no uplink";
    with.cap.on_message(ungranted, request("talk-start"));
    EXPECT_EQ(code(ungranted), "capability-denied");

    Rig without(nlohmann::json::object());
    fjarr::testing::RecordingContext ctx;
    without.cap.session_attached(ctx, TALK);
    EXPECT_TRUE(ctx.inbound.empty()) << "no speaker, no uplink";
    without.cap.on_message(ctx, request("talk-start"));
    EXPECT_EQ(code(ctx), "unavailable");
}

TEST(AudioTalk, audioIsPlayedOnlyBetweenTalkStartAndTalkStopAndAfterTheCap) {
    Rig rig;
    fjarr::testing::RecordingContext ctx;
    ctx.op = {"op-1", "Ada"};
    rig.cap.session_attached(ctx, TALK);
    ASSERT_EQ(ctx.inbound.size(), 1u);
    const auto pcm = loud();
    // Before talk-start: dropped, never played.
    ctx.inbound[0].on_pcm(pcm.data(), pcm.size());
    rig.cap.on_message(ctx, request("talk-start"));
    ASSERT_TRUE(ok(ctx)) << ctx.sent.back().payload.dump();
    ASSERT_EQ(ctx.timers.size(), 1u);
    ctx.fire_timer();
    EXPECT_EQ(ctx.sent.back().type, "talk-level");
    EXPECT_EQ(ctx.sent.back().payload.value("frames", -1), 0) << "audio sent before talk-start is never played";
    for (int i = 0; i < 5; i++) ctx.inbound[0].on_pcm(pcm.data(), pcm.size());
    ctx.fire_timer();
    const auto level = ctx.sent.back().payload;
    EXPECT_EQ(level.value("frames", -1), 5);
    // Full scale under a 0.5 cap is -6 dBFS: the cap is what reaches the speaker.
    EXPECT_NEAR(level.value("peak_db", 0.0), 20 * std::log10(0.5), 0.2);
    EXPECT_NEAR(level.value("rms_db", 0.0), 20 * std::log10(0.5), 0.2);
    rig.cap.on_message(ctx, request("talk-stop"));
    EXPECT_TRUE(ok(ctx));
    ctx.inbound[0].on_pcm(pcm.data(), pcm.size()); // after talk-stop: dropped
    ASSERT_EQ(ctx.session_events.size(), 2u);
    EXPECT_EQ(ctx.session_events[0], (std::pair<std::string, std::string>{"audio-uplink", "talk-start"}));
    EXPECT_EQ(ctx.session_events[1], (std::pair<std::string, std::string>{"audio-uplink", "talk-stop"}));
    rig.cap.on_message(ctx, request("talk-stop"));
    EXPECT_TRUE(ok(ctx)) << "talk-stop is idempotent";
    EXPECT_EQ(ctx.session_events.size(), 2u) << "and audits only a real stop";
}

TEST(AudioTalk, oneSessionTalksAtATimeAndASecondIsToldWho) {
    Rig rig;
    fjarr::testing::RecordingContext a, b;
    a.op = {"op-a", "Ada"};
    b.sid = "01a0-second";
    b.op = {"op-b", "Bo"};
    rig.cap.session_attached(a, TALK);
    rig.cap.session_attached(b, TALK);
    rig.cap.on_message(a, request("talk-start"));
    ASSERT_TRUE(ok(a));
    rig.cap.on_message(a, request("talk-start"));
    EXPECT_TRUE(ok(a)) << "talk-start is idempotent for the session that talks";
    rig.cap.on_message(b, request("talk-start"));
    EXPECT_EQ(code(b), "busy");
    EXPECT_EQ(b.sent.back().payload["error"]["data"]["holder"].value("label", ""), "Ada");
    const auto pcm = loud();
    b.inbound[0].on_pcm(pcm.data(), pcm.size()); // the other session's audio: not played
    a.fire_timer();
    EXPECT_EQ(a.sent.back().payload.value("frames", -1), 0);
    rig.cap.on_message(a, request("talk-stop"));
    rig.cap.on_message(b, request("talk-start"));
    EXPECT_TRUE(ok(b)) << "the floor is free once the first stops";
}

TEST(AudioTalk, aSessionThatEndsWhileTalkingClosesTheGateAndSaysSo) {
    // docs/15 safety: every end of a session closes the gate, whatever the reason.
    Rig rig;
    fjarr::testing::RecordingContext a, b;
    b.sid = "01a0-second";
    rig.cap.session_attached(a, TALK);
    rig.cap.session_attached(b, TALK);
    rig.cap.on_message(a, request("talk-start"));
    ASSERT_TRUE(ok(a));
    rig.cap.session_detached(a.sid, DetachReason::PeerGone, "");
    ASSERT_EQ(a.session_events.size(), 2u);
    EXPECT_EQ(a.session_events.back().second, "talk-stop");
    rig.cap.on_message(b, request("talk-start"));
    EXPECT_TRUE(ok(b)) << "the floor was freed by the session's end";
}
