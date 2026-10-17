// The import list and the device-memory head (ADR-0041): which DMA-BUF formats an encoder family
// imports without a copy (a fake vapostproc feeds the vaapi rule; software imports none), the
// encode branch for a DMA-BUF source, and the producer's head chosen by the source's memory. Where
// this machine has VA-API (a developer's workstation, never CI), a real DMA-BUF from VA goes through
// module E's frames and the device-memory head into vah264enc.
// spec: docs/23-agent-core-architecture.md#desktop-capture-memory · #encoders-and-tiers
#include <gtest/gtest.h>

#include <atomic>
#include <mutex>
#include <chrono>
#include <thread>

#include <gst/allocators/gstdmabuf.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/video/video.h>

#include <fjarr/errors.hpp>

#include "core/glib/raii.hpp"
#include "core/log.hpp"
#include "core/loop.hpp"
#include "desktop-mutter/capture_memory.hpp"
#include "media/encoder.hpp"
#include "media/frame_hub.hpp"
#include "media/media_plane.hpp"
#include "media/producer.hpp"

using namespace fjarr;
using namespace fjarr::media;

namespace {

constexpr std::uint32_t XR24 = 0x34325258, AR24 = 0x34325241, XB24 = 0x34324258;

GstCaps* caps(const char* s) { return gst_caps_from_string(s); }

/// A source whose output is DMA-BUF (or not), its bin an appsrc that `feed` is handed.
class DeclaredSource final : public VideoSource {
  public:
    DeclaredSource(std::string declared, std::function<void(GstElement*)> feed = {}) : declared_(std::move(declared)), feed_(std::move(feed)) {}
    SourceInfo describe() const override { return {"test:declared", {{"src", TrackKind::Video, declared_}}}; }
    /// What a capture whose memory changed says from then on (core loop).
    void declare(std::string caps) { declared_ = std::move(caps); }
    GstBin* create_bin() override {
        GstElement* src = gst_element_factory_make("appsrc", nullptr);
        g_object_set(src, "is-live", TRUE, "format", GST_FORMAT_TIME, "do-timestamp", TRUE, "max-buffers", static_cast<guint64>(4), "leaky-type", 2, nullptr);
        GstElement* bin = gst_bin_new(nullptr);
        gst_bin_add(GST_BIN(bin), src);
        GstPad* pad = gst_element_get_static_pad(src, "src");
        gst_element_add_pad(bin, gst_ghost_pad_new("src", pad));
        gst_object_unref(pad);
        if (feed_) feed_(src);
        return GST_BIN(bin);
    }
    bool available() const override { return true; }
    void on_availability_changed(std::function<void(bool)>) override {}

  private:
    std::string declared_;
    std::function<void(GstElement*)> feed_;
};

struct CountingSink final : FrameSink {
    std::atomic<int> frames{0};
    bool push(glib::GstBufferPtr, GstCaps*) override {
        frames++;
        return true;
    }
};

bool has_element(GstElement* pipeline, const std::string& name) {
    glib::GstElementPtr e = glib::adopt_element(gst_bin_get_by_name(GST_BIN(pipeline), name.c_str()));
    return e != nullptr;
}

} // namespace

TEST(EncoderMemory, softwareImportsNoDmaBufAndNeverAsksForVapostproc) {
    bool asked = false;
    const auto list = dmabuf_imports(EncoderKind::Software, [&](const char*) {
        asked = true;
        return static_cast<GstCaps*>(nullptr);
    });
    EXPECT_TRUE(list.empty());
    EXPECT_FALSE(asked);
}

