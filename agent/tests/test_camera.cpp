// Slice 4: fjarr.camera turns config into tracks through the SourceFactory, refuses to start on a
// required source that is missing, and re-offers every session on hot-plug.
#include <chrono>
#include <filesystem>
#include <thread>

#include <gtest/gtest.h>

#include <fjarr/camera_capability.hpp>
#include <fjarr/errors.hpp>
#include <fjarr/session_context.hpp>

#include "core/loop.hpp"
#include "media/sources.hpp"
#include "recording_context.hpp"

using namespace fjarr;

namespace {
nlohmann::json camera_config() {
    return nlohmann::json{{"tracks",
                           {{"pattern", {{"label", "Pattern"}, {"source", {{"type", "test"}, {"pattern", "ball"}}}}},
                            {"front", {{"label", "Front"}, {"source", "videotestsrc pattern=snow ! video/x-raw,width=320,height=240"}}},
                            {"webcam", {{"label", "Webcam"}, {"source", {{"type", "v4l2"}, {"device", "/dev/video-nope"}}}}}}}};
}
} // namespace

TEST(CameraCapability, configuresTracksInConfigOrderAndHoldsBackUnavailableOnes) {
    media::SourceRegistry reg;
    CameraCapability cam;
    // the schema the agent validates config against (docs/06): tracks need a source, nothing else is accepted
    const auto schema = cam.manifest().config_schema;
    EXPECT_EQ(schema["properties"]["tracks"]["additionalProperties"]["required"], nlohmann::json::array({"source"}));
    EXPECT_FALSE(schema["additionalProperties"].get<bool>());
    cam.configure(camera_config(), reg);
    const auto tracks = cam.configured_sources();
    ASSERT_EQ(tracks.size(), 3u);
    EXPECT_EQ(tracks[0].track_id, "front"); // nlohmann orders object keys; the manifest lists what config has
    EXPECT_TRUE(tracks[0].available);
    EXPECT_EQ(tracks[1].track_id, "pattern");
    EXPECT_EQ(tracks[1].identity, "test:ball");
    EXPECT_EQ(tracks[2].track_id, "webcam");
    EXPECT_FALSE(tracks[2].available);
    EXPECT_EQ(tracks[2].identity, "v4l2:/dev/video-nope");
    EXPECT_NE(tracks[2].reason.find("no such device"), std::string::npos) << tracks[2].reason;
    const auto m = cam.manifest();
    EXPECT_EQ(m.name, "fjarr.camera");
    EXPECT_EQ(m.tracks.size(), 3u);
    EXPECT_FALSE(m.input_bearing);
}

TEST(CameraCapability, aRequiredTrackWhoseSourceIsMissingIsAStartupError) {
    media::SourceRegistry reg;
    CameraCapability cam;
    nlohmann::json cfg = camera_config();
    cfg["tracks"]["webcam"]["required"] = true;
    try {
        cam.configure(cfg, reg);
        FAIL() << "configure accepted a required, unavailable track";
    } catch (const FjarrError& e) {
        EXPECT_EQ(e.code(), "config");
        EXPECT_NE(std::string(e.message()).find("tracks.webcam is required but its source is unavailable"), std::string::npos) << e.message();
    }
    nlohmann::json bad = camera_config();
    bad["tracks"]["front"]["source"] = nlohmann::json{{"type", "nope"}};
    EXPECT_THROW(cam.configure(bad, reg), FjarrError);
}

using fjarr::testing::RecordingContext;

TEST(CameraCapability, hotPlugReoffersEverySessionWithWhatIsAvailable) {
    // A by-id directory stands in for udev; the source's callback lands on the core loop, where the
    // capability re-offers each attached session's available set (docs/23: the core diffs by id).
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / ("fjarr-cam-byid-" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    CoreLoop loop;
    loop.start();
    media::SourceRegistry reg(loop.context());
    CameraCapability cam;
    RecordingContext ctx;
    nlohmann::json cfg{{"tracks",
                        {{"pattern", {{"label", "Pattern"}, {"source", {{"type", "test"}}}}},
                         {"webcam", {{"label", "Webcam"}, {"source", {{"type", "v4l2"}, {"device", (dir / "usb-Fake-video-index0").string()}}}}}}}};
    loop.call_sync([&] {
        cam.configure(cfg, reg);
        cam.session_attached(ctx, nlohmann::json::object());
    });
    EXPECT_EQ(ctx.added, (std::vector<std::string>{"pattern", "webcam"})); // attach offers everything; the core holds back the missing one
    { std::FILE* f = std::fopen((dir / "usb-Fake-video-index0").c_str(), "w"); std::fclose(f); } // the camera arrives
    for (int i = 0; i < 300 && ctx.updates.empty(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_EQ(ctx.updates.size(), 1u);
    EXPECT_EQ(ctx.updates[0], (std::vector<std::string>{"pattern", "webcam"}));
    std::filesystem::remove(dir / "usb-Fake-video-index0"); // and leaves: re-offered without it, so the core removes it
    for (int i = 0; i < 300 && ctx.updates.size() < 2; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_EQ(ctx.updates.size(), 2u);
    EXPECT_EQ(ctx.updates[1], (std::vector<std::string>{"pattern"}));
    loop.call_sync([&] {
        cam.session_detached(ctx.sid, DetachReason::Closed, "");
        cam.shutdown();
    });
    loop.call_sync([&] { cam.configure(nlohmann::json{{"tracks", nlohmann::json::object()}}, reg); }); // drops the monitors on the loop
    loop.stop();
    std::filesystem::remove_all(dir);
}
