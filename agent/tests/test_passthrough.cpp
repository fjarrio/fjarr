// Passthrough (docs/06, slice 6b): a source's elementary output is parsed and packetized, never
// transcoded; its tiers are the camera's own streams, and without a substream the track has one
// tier and cannot adapt to a viewer's link (docs/23#rate-control-and-tier-switching).
#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include <fjarr/errors.hpp>

#include "core/loop.hpp"
#include "media/media_plane.hpp"
#include "media/sources.hpp"

using namespace fjarr;
using namespace fjarr::media;

namespace {
std::unique_ptr<VideoSource> make(SourceRegistry& reg, const nlohmann::json& cfg) { return reg.create(cfg); }
} // namespace

TEST(Passthrough, anRtspSourceDeclaresElementaryOutputsAndOnlyDepayloadsAndParses) {
    SourceRegistry reg;
    auto decoded = make(reg, nlohmann::json{{"type", "rtsp"}, {"url", "rtsp://cam/main"}});
    EXPECT_EQ(decoded->describe().outputs.size(), 1u);
    EXPECT_EQ(decoded->describe().outputs[0].declared_caps, "video/x-raw");

    auto pass = make(reg, nlohmann::json{{"type", "rtsp"}, {"url", "rtsp://cam/main"}, {"passthrough", true}});
    const auto outs = pass->describe().outputs;
    ASSERT_EQ(outs.size(), 1u); // no substream: one tier
    EXPECT_EQ(outs[0].name, "src");
    EXPECT_EQ(outs[0].declared_caps, "video/x-h264");
    const std::string d = static_cast<RtspSource*>(pass.get())->description();
    EXPECT_NE(d.find("rtph264depay"), std::string::npos);
    EXPECT_NE(d.find("h264parse"), std::string::npos);
    EXPECT_EQ(d.find("decodebin"), std::string::npos) << d; // never decoded

    auto two = make(reg, nlohmann::json{{"type", "rtsp"}, {"url", "rtsp://cam/main"}, {"thumbnail_url", "rtsp://cam/sub"}, {"passthrough", true}});
    const auto both = two->describe().outputs;
    ASSERT_EQ(both.size(), 2u);
    EXPECT_EQ(both[1].name, "thumbnail");
    EXPECT_NE(static_cast<RtspSource*>(two.get())->description("thumbnail").find("rtsp://cam/sub"), std::string::npos);

    // A substream without passthrough is a configuration error: there is nothing to put it on.
    EXPECT_THROW(make(reg, nlohmann::json{{"type", "rtsp"}, {"url", "rtsp://cam/main"}, {"thumbnail_url", "rtsp://cam/sub"}}), FjarrError);
    // A tier-1 description can declare it too (a vendor pipeline that ends in a parser).
    auto gst = make(reg, nlohmann::json{{"type", "gst"}, {"description", "fakesrc ! h264parse"}, {"passthrough", true}});
    EXPECT_EQ(gst->describe().outputs[0].declared_caps, "video/x-h264");
    EXPECT_EQ(make(reg, nlohmann::json{{"type", "gst"}, {"description", "videotestsrc"}})->describe().outputs[0].declared_caps, "video/x-raw");
}

TEST(Passthrough, aTrackWithoutASubstreamHasOneTierAndSaysItCannotAdapt) {
    CoreLoop loop;
    loop.start();
    AgentConfig::MediaSection cfg;
    SourceRegistry reg(loop.context());
    std::unique_ptr<MediaPlane> plane;
    loop.call_sync([&] { plane = std::make_unique<MediaPlane>(loop, cfg, EncoderChoice{EncoderKind::Software, "software"}, reg); });

    auto reg_track = [&](const std::string& id, const nlohmann::json& source) {
        loop.call_sync([&] {
            TrackSpec spec;
            spec.track_id = id;
            spec.label = id;
            spec.source = SourceRef{std::shared_ptr<VideoSource>(reg.create(source)), "src"};
            plane->register_track(TrackRegistration{spec, "fjarr.camera", false});
        });
    };
    reg_track("raw", nlohmann::json{{"type", "test"}});
    reg_track("lone", nlohmann::json{{"type", "rtsp"}, {"url", "rtsp://cam/main"}, {"passthrough", true}});
    reg_track("tiered", nlohmann::json{{"type", "rtsp"}, {"url", "rtsp://cam/main"}, {"thumbnail_url", "rtsp://cam/sub"}, {"passthrough", true}});

    loop.call_sync([&] {
        // A transcoded track always has both tiers and can follow a link.
        EXPECT_TRUE(plane->tier_possible("raw", "thumbnail"));
        EXPECT_TRUE(plane->adaptive("raw"));
        // Passthrough without a substream: one tier, and the manifest says so.
        EXPECT_TRUE(plane->tier_possible("lone", "active"));
        EXPECT_FALSE(plane->tier_possible("lone", "thumbnail"));
        EXPECT_FALSE(plane->adaptive("lone"));
        // With one: the viewer can be demoted to the camera's own lower stream.
        EXPECT_TRUE(plane->tier_possible("tiered", "thumbnail"));
        EXPECT_TRUE(plane->adaptive("tiered"));
        EXPECT_TRUE(plane->adaptive("nosuchtrack")); // unknown: nothing to report
    });
    loop.call_sync([&] { plane.reset(); });
    loop.stop();
}

