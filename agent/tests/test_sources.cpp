// Slice 4: the source registry as a SourceFactory (schema-validated types, the config forms),
// the v4l2 and rtsp built-ins, and v4l2 hot-plug from a watched by-id directory on the core loop.
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>

#include <gst/gst.h>
#include <gtest/gtest.h>

#include <fjarr/errors.hpp>
#include <fjarr/video_source.hpp>

#include "core/loop.hpp"
#include "media/sources.hpp"

using namespace fjarr;
using namespace fjarr::media;

TEST(SourceRegistry, resolvesEveryConfigFormAndValidatesParams) {
    SourceRegistry reg;
    // tier 1: a description string
    auto s1 = reg.create("videotestsrc ! video/x-raw,width=320,height=240");
    EXPECT_EQ(s1->describe().identity, "videotestsrc ! video/x-raw,width=320,height=240");
    // tier 2 built-ins
    EXPECT_EQ(reg.create({{"type", "test"}, {"pattern", "ball"}})->describe().identity, "test:ball");
    EXPECT_EQ(reg.create({{"type", "v4l2"}, {"device", "/dev/video9"}})->describe().identity, "v4l2:/dev/video9");
    EXPECT_EQ(reg.create({{"type", "rtsp"}, {"url", "rtsp://cam/stream"}})->describe().identity, "rtsp:rtsp://cam/stream");
    // the built-in list is what docs/23 says
    const auto types = reg.types();
    for (const char* t : {"gst", "test", "v4l2", "rtsp"}) EXPECT_NE(std::find(types.begin(), types.end(), t), types.end()) << t;
    // errors are config errors naming the problem
    EXPECT_THROW(reg.create({{"type", "acme.stereo"}}), FjarrError);
    EXPECT_THROW(reg.create({{"type", "v4l2"}}), FjarrError);                                   // device required
    EXPECT_THROW(reg.create({{"type", "v4l2"}, {"device", "/dev/video0"}, {"format", "png"}}), FjarrError); // enum
    EXPECT_THROW(reg.create({{"type", "rtsp"}, {"url", "rtsp://x"}, {"latency", -1}}), FjarrError);      // minimum
    EXPECT_THROW(reg.create(nlohmann::json{{"no", "type"}}), FjarrError);
    // a registered type resolves like a built-in (docs/09 register_source_type)
    reg.add(SourceType{"acme.stereo", nlohmann::json{{"type", "object"}, {"required", {"serial"}}},
                       [](const nlohmann::json& p) -> std::unique_ptr<VideoSource> {
                           return std::make_unique<TestPatternSource>("smpte", 320, 240, 30, "acme:" + p.at("serial").get<std::string>());
                       }});
    EXPECT_EQ(reg.create({{"type", "acme.stereo"}, {"serial", "0123"}})->describe().identity, "acme:0123");
    EXPECT_THROW(reg.create({{"type", "acme.stereo"}}), FjarrError);
    // the public built-in factory
    EXPECT_EQ(builtin_source_factory()->types().size(), 4u);
}

TEST(V4l2Source, buildsTheDescriptionAndReportsAMissingDeviceWithItsReason) {
    V4l2Source::Params p;
    p.device = "/dev/video-nope";
    p.format = "mjpeg";
    p.width = 1280;
    p.height = 720;
    p.fps = 30;
    V4l2Source v(p);
    EXPECT_EQ(v.description(), "v4l2src device=/dev/video-nope ! capsfilter caps=\"image/jpeg,width=1280,height=720,framerate=30/1\" ! jpegdec");
    EXPECT_FALSE(v.available());
    EXPECT_NE(v.last_error().find("no such device: /dev/video-nope"), std::string::npos);
    V4l2Source::Params q;
    q.device = "usb-Acme_Cam-video-index0"; // a by-id name
    q.format = "yuyv";
    V4l2Source w(q);
    w.set_by_id_dir("/nonexistent/by-id");
    EXPECT_EQ(w.device_path(), "/nonexistent/by-id/usb-Acme_Cam-video-index0");
    EXPECT_EQ(w.description(), "v4l2src device=/nonexistent/by-id/usb-Acme_Cam-video-index0 ! capsfilter caps=\"video/x-raw,format=YUY2\"");
    EXPECT_FALSE(w.available());
    EXPECT_NE(w.last_error().find("not in /nonexistent/by-id"), std::string::npos);
    V4l2Source::Params r;
    r.device = "/dev/video0";
    EXPECT_EQ(V4l2Source(r).description(), "v4l2src device=/dev/video0 ! capsfilter caps=\"video/x-raw\""); // auto, no size: the device's preferred raw format
}