TEST(EncoderMemory, vaapiImportsTheRgbDrmFormatsOfVapostprocsDmaBufSinkCaps) {
    // radeonsi on the mini-PC (2026-10-17), trimmed: one RGB modifier, YUV ones beside it, and the
    // VA-memory and system-memory structures, which say nothing about DMA-BUF.
    std::string asked;
    const auto list = dmabuf_imports(EncoderKind::VaApi, [&](const char* factory) {
        asked = factory;
        return caps("video/x-raw(memory:VAMemory), format=(string){ NV12, BGRA }; "
                    "video/x-raw(memory:DMABuf), format=(string)DMA_DRM, drm-format=(string){ NV12:0x0200000000000901, AR24:0x0200000000000901, "
                    "XR24:0x0200000000000901, XR24, AR30:0x0200000000000901, AR24:0x0200000000000901 }; "
                    "video/x-raw, format=(string){ BGRx, NV12 }");
    });
    EXPECT_EQ(asked, "vapostproc");
    ASSERT_EQ(list.size(), 3u) << describe_imports(list);
    EXPECT_EQ(list[0], (DmaBufFormat{AR24, 0x0200000000000901ULL}));
    EXPECT_EQ(list[1], (DmaBufFormat{XR24, 0x0200000000000901ULL}));
    EXPECT_EQ(list[2], (DmaBufFormat{XR24, 0})) << "a drm-format without a modifier is LINEAR";
    EXPECT_EQ(describe_imports(list), "AR24:0x0200000000000901, XR24:0x0200000000000901, XR24");
}

TEST(EncoderMemory, aSingleDrmFormatAndNoDmaBufStructureAreBothUnderstood) {
    EXPECT_EQ(dmabuf_imports(EncoderKind::VaApi, [](const char*) { return caps("video/x-raw(memory:DMABuf), format=DMA_DRM, drm-format=XB24:0x0100000000000002"); }),
              (std::vector<DmaBufFormat>{{XB24, 0x0100000000000002ULL}}));
    EXPECT_TRUE(dmabuf_imports(EncoderKind::VaApi, [](const char*) { return caps("video/x-raw, format=BGRx"); }).empty());
    EXPECT_TRUE(dmabuf_imports(EncoderKind::VaApi, [](const char*) { return static_cast<GstCaps*>(nullptr); }).empty()) << "no vapostproc: none";
    EXPECT_EQ(describe_imports({}), "none");
}

TEST(EncoderMemory, memoryOfCapsReadsTheDmaBufFeature) {
    EXPECT_EQ(memory_of_caps("video/x-raw(memory:DMABuf),format=DMA_DRM"), SourceMemory::DmaBuf);
    EXPECT_EQ(memory_of_caps("video/x-raw"), SourceMemory::System);
    EXPECT_EQ(memory_of_caps("video/x-raw(memory:VAMemory)"), SourceMemory::System);
    EXPECT_EQ(memory_of_caps(""), SourceMemory::System);
}

TEST(EncoderMemory, theDeviceMemoryBranchTakesVaMemoryAndScalesOnlyTheThumbnailWithNoSystemMemoryStep) {
    const EncoderChoice va{EncoderKind::VaApi, "vaapi"};
    TierProfile thumb;
    thumb.tier = "thumbnail";
    thumb.width = 960;
    thumb.height = 540;
    thumb.fps = 5;
    const std::string d = encode_description(va, thumb, "p:thumbnail", SourceMemory::DmaBuf);
    EXPECT_NE(d.find("video/x-raw(memory:VAMemory),framerate=(fraction)[0/1,5/1]"), std::string::npos) << d;
    EXPECT_NE(d.find("vapostproc"), std::string::npos) << d;
    EXPECT_NE(d.find("video/x-raw(memory:VAMemory),format=NV12,width=960,height=540"), std::string::npos) << d;
    EXPECT_NE(d.find("vah264enc"), std::string::npos) << d;
    EXPECT_EQ(d.find("videoscale"), std::string::npos) << d;
    EXPECT_EQ(d.find("videoconvert"), std::string::npos) << d;
    EXPECT_EQ(d.find("video/x-raw,"), std::string::npos) << "no system-memory caps: " << d;
    // The active tier: the head's NV12 straight into the encoder.
    TierProfile active;
    active.tier = "active";
    const std::string a = encode_description(va, active, "p:active", SourceMemory::DmaBuf);
    EXPECT_EQ(a.find("vapostproc"), std::string::npos) << "only the head imports the DMA-BUF: " << a;
    EXPECT_NE(a.find("vah264enc"), std::string::npos) << a;
    // System memory keeps its branch.
    const std::string s = encode_description(va, thumb, "p:thumbnail");
    EXPECT_NE(s.find("videoscale"), std::string::npos) << s;
    // Software imports none: a DMA-BUF there is an error by name, never a silent download.
    EXPECT_THROW(encode_description({EncoderKind::Software, "software"}, thumb, "p", SourceMemory::DmaBuf), FjarrError);
}

