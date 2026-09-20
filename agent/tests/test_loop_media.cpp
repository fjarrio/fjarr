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
