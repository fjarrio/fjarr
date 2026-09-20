// Loop test: a test-pattern producer through the real encoder path into the FrameHub.
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