TEST(EncoderMemory, aDmaBufSourceGetsTheDeviceMemoryHeadWithOneImporterOrNamesWhatItLacks) {
    GMainContext* ctx = g_main_context_new();
    FrameHub hub(4);
    ProducerConfig va;
    va.encoder = {EncoderKind::VaApi, "vaapi"};
    {
        Producer p("dev", SourceRef{std::make_shared<DeclaredSource>("video/x-raw(memory:DMABuf),format=DMA_DRM"), "src"}, false, va, hub, ctx);
        const bool have_va = glib::GstObjectPtr<GstElementFactory>(gst_element_factory_find("vapostproc")) != nullptr;
        if (!have_va) {
            // CI: no GPU, no VA elements. The head says what it lacks, by name.
            EXPECT_FALSE(p.build());
            EXPECT_NE(p.error().find("vapostproc is missing"), std::string::npos) << p.error();
            EXPECT_EQ(p.source_memory(), SourceMemory::DmaBuf);
        } else {
            ASSERT_TRUE(p.build()) << p.error();
            EXPECT_EQ(p.source_memory(), SourceMemory::DmaBuf);
            EXPECT_FALSE(has_element(GST_ELEMENT(p.pipeline()), "producer:dev/convert")) << "no videoconvert before the tee";
            EXPECT_TRUE(has_element(GST_ELEMENT(p.pipeline()), "producer:dev/import")) << "one vapostproc imports, before the tee";
            EXPECT_TRUE(has_element(GST_ELEMENT(p.pipeline()), "producer:dev/vacaps"));
            glib::GstElementPtr raw = glib::adopt_element(gst_bin_get_by_name(GST_BIN(p.pipeline()), "producer:dev/rawcaps"));
            ASSERT_TRUE(raw);
            GstCaps* c = nullptr;
            g_object_get(raw.get(), "caps", &c, nullptr);
            glib::GstCapsPtr held(c);
            EXPECT_EQ(glib::caps_to_string(c), "video/x-raw(memory:DMABuf), format=(string)DMA_DRM");
            EXPECT_TRUE(p.source_counters());
        }
    }
    g_main_context_unref(ctx);
}

TEST(EncoderMemory, aSystemMemorySourceKeepsVideoconvertAndI420) {
    GMainContext* ctx = g_main_context_new();
    FrameHub hub(4);
    ProducerConfig sw;
    sw.encoder = {EncoderKind::Software, "software"};
    {
        Producer p("sys", SourceRef{std::make_shared<DeclaredSource>("video/x-raw"), "src"}, false, sw, hub, ctx);
        ASSERT_TRUE(p.build()) << p.error();
        EXPECT_EQ(p.source_memory(), SourceMemory::System);
        EXPECT_TRUE(has_element(GST_ELEMENT(p.pipeline()), "producer:sys/convert")) << "system memory keeps videoconvert and I420";
        EXPECT_FALSE(has_element(GST_ELEMENT(p.pipeline()), "producer:sys/import"));
        EXPECT_TRUE(p.source_counters()) << "counted at the source's output whatever its memory";
    }
    g_main_context_unref(ctx);
}

TEST(EncoderMemory, aDmaBufSourceUnderTheSoftwareEncoderIsRefusedByNameNeverDownloaded) {
    // docs/23#desktop-capture-memory: the capability never asks for one (its import list is empty),
    // so a source that declares DMA-BUF there is an error, never a silent GPU download.
    GMainContext* ctx = g_main_context_new();
    FrameHub hub(4);
    ProducerConfig sw;
    sw.encoder = {EncoderKind::Software, "software"};
    {
        Producer p("sw", SourceRef{std::make_shared<DeclaredSource>("video/x-raw(memory:DMABuf),format=DMA_DRM"), "src"}, false, sw, hub, ctx);
        EXPECT_FALSE(p.build());
        EXPECT_EQ(p.source_memory(), SourceMemory::DmaBuf);
        EXPECT_NE(p.error().find("the source delivers DMA-BUF and the software encoder imports none"), std::string::npos) << p.error();
    }
    g_main_context_unref(ctx);
}