TEST(V4l2Source, hotPlugFromTheWatchedByIdDirectoryArrivesOnTheCoreLoop) {
    // The by-id tree is simulated with a temporary directory: creating and deleting the "device"
    // must flip availability and call back on the loop that owns the context (docs/23).
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / ("fjarr-byid-" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    CoreLoop loop;
    loop.start();
    V4l2Source::Params p;
    p.device = "usb-Fake_Cam-video-index0";
    auto src = std::make_unique<V4l2Source>(p, loop.context());
    src->set_by_id_dir(dir.string());
    std::atomic<int> arrivals{0}, departures{0};
    std::atomic<bool> on_loop{true};
    loop.call_sync([&] {
        src->on_availability_changed([&](bool now) {
            if (!loop.is_owner_thread()) on_loop = false;
            (now ? arrivals : departures)++;
        });
    });
    EXPECT_FALSE(src->available());
    { std::FILE* f = std::fopen((dir / p.device).c_str(), "w"); std::fclose(f); } // the device node appears
    for (int i = 0; i < 200 && arrivals == 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_EQ(arrivals, 1);
    EXPECT_TRUE(src->available());
    std::filesystem::remove(dir / p.device); // and leaves
    for (int i = 0; i < 200 && departures == 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_EQ(departures, 1);
    EXPECT_TRUE(on_loop) << "the availability callback ran off the core loop";
    loop.call_sync([&] { src.reset(); }); // monitors die on the loop that owns their context
    loop.stop();
    std::filesystem::remove_all(dir);
}

TEST(RtspSource, buildsTheDescriptionAndABinWithAGhostSrc) {
    RtspSource::Params p;
    p.url = "rtsp://cam.local:8554/stream";
    p.latency_ms = 150;
    p.protocols = "tcp";
    RtspSource r(p);
    EXPECT_EQ(r.description(), "rtspsrc location=rtsp://cam.local:8554/stream latency=150 protocols=tcp ! decodebin name=fjarr-rtsp-decode");
    EXPECT_TRUE(r.available());
    GstBin* bin = r.create_bin();
    ASSERT_NE(bin, nullptr);
    glib::GstElementPtr keep = glib::sink_element(GST_ELEMENT(bin));
    glib::GstPadPtr src = glib::adopt_pad(gst_element_get_static_pad(GST_ELEMENT(bin), "src"));
    EXPECT_NE(src, nullptr) << "the ghost src pad is there before decodebin has anything (target set on pad-added)";
    EXPECT_TRUE(GST_IS_GHOST_PAD(src.get()));
}

TEST(V4l2Source, everyFormatShapeBuildsABinWithASrcPad) {
    // The bins parse and expose `src` without a device (v4l2src opens the device at READY): a bare caps
    // string at the end of a description would fail here (the trap TestPatternSource documents).
    for (const auto& [format, w] : std::vector<std::pair<std::string, int>>{{"mjpeg", 1280}, {"yuyv", 640}, {"auto", 1280}, {"auto", 0}}) {
        V4l2Source::Params p;
        p.device = "/dev/video-nope";
        p.format = format;
        p.width = w;
        p.height = w ? w * 9 / 16 : 0;
        p.fps = w ? 30 : 0;
        V4l2Source v(p);
        GstBin* bin = v.create_bin();
        ASSERT_NE(bin, nullptr) << v.description() << ": " << v.last_error();
        glib::GstElementPtr keep = glib::sink_element(GST_ELEMENT(bin));
        glib::GstPadPtr src = glib::adopt_pad(gst_element_get_static_pad(GST_ELEMENT(bin), "src"));
        EXPECT_NE(src, nullptr) << v.description();
    }
}
