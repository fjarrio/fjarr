// Passthrough (docs/06, slice 6b): a source's elementary output is parsed and packetized, never
// transcoded; its tiers are the camera's own streams, and without a substream the track has one
// tier and cannot adapt to a viewer's link (docs/23#rate-control-and-tier-switching).
#include <gtest/gtest.h>

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