TEST(EncoderMemory, aSourceWhoseMemoryChangesUnderARunningProducerIsRebuiltFromItsNewCaps) {
    // docs/23#desktop-capture-memory: a capture that adopts a renegotiated memory (or is restarted
    // into another) says so in describe(), and feeds the new caps into the pipeline built for the
    // old memory. That pipeline fails to negotiate inside the source bin, the slow source ladder
    // rebuilds the producer, and the rebuild reads describe(): the new head. Under the software
    // encoder that head is refused by name, which is how this test sees it was chosen (no VA in CI).
    fjarr::log::clear_ring();
    CoreLoop loop;
    loop.start();
    AgentConfig::MediaSection media;
    media.tier_grace_ms = 500;
    media.gop_seconds = 1;
    SourceRegistry reg(loop.context());
    std::mutex mu;
    glib::GstObjectPtr<GstElement> appsrc; // the newest pipeline's
    auto source = std::make_shared<DeclaredSource>("video/x-raw", [&](GstElement* a) {
        std::lock_guard<std::mutex> lk(mu);
        appsrc.reset(GST_ELEMENT(gst_object_ref(a)));
    });
    std::unique_ptr<MediaPlane> plane;
    std::atomic<int> source_failures{0};
    loop.call_sync([&] {
        plane = std::make_unique<MediaPlane>(loop, media, EncoderChoice{EncoderKind::Software, "software"}, reg);
        plane->on_producer_event([&](const std::string&, const std::string& ev) {
            if (ev == "source-failed") source_failures++;
        });
        TrackSpec spec;
        spec.track_id = "desk-mem";
        spec.label = "A capture whose memory changes";
        spec.source = SourceRef{source, "src"};
        plane->register_track({spec, "fjarr.desktop", false});
    });
    auto push = [&](const char* caps_str) {
        glib::GstObjectPtr<GstElement> a;
        {
            std::lock_guard<std::mutex> lk(mu);
            if (appsrc) a.reset(GST_ELEMENT(gst_object_ref(appsrc.get())));
        }
        if (!a) return;
        glib::GstCapsPtr caps(gst_caps_from_string(caps_str));
        gst_app_src_set_caps(GST_APP_SRC(a.get()), caps.get());
        GstBuffer* b = gst_buffer_new_allocate(nullptr, 64 * 64 * 4, nullptr);
        gst_buffer_memset(b, 0, 0x50, 64 * 64 * 4);
        gst_app_src_push_buffer(GST_APP_SRC(a.get()), b);
    };
    auto sink = std::make_shared<CountingSink>();
    plane->hub().subscribe(HubKey{"desk-mem", "active"}, sink);
    for (int i = 0; i < 150 && sink->frames < 3; i++) {
        push("video/x-raw,format=BGRx,width=64,height=64,framerate=0/1");
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }
    ASSERT_GE(sink->frames, 3) << "the system-memory producer never encoded";
    SourceMemory before = SourceMemory::DmaBuf;
    loop.call_sync([&] {
        if (auto* p = plane->producer("desk-mem")) before = p->source_memory();
        source->declare("video/x-raw(memory:DMABuf),format=DMA_DRM"); // the reader adopted DMA-BUF
    });
    EXPECT_EQ(before, SourceMemory::System);
    bool refused = false;
    for (int i = 0; i < 150 && !refused; i++) {
        push("video/x-raw(memory:DMABuf),format=DMA_DRM,drm-format=XR24,width=64,height=64,framerate=0/1");
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
        for (const auto& line : fjarr::log::recent())
            if (line.find("producer build failed") != std::string::npos && line.find("imports none") != std::string::npos) refused = true;
    }
    EXPECT_GE(source_failures, 1) << "the old pipeline's not-negotiated error was not the source's";
    EXPECT_TRUE(refused) << "the rebuilt producer did not take the DMA-BUF head from describe()";
    plane->hub().unsubscribe(HubKey{"desk-mem", "active"}, sink);
    loop.call_sync([&] { plane.reset(); });
    loop.stop();
    {
        std::lock_guard<std::mutex> lk(mu);
        appsrc.reset();
    }
    fjarr::log::clear_ring();
}

