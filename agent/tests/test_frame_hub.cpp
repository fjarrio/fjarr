// FrameHub: keyframe gate, PTS rebase, fan-out, ring overrun, keyframe rate limit.
#include <atomic>
#include <thread>

#include <gtest/gtest.h>

#include "media/frame_hub.hpp"

using fjarr::media::FrameHub;
using fjarr::media::FrameSink;
using fjarr::media::HubKey;
namespace glib = fjarr::glib;

namespace {
struct RecordingSink final : FrameSink {
    std::mutex m;
    std::vector<GstClockTime> pts;
    std::atomic<int> count{0};
    std::atomic<bool> accept{true};
    bool push(glib::GstBufferPtr b, GstCaps*) override {
        if (!accept) return false;
        std::lock_guard<std::mutex> lock(m);
        pts.push_back(GST_BUFFER_PTS(b.get()));
        count++;
        return true;
    }
};

glib::GstSamplePtr sample(GstClockTime pts, bool keyframe) {
    GstBuffer* b = gst_buffer_new_allocate(nullptr, 16, nullptr);
    GST_BUFFER_PTS(b) = pts;
    if (!keyframe) GST_BUFFER_FLAG_SET(b, GST_BUFFER_FLAG_DELTA_UNIT);
    GstCaps* caps = gst_caps_from_string("video/x-h264");
    GstSample* s = gst_sample_new(b, caps, nullptr, nullptr);
    gst_buffer_unref(b);
    gst_caps_unref(caps);
    return glib::adopt_sample(s);
}

void wait_for(std::function<bool()> pred, int ms = 500) {
    for (int i = 0; i < ms / 2 && !pred(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(2));
}
} // namespace

TEST(FrameHub, keyframeGateAndPtsRebase) {
    FrameHub hub(4);
    const HubKey key{"t", "active"};
    auto sink = std::make_shared<RecordingSink>();
    hub.push(key, sample(1000, true));
    hub.push(key, sample(2000, false));
    std::this_thread::sleep_for(std::chrono::milliseconds(20)); // let the (unsubscribed) deliveries drain
    hub.subscribe(key, sink); // late joiner: gets the whole ring (this GOP), keyframe first
    wait_for([&] { return sink->count >= 2; });
    hub.push(key, sample(3000, false));
    hub.push(key, sample(4000, false));
    wait_for([&] { return sink->count >= 4; });
    std::lock_guard<std::mutex> lock(sink->m);
    ASSERT_EQ(sink->pts.size(), 4u);
    EXPECT_EQ(sink->pts[0], 0u);    // the keyframe becomes zero
    EXPECT_EQ(sink->pts[1], 1000u); // the delta that was already in the ring: decodable in order
    EXPECT_EQ(sink->pts[2], 2000u); // 3000 - 1000
    EXPECT_EQ(sink->pts[3], 3000u);
}

TEST(FrameHub, subscriberWaitsForKeyframeWhenNoneRetained) {
    FrameHub hub(4);
    const HubKey key{"t", "active"};
    auto sink = std::make_shared<RecordingSink>();
    hub.subscribe(key, sink);
    hub.push(key, sample(100, false));
    hub.push(key, sample(200, false));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(sink->count, 0);
    hub.push(key, sample(300, true));
    wait_for([&] { return sink->count >= 1; });
    EXPECT_EQ(sink->count, 1);
    EXPECT_EQ(hub.subscriber_stats(key, sink).dropped, 2u);
}

TEST(FrameHub, fanOutToManySubscribersSharesTheMemory) {
    FrameHub hub(4);
    const HubKey key{"t", "active"};
    std::vector<std::shared_ptr<RecordingSink>> sinks;
    for (int i = 0; i < 5; i++) {
        sinks.push_back(std::make_shared<RecordingSink>());
        hub.subscribe(key, sinks.back());
    }
    EXPECT_EQ(hub.subscriber_count(key), 5);
    hub.push(key, sample(10, true));
    for (auto& s : sinks) wait_for([&] { return s->count >= 1; });
    for (auto& s : sinks) EXPECT_EQ(s->count, 1);
    hub.unsubscribe(key, sinks[0]);
    EXPECT_EQ(hub.subscriber_count(key), 4);
}

TEST(FrameHub, rejectedPushResyncsAtTheNextKeyframe) {
    FrameHub hub(4);
    const HubKey key{"t", "active"};
    auto sink = std::make_shared<RecordingSink>();
    hub.subscribe(key, sink);
    hub.push(key, sample(1, true));
    wait_for([&] { return sink->count >= 1; });
    sink->accept = false;
    hub.push(key, sample(2, false));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    sink->accept = true;
    hub.push(key, sample(3, false)); // skipped: waiting for a keyframe again
    hub.push(key, sample(4, true));
    wait_for([&] { return sink->count >= 2; });
    EXPECT_EQ(sink->count, 2);
}

TEST(FrameHub, keyframeRequestsAreForwardedPerKeyOnSubscribeAndResync) {
    FrameHub hub(4);
    std::vector<std::string> requests;
    hub.on_keyframe_request([&](const HubKey& k) { requests.push_back(k.str()); });
    const HubKey key{"t", "active"};
    auto sink = std::make_shared<RecordingSink>();
    hub.subscribe(key, sink); // a new subscriber asks for a keyframe
    hub.resync(key, sink);    // and so does a resync (the media plane rate-limits and defers)
    EXPECT_EQ(requests, (std::vector<std::string>{"t:active", "t:active"}));
}

TEST(FrameHub, subscribingBetweenPushAndDeliveryNeverDuplicatesTheKeyframe) {
    FrameHub hub(4);
    const HubKey key{"t", "active"};
    auto sink = std::make_shared<RecordingSink>();
    hub.push(key, sample(1000, true)); // still in the delivery queue…
    hub.subscribe(key, sink);          // …when the late joiner arrives with the retained copy
    hub.push(key, sample(2000, false));
    wait_for([&] { return sink->count >= 2; });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    std::lock_guard<std::mutex> lock(sink->m);
    ASSERT_EQ(sink->pts.size(), 2u);
    EXPECT_EQ(sink->pts[0], 0u);
    EXPECT_EQ(sink->pts[1], 1000u);
}