TEST(MediaPlaneTargets, aViewerThatChangedTierStopsSteeringTheEncoderItLeft) {
    // docs/18 #40: a demoted viewer's last share in the active tier stayed until it aged out (3 s),
    // and the encoder it had left stayed at band_low for every other viewer meanwhile. Its report
    // for the new tier withdraws it from the old one at once.
    CoreLoop loop;
    loop.start();
    AgentConfig::MediaSection cfg; // active 4000 kbps, band_low 2000
    SourceRegistry reg(loop.context());
    std::unique_ptr<MediaPlane> plane;
    loop.call_sync([&] {
        plane = std::make_unique<MediaPlane>(loop, cfg, EncoderChoice{EncoderKind::Software, "software"}, reg);
        TrackSpec spec;
        spec.track_id = "t";
        spec.label = "t";
        spec.source = SourceRef{std::shared_ptr<VideoSource>(reg.create(nlohmann::json{{"type", "test"}})), "src"};
        plane->register_track(TrackRegistration{spec, "fjarr.test", false});
    });
    struct Sink : FrameSink {
        bool push(glib::GstBufferPtr, GstCaps*) override { return true; }
    };
    auto slow = std::make_shared<Sink>();
    auto fast = std::make_shared<Sink>();
    loop.call_sync([&] {
        plane->hub().subscribe(HubKey{"t", "active"}, slow);
        plane->hub().subscribe(HubKey{"t", "active"}, fast);
    });
    auto active_kbps = [&] {
        int k = -1;
        loop.call_sync([&] {
            auto* p = plane->producer("t");
            k = p && p->has_tier("active") ? p->current_kbps("active") : -1;
        });
        return k;
    };
    for (int i = 0; i < 300 && active_kbps() < 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_GT(active_kbps(), 0) << "the active tier never started";

    // The slow viewer pulls the shared encoder down to the band's floor (docs/23: as designed).
    loop.call_sync([&] {
        plane->report_allotment("t", "active", slow.get(), 1'000'000);
        plane->report_allotment("t", "active", fast.get(), 4'500'000);
    });
    for (int i = 0; i < 150 && active_kbps() != 2000; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_EQ(active_kbps(), 2000);

    // It is demoted: it now reports for the thumbnail tier. The encoder it left recovers on the next
    // target pass (500 ms), not when its old share ages out (3 s).
    const auto moved = std::chrono::steady_clock::now();
    loop.call_sync([&] {
        plane->report_allotment("t", "thumbnail", slow.get(), 300'000);
        plane->report_allotment("t", "active", fast.get(), 4'500'000);
    });
    while (active_kbps() != 4000 && std::chrono::steady_clock::now() - moved < std::chrono::seconds(4)) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(active_kbps(), 4000);
    EXPECT_LT(std::chrono::steady_clock::now() - moved, std::chrono::milliseconds(1500)) << "the old share steered the encoder until it aged out";

    loop.call_sync([&] {
        plane->hub().unsubscribe(HubKey{"t", "active"}, slow);
        plane->hub().unsubscribe(HubKey{"t", "active"}, fast);
        plane.reset();
    });
    loop.stop();
}

TEST(MediaPlaneTargets, sharpnessLowersTheFrameRateWithTheBitrateAndMotionHoldsIt) {
    // docs/23 rate control, motion or sharpness: under sharpness each frame keeps its bits — the rate
    // follows the target down from the band's top (never below 5); a viewer's preference goes with it.
    CoreLoop loop;
    loop.start();
    AgentConfig::MediaSection cfg; // active 4000 kbps at 30 fps, band_low 2000
    SourceRegistry reg(loop.context());
    std::unique_ptr<MediaPlane> plane;
    loop.call_sync([&] {
        plane = std::make_unique<MediaPlane>(loop, cfg, EncoderChoice{EncoderKind::Software, "software"}, reg);
        TrackSpec spec;
        spec.track_id = "t";
        spec.label = "t";
        spec.source = SourceRef{std::shared_ptr<VideoSource>(reg.create(nlohmann::json{{"type", "test"}})), "src"};
        plane->register_track(TrackRegistration{spec, "fjarr.test", false});
    });
    struct Sink : FrameSink {
        bool push(glib::GstBufferPtr, GstCaps*) override { return true; }
    };
    auto a = std::make_shared<Sink>();
    auto b = std::make_shared<Sink>();
    loop.call_sync([&] {
        plane->hub().subscribe(HubKey{"t", "active"}, a);
        plane->hub().subscribe(HubKey{"t", "active"}, b);
    });
    auto fps = [&] {
        int f = -1;
        loop.call_sync([&] {
            auto* p = plane->producer("t");
            f = p && p->has_tier("active") ? p->current_max_fps("active") : -1;
        });
        return f;
    };
    auto settle = [&](int want) {
        for (int i = 0; i < 150 && fps() != want; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return fps();
    };
    for (int i = 0; i < 300 && fps() < 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_EQ(fps(), 30);

    // Motion, the default: half the bitrate, the same frame rate.
    loop.call_sync([&] {
        plane->report_allotment("t", "active", a.get(), 2'000'000);
        plane->report_allotment("t", "active", b.get(), 4'500'000);
    });
    EXPECT_EQ(settle(30), 30);
    // One viewer asks for sharpness: half the target, half the frames.
    loop.call_sync([&] { plane->report_preference("t", a.get(), true); });
    EXPECT_EQ(settle(15), 15);
    // The floor: a lone viewer far below the band still gets 5 frames a second.
    loop.call_sync([&] {
        plane->forget_allotment(b.get());
        plane->report_allotment("t", "active", a.get(), 300'000);
    });
    EXPECT_EQ(settle(5), 5);
    // The viewer goes, and its preference with it: back to motion.
    loop.call_sync([&] { plane->forget_allotment(a.get()); });
    EXPECT_EQ(settle(30), 30);

    loop.call_sync([&] {
        plane->hub().unsubscribe(HubKey{"t", "active"}, a);
        plane->hub().unsubscribe(HubKey{"t", "active"}, b);
        plane.reset();
    });
    loop.stop();
}