// Real DMA-BUFs, where this machine has VA-API: vapostproc renders frames into DMA-BUFs in a format
// of the import list; they are handed to module E's Frames as mutter's buffers would be, and go
// through the device-memory head into vah264enc. Every buffer must come back to "mutter".
TEST(EncoderMemory, aVaDmaBufGoesThroughFramesAndTheDeviceHeadIntoTheEncoder) {
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    // Intel's closed driver mallocs and `operator delete`s the same pointer in vaDestroyContext
    // (iHD_drv_video.so; the same third-party defect test_supervision.cpp works around), and its
    // threads are not ours to race-check: the sanitizer presets leave VA to the release run.
    GTEST_SKIP() << "VA-API's driver is not sanitizer-clean (iHD alloc-dealloc mismatch in vaDestroyContext)";
#endif
    const auto imports = dmabuf_imports(EncoderKind::VaApi);
    if (imports.empty() || !glib::GstObjectPtr<GstElementFactory>(gst_element_factory_find("vah264enc")))
        GTEST_SKIP() << "no VA-API here (CI has no GPU): covered on the mini-PC by the nightly's scanout check";
    const DmaBufFormat f = imports.front();
    const spa_video_format sf = desktop::mutter::spa_format_of(f.drm_fourcc);
    const GstVideoFormat gf = gst_video_dma_drm_fourcc_to_format(f.drm_fourcc);
    const std::string drm = desktop::mutter::drm_format_string(f.drm_fourcc, f.modifier);
    constexpr int W = 320, H = 240, N = 8;

    // N DMA-BUFs from VA, kept for the test: mutter's buffers.
    const std::string desc = std::string("videotestsrc num-buffers=") + std::to_string(N) + " pattern=ball ! video/x-raw,format=" + gst_video_format_to_string(gf) +
                             ",width=" + std::to_string(W) + ",height=" + std::to_string(H) + " ! vapostproc ! video/x-raw(memory:DMABuf),format=DMA_DRM,drm-format=" + drm +
                             " ! appsink name=out sync=false enable-last-sample=false max-buffers=" + std::to_string(N);
    GError* err = nullptr;
    glib::GstElementPtr maker = glib::sink_element(gst_parse_launch(desc.c_str(), &err));
    if (err) {
        glib::GErrorPtr e(err);
        GTEST_SKIP() << "VA cannot render " << drm << " here: " << err->message;
    }
    GstElement* out = gst_bin_get_by_name(GST_BIN(maker.get()), "out");
    // vapostproc hands out DMA-BUF only to a peer that takes a GstVideoMeta; appsink does not say so.
    glib::GstPadPtr out_pad = glib::adopt_pad(gst_element_get_static_pad(out, "sink"));
    gst_pad_add_probe(out_pad.get(), GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM, [](GstPad*, GstPadProbeInfo* info, gpointer) -> GstPadProbeReturn {
        GstQuery* q = GST_PAD_PROBE_INFO_QUERY(info);
        if (GST_QUERY_TYPE(q) == GST_QUERY_ALLOCATION) gst_query_add_allocation_meta(q, GST_VIDEO_META_API_TYPE, nullptr);
        return GST_PAD_PROBE_OK;
    }, nullptr, nullptr);
    gst_element_set_state(maker.get(), GST_STATE_PLAYING);
    std::vector<GstSample*> samples;
    for (int i = 0; i < N; i++)
        if (GstSample* s = gst_app_sink_try_pull_sample(GST_APP_SINK(out), 5 * GST_SECOND)) samples.push_back(s);
    ASSERT_EQ(samples.size(), static_cast<std::size_t>(N));

    struct Fake {
        spa_chunk chunk{};
        spa_data data{};
        spa_buffer sb{};
        pw_buffer pw{};
    };
    std::vector<std::unique_ptr<Fake>> fakes;
    for (GstSample* s : samples) {
        GstBuffer* b = gst_sample_get_buffer(s);
        ASSERT_EQ(gst_buffer_n_memory(b), 1u) << "a single-plane RGB DMA-BUF expected";
        GstMemory* m = gst_buffer_peek_memory(b, 0);
        ASSERT_TRUE(gst_is_dmabuf_memory(m));
        const GstVideoMeta* vm = gst_buffer_get_video_meta(b);
        auto fk = std::make_unique<Fake>();
        gsize offset = 0, maxsize = 0;
        const gsize size = gst_memory_get_sizes(m, &offset, &maxsize);
        fk->data.type = SPA_DATA_DmaBuf;
        fk->data.fd = gst_dmabuf_memory_get_fd(m);
        fk->data.maxsize = static_cast<std::uint32_t>(maxsize);
        fk->data.chunk = &fk->chunk;
        fk->chunk.offset = static_cast<std::uint32_t>(offset + (vm ? vm->offset[0] : 0));
        fk->chunk.stride = vm ? vm->stride[0] : W * 4;
        fk->chunk.size = static_cast<std::uint32_t>(size);
        fk->sb.n_datas = 1;
        fk->sb.datas = &fk->data;
        fk->pw.buffer = &fk->sb;
        fakes.push_back(std::move(fk));
    }

    std::mutex mu;
    std::vector<pw_buffer*> free_list; // mutter's free buffers: delivered only once given back
    std::atomic<int> requeued{0};
    auto frames = std::make_unique<desktop::mutter::Frames>(
        [&](pw_buffer* b) {
            std::lock_guard<std::mutex> lk(mu);
            free_list.push_back(b);
            requeued++;
        },
        [] {});
    desktop::mutter::NegotiatedFormat nf;
    nf.format = sf;
    nf.width = W;
    nf.height = H;
    nf.dmabuf = true;
    nf.modifier = f.modifier;
    frames->set_format(nf);
    for (auto& fk : fakes) {
        frames->add_buffer(&fk->pw);
        free_list.push_back(&fk->pw);
    }

    CoreLoop loop;
    loop.start();
    FrameHub hub(61);
    ProducerConfig cfg;
    cfg.encoder = {EncoderKind::VaApi, "vaapi"};
    cfg.gop_seconds = 1;
    auto source = std::make_shared<DeclaredSource>("video/x-raw(memory:DMABuf),format=DMA_DRM", [&](GstElement* appsrc) { frames->attach(appsrc); });
    auto producer = std::make_unique<Producer>("va", SourceRef{source, "src"}, false, cfg, hub, loop.context());
    bool built = false;
    loop.call_sync([&] { built = producer->build() && producer->start_tier("active") && producer->start_tier("thumbnail"); });
    ASSERT_TRUE(built) << producer->error();
    auto active = std::make_shared<CountingSink>();
    auto thumb = std::make_shared<CountingSink>();
    hub.subscribe(HubKey{"va", "active"}, active);
    hub.subscribe(HubKey{"va", "thumbnail"}, thumb);
    for (int i = 0; i < 90 && (active->frames < 20 || thumb->frames < 2); i++) {
        pw_buffer* next = nullptr;
        {
            std::lock_guard<std::mutex> lk(mu);
            if (!free_list.empty()) {
                next = free_list.front();
                free_list.erase(free_list.begin());
            }
        }
        if (next && desktop::mutter::requeue_now(frames->on_buffer(next))) {
            std::lock_guard<std::mutex> lk(mu);
            free_list.push_back(next);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
        frames->drain_releases();
    }
    EXPECT_GE(active->frames, 20) << "the device-memory head did not encode the DMA-BUFs";
    EXPECT_GE(thumb->frames, 2) << "the thumbnail tier (scaled in vapostproc) did not encode";
    EXPECT_GT(requeued, 0) << "no buffer was ever given back to mutter";
    EXPECT_GE(frames->counts().fresh, 20u);
    loop.call_sync([&] { producer.reset(); });
    loop.stop();
    frames.reset();
    for (GstSample* s : samples) gst_sample_unref(s);
    gst_element_set_state(maker.get(), GST_STATE_NULL);
    gst_object_unref(out);
}
