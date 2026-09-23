// Loop test: a test-pattern producer through the real encoder path into the FrameHub.
#include <mutex>
#include <thread>

#include <gtest/gtest.h>

#include "core/loop.hpp"
#include "media/encoder.hpp"
#include "media/frame_hub.hpp"
#include "media/producer.hpp"
#include "media/sources.hpp"

namespace glib = fjarr::glib;
using namespace fjarr::media;

namespace {
struct CountingSink final : FrameSink {
    std::atomic<int> frames{0}, keyframes{0};
    bool push(glib::GstBufferPtr b, GstCaps*) override {
        frames++;
        if (!GST_BUFFER_FLAG_IS_SET(b.get(), GST_BUFFER_FLAG_DELTA_UNIT)) keyframes++;
        return true;
    }
};
} // namespace

TEST(LoopMedia, testPatternEncodesIntoTheHubAndFansOut) {
    fjarr::CoreLoop loop;
    loop.start();
    FrameHub hub(61);
    ProducerConfig cfg;
    cfg.encoder = {EncoderKind::Software, "software"};
    cfg.gop_seconds = 1;
    cfg.active_kbps = 1000;
    fjarr::SourceRef src{std::make_shared<TestPatternSource>("smpte", 640, 360, 30), "src"};
    auto producer = std::make_unique<Producer>("t", src, true, cfg, hub, loop.context());
    bool built = false;
    loop.call_sync([&] { built = producer->build() && producer->start_tier("active"); });
    ASSERT_TRUE(built) << producer->error();
    auto a = std::make_shared<CountingSink>();
    auto b = std::make_shared<CountingSink>();
    hub.subscribe(HubKey{"t", "active"}, a);
    hub.subscribe(HubKey{"t", "active"}, b);
    for (int i = 0; i < 300 && (a->frames < 15 || b->frames < 15); i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_GE(a->frames, 15);
    EXPECT_GE(b->frames, 15);
    EXPECT_GE(a->keyframes, 1);
    EXPECT_TRUE(hub.stats(HubKey{"t", "active"}).has_keyframe);
    loop.call_sync([&] { producer->request_keyframe("active"); });
    const int kf = a->keyframes;
    for (int i = 0; i < 200 && a->keyframes == kf; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_GT(a->keyframes, kf) << "force-key-unit did not produce a keyframe";
    loop.call_sync([&] {
        producer->stop_tier("active");
        EXPECT_FALSE(producer->playing());
        producer.reset();
    });
    loop.stop();
}

TEST(LoopMedia, secondTierStartsOnARunningProducerWithoutStallingTheFirst) {
    // A branch synced upstream-first pushed into an inactive encode bin, took FLUSHING and parked
    // the tee for good, with no bus error to trigger the recovery ladder.
    fjarr::CoreLoop loop;
    loop.start();
    FrameHub hub(61);
    ProducerConfig cfg;
    cfg.encoder = {EncoderKind::Software, "software"};
    cfg.gop_seconds = 1;
    cfg.active_kbps = 1000;
    cfg.thumbnail_kbps = 200;
    fjarr::SourceRef src{std::make_shared<TestPatternSource>("smpte", 640, 360, 30), "src"};
    auto producer = std::make_unique<Producer>("t", src, true, cfg, hub, loop.context());
    bool built = false;
    loop.call_sync([&] { built = producer->build() && producer->start_tier("active"); });
    ASSERT_TRUE(built) << producer->error();
    auto active = std::make_shared<CountingSink>();
    hub.subscribe(HubKey{"t", "active"}, active);
    for (int i = 0; i < 300 && active->frames < 10; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_GE(active->frames, 10);
    bool started = false;
    loop.call_sync([&] { started = producer->start_tier("thumbnail"); });
    ASSERT_TRUE(started) << producer->error();
    auto thumb = std::make_shared<CountingSink>();
    hub.subscribe(HubKey{"t", "thumbnail"}, thumb);
    const int before = active->frames;
    for (int i = 0; i < 300 && (thumb->frames < 3 || active->frames < before + 20); i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_GE(thumb->frames, 3) << "thumbnail tier never delivered";
    EXPECT_GE(active->frames, before + 20) << "active tier stalled when the second tier started";
    loop.call_sync([&] {
        producer->stop_tier("thumbnail");
        producer->stop_tier("active");
        producer.reset();
    });
    loop.stop();
}

#include "media/media_plane.hpp"
#include "media/sources.hpp"

TEST(LoopMedia, aSourceErrorDegradesItsTrackAndNeverRebuildsThePlane) {
    // docs/23 media-plane recovery (slice 4): an error from inside the source bin (a camera gone, a
    // stream unreachable) is retried on the slow ladder and reported as the track's reason; the
    // plane rebuild — which closes every session — is never requested for it.
    fjarr::CoreLoop loop;
    loop.start();
    fjarr::AgentConfig::MediaSection media;
    media.tier_grace_ms = 500;
    media.gop_seconds = 1;
    fjarr::media::SourceRegistry reg(loop.context());
    std::unique_ptr<fjarr::media::MediaPlane> plane;
    std::atomic<int> rebuilds{0}, source_failures{0};
    loop.call_sync([&] {
        plane = std::make_unique<fjarr::media::MediaPlane>(loop, media, fjarr::media::EncoderChoice{fjarr::media::EncoderKind::Software, "software"}, reg);
        plane->on_rebuild_needed([&](const std::string&) { rebuilds++; });
        plane->on_producer_event([&](const std::string&, const std::string& ev) {
            if (ev == "source-failed") source_failures++;
        });
        fjarr::TrackSpec spec;
        spec.track_id = "flaky";
        spec.label = "Flaky camera";
        // identity errors from INSIDE the source bin after 5 buffers: the classification must see it there
        spec.source = fjarr::SourceRef{reg.create("videotestsrc is-live=true ! identity error-after=5 ! capsfilter caps=\"video/x-raw,width=320,height=240\""), "src"};
        plane->register_track({spec, "fjarr.camera", false});
    });
    auto sink = std::make_shared<CountingSink>();
    plane->hub().subscribe(HubKey{"flaky", "active"}, sink);
    // Six errors would exhaust the plane ladder (5 attempts); the slow per-track ladder must carry on instead.
    for (int i = 0; i < 800 && source_failures < 2; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_GE(source_failures, 2) << "the source's error was not classified as the track's";
    EXPECT_EQ(rebuilds, 0) << "a bad source must not take the plane down";
    std::string reason;
    bool available = true;
    loop.call_sync([&] {
        for (const auto& s : plane->source_status())
            if (s.track_id == "flaky") {
                available = s.available;
                reason = s.reason;
            }
    });
    EXPECT_FALSE(available);
    EXPECT_NE(reason.find("identity"), std::string::npos) << reason; // the bus error is the reason
    plane->hub().unsubscribe(HubKey{"flaky", "active"}, sink);
    loop.call_sync([&] { plane.reset(); });
    loop.stop();
}

TEST(LoopMedia, anEncodePathErrorClimbsTheRestartLadderAndThenAsksForAPlaneRebuild) {
    // docs/23 media-plane recovery + docs/15 "pipeline error": an error from ANYWHERE BUT the
    // source bin is ours, not the camera's. The producer is restarted on the fast ladder
    // (0.5 s → 5 s, five attempts) and, when that budget is spent, the plane asks to be rebuilt —
    // which is what closes every session with `media-restart` while the robot stays online.
    // The fault is a bus ERROR posted with the PIPELINE as its source: outside the source bin, so
    // the slice-4 classification must not mistake it for a camera problem.
    fjarr::CoreLoop loop;
    loop.start();
    fjarr::AgentConfig::MediaSection media;
    media.tier_grace_ms = 500;
    media.gop_seconds = 1;
    fjarr::media::SourceRegistry reg(loop.context());
    std::unique_ptr<fjarr::media::MediaPlane> plane;
    std::atomic<int> rebuilds{0}, restarts{0}, source_failures{0};
    std::string rebuild_reason;
    std::mutex reason_mu;
    loop.call_sync([&] {
        plane = std::make_unique<fjarr::media::MediaPlane>(loop, media, fjarr::media::EncoderChoice{fjarr::media::EncoderKind::Software, "software"}, reg);
        plane->on_rebuild_needed([&](const std::string& why) {
            {
                std::lock_guard<std::mutex> g(reason_mu);
                rebuild_reason = why;
            }
            rebuilds++;
        });
        plane->on_producer_event([&](const std::string&, const std::string& ev) {
            if (ev == "producer-restart") restarts++;
            if (ev == "source-failed") source_failures++;
        });
        fjarr::TrackSpec spec;
        spec.track_id = "encodefail";
        spec.label = "Healthy source, broken encode path";
        spec.source = fjarr::SourceRef{std::make_shared<fjarr::media::TestPatternSource>("smpte", 640, 360, 30), "src"};
        plane->register_track({spec, "fjarr.camera", false});
    });
    auto sink = std::make_shared<CountingSink>();
    plane->hub().subscribe(HubKey{"encodefail", "active"}, sink);

    // Post one non-source error per live producer until the ladder escalates. The ladder's own
    // backoff (0.5 + 1 + 2 + 4 + 5 s) paces this loop; 60 s is a generous ceiling over that 12.5 s.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    int posted = 0;
    std::string diag;
    while (rebuilds == 0 && std::chrono::steady_clock::now() < deadline) {
        bool did = false;
        loop.call_sync([&] {
            auto* p = plane->producer("encodefail");
            if (!p) { diag = "no producer"; return; }
            if (!p->playing()) { diag = "producer not playing, tiers=" + std::to_string(p->tier_count()) + " err=" + p->error(); return; }
            GError* err = g_error_new_literal(GST_CORE_ERROR, GST_CORE_ERROR_FAILED, "injected encode-path failure");
            // GST_OBJECT(pipeline): not a descendant of the source bin, so error_in_source() is false.
            gst_element_post_message(GST_ELEMENT(p->pipeline()),
                                     gst_message_new_error(GST_OBJECT(p->pipeline()), err, "docs/15 fault injection"));
            g_error_free(err);
            did = true;
        });
        if (did) posted++;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    EXPECT_EQ(rebuilds, 1) << "the exhausted fast ladder must ask for exactly one plane rebuild; diag: " << diag;
    EXPECT_GE(restarts, 5) << "the producer must be retried five times before escalating (got " << restarts << " after " << posted << " errors)";
    EXPECT_EQ(source_failures, 0) << "an error outside the source bin must never be blamed on the source";
    {
        std::lock_guard<std::mutex> g(reason_mu);
        EXPECT_NE(rebuild_reason.find("encodefail"), std::string::npos) << rebuild_reason; // the track is named in the reason
    }
    plane->hub().unsubscribe(HubKey{"encodefail", "active"}, sink);
    loop.call_sync([&] { plane.reset(); });
    loop.stop();
}

TEST(LoopMedia, planeRebuildsAreCountedInsideTheTenMinuteWindow) {
    // docs/23 process model: the third plane rebuild within ten minutes is what makes the agent
    // exit 2 ("restart me") instead of rebuilding forever. The exit lives in Agent; the counter
    // it reads lives here, so this pins the counter.
    fjarr::CoreLoop loop;
    loop.start();
    fjarr::AgentConfig::MediaSection media;
    fjarr::media::SourceRegistry reg(loop.context());
    std::unique_ptr<fjarr::media::MediaPlane> plane;
    int after_one = 0, after_three = 0;
    loop.call_sync([&] {
        plane = std::make_unique<fjarr::media::MediaPlane>(loop, media, fjarr::media::EncoderChoice{fjarr::media::EncoderKind::Software, "software"}, reg);
        plane->rebuild();
        after_one = plane->rebuilds_in_window();
        plane->rebuild();
        plane->rebuild();
        after_three = plane->rebuilds_in_window();
    });
    EXPECT_EQ(after_one, 1);
    EXPECT_EQ(after_three, 3) << "three rebuilds in the window is the agent's exit-2 trigger";
    loop.call_sync([&] { plane.reset(); });
    loop.stop();
}

TEST(LoopMedia, aSourceSlowerThanTheTierCeilingStillStreamsOnBothTiers) {
    // Regression (found by the slice-7a ladder test): a tier's fps is a CEILING, not a target
    // (docs/23: the active tier is "the source size at <= 30 fps"). `videorate drop-only=true` can
    // only drop frames, so a capsfilter demanding an exact `fps/1` cannot be satisfied by a slower
    // source and the tee refuses to link — a 15 fps camera could not stream at all, on any tier.
    // Every fixture in the repo happens to run at 30 fps, which is why nothing caught it.
    fjarr::CoreLoop loop;
    loop.start();
    fjarr::AgentConfig::MediaSection media;
    media.tier_grace_ms = 500;
    media.gop_seconds = 1;
    fjarr::media::SourceRegistry reg(loop.context());
    std::unique_ptr<fjarr::media::MediaPlane> plane;
    loop.call_sync([&] {
        plane = std::make_unique<fjarr::media::MediaPlane>(loop, media, fjarr::media::EncoderChoice{fjarr::media::EncoderKind::Software, "software"}, reg);
        fjarr::TrackSpec spec;
        spec.track_id = "slowcam";
        spec.label = "A 15 fps camera";
        spec.source = fjarr::SourceRef{std::make_shared<fjarr::media::TestPatternSource>("smpte", 640, 360, 15), "src"};
        plane->register_track({spec, "fjarr.camera", false});
    });
    auto active = std::make_shared<CountingSink>();
    auto thumb = std::make_shared<CountingSink>();
    plane->hub().subscribe(HubKey{"slowcam", "active"}, active);
    plane->hub().subscribe(HubKey{"slowcam", "thumbnail"}, thumb);
    for (int i = 0; i < 600 && (active->frames < 5 || thumb->frames < 2); i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    std::string err;
    loop.call_sync([&] {
        if (auto* p = plane->producer("slowcam")) err = p->error();
    });
    EXPECT_GE(active->frames, 5) << "the active tier must accept a source below its ceiling: " << err;
    EXPECT_GE(thumb->frames, 2) << "the thumbnail tier must too (5 fps ceiling, 15 fps source): " << err;
    plane->hub().unsubscribe(HubKey{"slowcam", "active"}, active);
    plane->hub().unsubscribe(HubKey{"slowcam", "thumbnail"}, thumb);
    loop.call_sync([&] { plane.reset(); });
    loop.stop();
}
