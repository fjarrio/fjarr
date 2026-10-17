// Module E's capture memory (ADR-0041) against hand-made PipeWire buffers in place of a pw_stream:
// the offer for an import list (the intersection with mutter's modifiers, the order, the SHM tail),
// every reason a capture falls back to shared memory, the pixels-or-cursor-only rule without
// mapping, a DMA-BUF buffer held while GStreamer holds its frame and given back on release, and the
// repeat marking.
// spec: docs/23-agent-core-architecture.md#desktop-capture-memory
#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

#include <gst/allocators/gstdmabuf.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/video/video.h>
#include <spa/buffer/meta.h>
#include <spa/param/buffers.h>
#include <spa/param/format-utils.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/iter.h>

#include "core/glib/raii.hpp"
#include "desktop-mutter/capture_memory.hpp"
#include "media/frame_repeat.hpp"

using namespace fjarr;
using namespace fjarr::desktop::mutter;

namespace {

constexpr std::uint32_t XR24 = 0x34325258; // 'X','R','2','4'
constexpr std::uint32_t AR24 = 0x34325241;
constexpr std::uint32_t XB24 = 0x34324258;
constexpr std::uint64_t VA_MOD = 0x0200000000000901ULL;     // the one RGB modifier radeonsi's vapostproc lists (mini-PC)
constexpr std::uint64_t MUTTER_FIRST = 0x0200000000401a01ULL; // mutter's own first choice there

/// mutter's EnumFormat for one format: with its modifiers (a DMA-BUF offer) or without (shared memory).
const spa_pod* mutter_format(spa_pod_builder* b, spa_video_format f, std::vector<std::uint64_t> mods) {
    spa_pod_frame fr[2];
    spa_pod_builder_push_object(b, &fr[0], SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
    spa_pod_builder_add(b, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                        SPA_FORMAT_VIDEO_format, SPA_POD_Id(f), 0);
    if (!mods.empty()) {
        spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
        spa_pod_builder_push_choice(b, &fr[1], SPA_CHOICE_Enum, 0);
        spa_pod_builder_long(b, static_cast<std::int64_t>(mods[0]));
        for (auto m : mods) spa_pod_builder_long(b, static_cast<std::int64_t>(m));
        spa_pod_builder_pop(b, &fr[1]);
    }
    return static_cast<const spa_pod*>(spa_pod_builder_pop(b, &fr[0]));
}

/// The mini-PC's mutter (2026-10-17): BGRx and BGRA with six modifiers each, and shared memory.
ProducerFormats mini_pc_mutter() {
    static std::uint8_t buf[4096];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    const std::vector<std::uint64_t> mods{MUTTER_FIRST, 0x0200000000401901ULL, 0x0200000000401601ULL, 0x0200000000000a01ULL, VA_MOD, 0};
    std::vector<const spa_pod*> p{mutter_format(&b, SPA_VIDEO_FORMAT_BGRx, mods), mutter_format(&b, SPA_VIDEO_FORMAT_BGRA, mods),
                                  mutter_format(&b, SPA_VIDEO_FORMAT_BGRx, {}), mutter_format(&b, SPA_VIDEO_FORMAT_BGRA, {})};
    return parse_producer_formats(p);
}

MemoryRequest physical(std::vector<DmaBufFormat> imports) {
    MemoryRequest r;
    r.imports = std::move(imports);
    r.encoder = "vaapi";
    return r;
}

/// The values of one property of a pod, as longs or ids.
template <typename T> std::vector<T> values(const spa_pod* pod, std::uint32_t key, std::uint32_t* flags = nullptr) {
    const spa_pod_prop* p = spa_pod_find_prop(pod, nullptr, key);
    if (!p) return {};
    if (flags) *flags = p->flags;
    std::uint32_t n = 0, choice = 0;
    const spa_pod* v = spa_pod_get_values(&p->value, &n, &choice);
    const auto* body = static_cast<const T*>(SPA_POD_BODY_CONST(v));
    return std::vector<T>(body, body + n);
}

} // namespace

// ------------------------------------------------------------------ the offer

TEST(CaptureMemory, theOfferIsTheImportListIntersectedWithMuttersModifiersInTheEncodersOrder) {
    // The mini-PC: mutter's first choice (0x…401a01) is not one VA imports, so mutter's whole list
    // would have negotiated a buffer the encoder cannot take.
    const Offer o = plan_offer(physical({{AR24, VA_MOD}, {XR24, VA_MOD}, {XR24, 0x0100000000000002ULL}}), mini_pc_mutter());
    ASSERT_TRUE(o.dmabuf_offered()) << o.shm_reason;
    ASSERT_EQ(o.dmabuf.size(), 2u);
    EXPECT_EQ(o.dmabuf[0].first, SPA_VIDEO_FORMAT_BGRA); // the import list's order, not mutter's
    EXPECT_EQ(o.dmabuf[0].second, std::vector<std::uint64_t>{VA_MOD});
    EXPECT_EQ(o.dmabuf[1].first, SPA_VIDEO_FORMAT_BGRx);
    EXPECT_EQ(o.dmabuf[1].second, std::vector<std::uint64_t>{VA_MOD}); // Intel's modifier: mutter cannot make it here
    // The SHM tail: the same formats first, then the rest the reader takes.
    EXPECT_EQ(o.shm, (std::vector<spa_video_format>{SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_RGBA}));
    EXPECT_EQ(o.shm_reason, "");
}

TEST(CaptureMemory, theEnumFormatsOfferDmaBufWithMandatoryModifiersMutterFixatesThenSharedMemoryLast) {
    const Offer o = plan_offer(physical({{XR24, VA_MOD}, {XR24, 0}}), mini_pc_mutter());
    std::uint8_t buf[2048];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    const auto pods = build_enum_formats(o, &b);
    ASSERT_EQ(pods.size(), 2u);
    std::uint32_t flags = 0;
    EXPECT_EQ(values<std::uint32_t>(pods[0], SPA_FORMAT_VIDEO_format), std::vector<std::uint32_t>{SPA_VIDEO_FORMAT_BGRx});
    // The default first, then every alternative, flagged mandatory and left for mutter (which allocates) to fixate.
    EXPECT_EQ(values<std::int64_t>(pods[0], SPA_FORMAT_VIDEO_modifier, &flags), (std::vector<std::int64_t>{static_cast<std::int64_t>(VA_MOD), static_cast<std::int64_t>(VA_MOD), 0}));
    EXPECT_EQ(flags & (SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE), SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
    EXPECT_FALSE(spa_pod_find_prop(pods[1], nullptr, SPA_FORMAT_VIDEO_modifier)) << "the last offer is shared memory: no modifier";
    const auto shm = values<std::uint32_t>(pods[1], SPA_FORMAT_VIDEO_format);
    ASSERT_GE(shm.size(), 2u);
    EXPECT_EQ(shm[0], SPA_VIDEO_FORMAT_BGRx);
}

TEST(CaptureMemory, aVirtualMonitorsSizeIsAskedForInEveryOffer) {
    MemoryRequest r = physical({{XR24, VA_MOD}});
    r.virtual_monitor = true;
    const Offer o = plan_offer(r, mini_pc_mutter());
    std::uint8_t buf[1024];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    const auto pods = build_enum_formats(o, &b, 1280, 720);
    ASSERT_EQ(pods.size(), 1u);
    const auto size = values<spa_rectangle>(pods[0], SPA_FORMAT_VIDEO_size);
    ASSERT_EQ(size.size(), 1u);
    EXPECT_EQ(size[0].width, 1280u);
    EXPECT_EQ(size[0].height, 720u);
}

TEST(CaptureMemory, everyReasonForSharedMemoryIsNamed) {
    const ProducerFormats mutter = mini_pc_mutter();
    {
        MemoryRequest r = physical({{XR24, VA_MOD}});
        r.virtual_monitor = true;
        const Offer o = plan_offer(r, mutter);
        EXPECT_FALSE(o.dmabuf_offered());
        EXPECT_EQ(o.shm_reason, "a virtual monitor");
    }
    {
        MemoryRequest r = physical({{XR24, VA_MOD}});
        r.force_shm = true;
        EXPECT_EQ(plan_offer(r, mutter).shm_reason, "capture_memory = \"shm\"");
    }
    {
        MemoryRequest r = physical({});
        r.encoder = "software";
        EXPECT_EQ(plan_offer(r, mutter).shm_reason, "the software encoder imports no DMA-BUF");
    }
    {
        // No GPU mutter can allocate on (CI's headless mutter): its formats carry no modifier.
        std::uint8_t buf[1024];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
        const ProducerFormats shm_only = parse_producer_formats({mutter_format(&b, SPA_VIDEO_FORMAT_BGRx, {}), mutter_format(&b, SPA_VIDEO_FORMAT_BGRA, {})});
        EXPECT_EQ(plan_offer(physical({{XR24, VA_MOD}}), shm_only).shm_reason, "mutter offers no DMA-BUF (no GPU it can allocate on)");
    }
    {
        // Intel's modifier against the mini-PC's mutter: both lists printed.
        const Offer o = plan_offer(physical({{XB24, 0x0100000000000002ULL}, {AR24, 0x0100000000000009ULL}}), mutter);
        EXPECT_FALSE(o.dmabuf_offered());
        EXPECT_NE(o.shm_reason.find("no modifier in common"), std::string::npos) << o.shm_reason;
        EXPECT_NE(o.shm_reason.find("XB24:0x0100000000000002"), std::string::npos) << o.shm_reason;
        EXPECT_NE(o.shm_reason.find("XR24:0x0200000000401a01"), std::string::npos) << o.shm_reason;
        EXPECT_EQ(o.shm.front(), SPA_VIDEO_FORMAT_BGRx);
    }
    {
        // mutter's formats could not be read: the import list goes out as it is, PipeWire intersects.
        const Offer o = plan_offer(physical({{XR24, VA_MOD}}), ProducerFormats{});
        ASSERT_TRUE(o.dmabuf_offered());
        EXPECT_EQ(o.dmabuf[0].second, std::vector<std::uint64_t>{VA_MOD});
    }
}

TEST(CaptureMemory, aFormatWithAModifierIsDmaBufAndIsFixedOnlyOnceMutterPickedOne) {
    std::uint8_t buf[1024];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    auto format = [&](std::vector<std::uint64_t> mods, bool choice) {
        spa_pod_frame fr[2];
        spa_pod_builder_push_object(&b, &fr[0], SPA_TYPE_OBJECT_Format, SPA_PARAM_Format);
        const spa_rectangle size{1920, 1080};
        spa_pod_builder_add(&b, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                            SPA_FORMAT_VIDEO_format, SPA_POD_Id(SPA_VIDEO_FORMAT_BGRA), SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&size), 0);
        if (!mods.empty()) {
            spa_pod_builder_prop(&b, SPA_FORMAT_VIDEO_modifier, choice ? SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE : SPA_POD_PROP_FLAG_MANDATORY);
            if (choice) {
                spa_pod_builder_push_choice(&b, &fr[1], SPA_CHOICE_Enum, 0);
                for (auto m : mods) spa_pod_builder_long(&b, static_cast<std::int64_t>(m));
                spa_pod_builder_pop(&b, &fr[1]);
            } else {
                spa_pod_builder_long(&b, static_cast<std::int64_t>(mods[0]));
            }
        }
        return static_cast<const spa_pod*>(spa_pod_builder_pop(&b, &fr[0]));
    };
    const auto open = parse_format(format({VA_MOD, VA_MOD, 0}, true));
    ASSERT_TRUE(open);
    EXPECT_TRUE(open->dmabuf);
    EXPECT_FALSE(open->fixed);
    const auto fixed = parse_format(format({VA_MOD}, false));
    ASSERT_TRUE(fixed);
    EXPECT_TRUE(fixed->dmabuf && fixed->fixed);
    EXPECT_EQ(fixed->modifier, VA_MOD);
    EXPECT_EQ(fixed->width, 1920);
    EXPECT_EQ(fixed->drm_format(), "AR24:0x0200000000000901");
    const auto shm = parse_format(format({}, false));
    ASSERT_TRUE(shm);
    EXPECT_FALSE(shm->dmabuf);
    EXPECT_TRUE(shm->fixed);
}

TEST(CaptureMemory, dmaBufBuffersAreAtLeastEightAndNeverAskForExplicitSync) {
    std::uint8_t buf[1024];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    const auto dma = build_buffer_params(true, &b);
    ASSERT_FALSE(dma.empty());
    const auto count = values<std::int32_t>(dma[0], SPA_PARAM_BUFFERS_buffers);
    ASSERT_EQ(count.size(), 3u); // default, min, max
    EXPECT_GE(count[1], MIN_DMABUF_BUFFERS);
    EXPECT_EQ(values<std::int32_t>(dma[0], SPA_PARAM_BUFFERS_dataType)[0], 1 << SPA_DATA_DmaBuf);
    for (const spa_pod* p : dma) {
        if (!spa_pod_is_object_id(p, SPA_PARAM_Meta)) continue;
        EXPECT_NE(values<std::uint32_t>(p, SPA_PARAM_META_type)[0], static_cast<std::uint32_t>(SPA_META_SyncTimeline)) << "implicit sync (ADR-0041)";
    }
    const auto shm = build_buffer_params(false, &b);
    EXPECT_EQ(values<std::int32_t>(shm[0], SPA_PARAM_BUFFERS_dataType)[0], (1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr));
}

// ------------------------------------------------------------------ the frames

namespace {

/// One of mutter's buffers, as pw_stream hands it over: a DMA-BUF (a memfd stands in for its
/// descriptor; Frames never maps it) or shared memory in a MemPtr.
struct FakeBuffer {
    spa_chunk chunk{};
    spa_data data{};
    spa_buffer sb{};
    pw_buffer pw{};
    std::vector<std::uint8_t> shm;
    int fd = -1;
    FakeBuffer(bool dmabuf, int width, int height) {
        const std::uint32_t stride = static_cast<std::uint32_t>(width) * 4;
        data.maxsize = stride * static_cast<std::uint32_t>(height);
        if (dmabuf) {
            fd = ::memfd_create("fake-dmabuf", MFD_CLOEXEC);
            EXPECT_EQ(::ftruncate(fd, data.maxsize), 0);
            data.type = SPA_DATA_DmaBuf;
            data.fd = fd;
            data.data = nullptr; // never mapped (a tiled one cannot be: EPERM)
        } else {
            shm.assign(data.maxsize, 0x7f);
            data.type = SPA_DATA_MemPtr;
            data.fd = -1;
            data.data = shm.data();
        }
        data.chunk = &chunk;
        chunk.stride = static_cast<std::int32_t>(stride);
        sb.n_datas = 1;
        sb.datas = &data;
        pw.buffer = &sb;
        pixels();
    }
    ~FakeBuffer() {
        if (fd >= 0) ::close(fd);
    }
    void pixels() {
        chunk.size = data.maxsize;
        chunk.flags = 0;
    }
    void cursor_only() {
        chunk.size = 0;
        chunk.flags = SPA_CHUNK_FLAG_CORRUPTED;
    }
};

NegotiatedFormat dmabuf_format(int w, int h) {
    NegotiatedFormat f;
    f.format = SPA_VIDEO_FORMAT_BGRx;
    f.width = w;
    f.height = h;
    f.dmabuf = true;
    f.modifier = VA_MOD;
    return f;
}

/// appsrc ! appsink, PLAYING: what the reader feeds and what the pipeline holds.
struct Sink {
    glib::GstElementPtr pipeline;
    GstElement* appsrc = nullptr;
    GstElement* appsink = nullptr;
    Sink() {
        GError* err = nullptr;
        pipeline = glib::sink_element(gst_parse_launch("appsrc name=src is-live=true format=time do-timestamp=true ! appsink name=sink sync=false max-buffers=64 enable-last-sample=false", &err));
        EXPECT_EQ(err, nullptr);
        appsrc = gst_bin_get_by_name(GST_BIN(pipeline.get()), "src");
        appsink = gst_bin_get_by_name(GST_BIN(pipeline.get()), "sink");
        gst_element_set_state(pipeline.get(), GST_STATE_PLAYING);
    }
    ~Sink() {
        gst_element_set_state(pipeline.get(), GST_STATE_NULL);
        gst_object_unref(appsrc);
        gst_object_unref(appsink);
    }
    /// The next buffer the pipeline got (a ref), or null within a second.
    GstBuffer* pull() {
        GstSample* s = gst_app_sink_try_pull_sample(GST_APP_SINK(appsink), GST_SECOND);
        if (!s) return nullptr;
        GstBuffer* b = gst_buffer_ref(gst_sample_get_buffer(s));
        gst_sample_unref(s);
        return b;
    }
};

/// Frames with its PipeWire side recorded: what went back to mutter, and whether a release woke it.
struct Harness {
    std::vector<pw_buffer*> requeued;
    std::atomic<int> wakes{0};
    std::unique_ptr<Frames> frames = std::make_unique<Frames>([this](pw_buffer* b) { requeued.push_back(b); }, [this] { wakes++; });
    /// What the PipeWire thread does after a wake.
    void settle() {
        for (int i = 0; i < 100 && wakes == 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        frames->drain_releases();
        wakes = 0;
    }
};

} // namespace

TEST(CaptureMemory, aBufferHasPixelsByItsChunkAloneAndIsNeverMapped) {
    FakeBuffer dma(true, 64, 32);
    EXPECT_TRUE(has_pixels(&dma.sb)) << "a DMA-BUF with a full chunk has pixels, though nothing mapped it";
    dma.cursor_only();
    EXPECT_FALSE(has_pixels(&dma.sb)) << "an empty chunk flagged corrupted is the cursor alone";
    dma.chunk.size = 64;
    dma.chunk.flags = SPA_CHUNK_FLAG_CORRUPTED;
    EXPECT_FALSE(has_pixels(&dma.sb)) << "a corrupted chunk has no pixels, whatever its size";
    dma.chunk.flags = 0;
    dma.chunk.size = 0;
    EXPECT_FALSE(has_pixels(&dma.sb));

    // The same rule inside Frames: a cursor-only DMA-BUF goes straight back, counted as such.
    Harness h;
    h.frames->set_format(dmabuf_format(64, 32));
    h.frames->add_buffer(&dma.pw);
    dma.cursor_only();
    EXPECT_EQ(h.frames->on_buffer(&dma.pw), Delivery::CursorOnly);
    EXPECT_EQ(h.frames->counts().cursor_only, 1u);
    EXPECT_EQ(h.frames->counts().fresh, 0u);
    EXPECT_FALSE(h.frames->has_frame());
    h.frames->remove_buffer(&dma.pw);
}

TEST(CaptureMemory, aDmaBufFrameIsWrappedOnceAsDmaDrmCapsWithItsPlaneLayout) {
    Harness h;
    Sink sink;
    FakeBuffer a(true, 64, 32);
    a.chunk.offset = 0;
    h.frames->set_format(dmabuf_format(64, 32));
    h.frames->add_buffer(&a.pw);
    h.frames->attach(sink.appsrc);
    glib::GstCapsPtr caps(h.frames->caps());
    ASSERT_TRUE(caps);
    const std::string c = glib::caps_to_string(caps.get());
    EXPECT_NE(c.find("video/x-raw(memory:DMABuf)"), std::string::npos) << c;
    EXPECT_NE(c.find("DMA_DRM"), std::string::npos) << c;
    EXPECT_NE(c.find("XR24:0x0200000000000901"), std::string::npos) << c;
    EXPECT_EQ(h.frames->on_buffer(&a.pw), Delivery::Held) << "a DMA-BUF frame is held, not queued back at once";
    GstBuffer* got = sink.pull();
    ASSERT_TRUE(got);
    ASSERT_EQ(gst_buffer_n_memory(got), 1u);
    GstMemory* mem = gst_buffer_peek_memory(got, 0);
    EXPECT_TRUE(gst_is_dmabuf_memory(mem));
    EXPECT_NE(gst_dmabuf_memory_get_fd(mem), a.fd) << "wrapped on a duplicate of mutter's descriptor";
    const GstVideoMeta* vm = gst_buffer_get_video_meta(got);
    ASSERT_TRUE(vm);
    EXPECT_EQ(vm->stride[0], 64 * 4);
    EXPECT_EQ(vm->offset[0], 0u);
    EXPECT_FALSE(media::is_repeat(got)) << "a new frame is not a repeat";
    // The keepalive re-sends the same memory, as a new GstBuffer marked as a repeat.
    h.frames->keepalive(0);
    GstBuffer* again = sink.pull();
    ASSERT_TRUE(again);
    EXPECT_NE(again, got);
    EXPECT_EQ(gst_buffer_peek_memory(again, 0), mem) << "wrapped once, reused";
    EXPECT_TRUE(media::is_repeat(again));
    gst_buffer_unref(again);
    gst_buffer_unref(got);
    h.frames.reset();
}

TEST(CaptureMemory, aDmaBufBufferGoesBackToMutterOnlyWhenTheLastGstBufferOverItIsReleased) {
    Harness h;
    Sink sink;
    FakeBuffer a(true, 32, 16), b(true, 32, 16), c(true, 32, 16);
    h.frames->set_format(dmabuf_format(32, 16));
    for (FakeBuffer* f : {&a, &b, &c}) h.frames->add_buffer(&f->pw);
    h.frames->attach(sink.appsrc);

    EXPECT_EQ(h.frames->on_buffer(&a.pw), Delivery::Held);
    GstBuffer* fa = sink.pull();
    ASSERT_TRUE(fa);
    GstBuffer* fa_copy = gst_buffer_copy(fa); // an element's shallow copy (appsrc stamping, videorate): it holds A too
    h.frames->keepalive(0);                   // a repeat of A, also in flight
    GstBuffer* fa_again = sink.pull();
    ASSERT_TRUE(fa_again);

    // B arrives: A is no longer the newest, but GStreamer still holds three buffers over it.
    EXPECT_EQ(h.frames->on_buffer(&b.pw), Delivery::Held);
    GstBuffer* fb = sink.pull();
    ASSERT_TRUE(fb);
    h.frames->drain_releases();
    EXPECT_TRUE(h.requeued.empty()) << "A went back to mutter while the pipeline still held it";
    EXPECT_EQ(h.frames->buffers_out(), 2u);

    gst_buffer_unref(fa);
    gst_buffer_unref(fa_again);
    h.frames->drain_releases();
    EXPECT_TRUE(h.requeued.empty()) << "a copy still holds A";
    gst_buffer_unref(fa_copy); // the last one: released, posted, given back on the PipeWire thread
    h.settle();
    ASSERT_EQ(h.requeued.size(), 1u);
    EXPECT_EQ(h.requeued[0], &a.pw);

    // B is the newest: held for the keepalive even with nothing else holding it.
    gst_buffer_unref(fb);
    h.frames->drain_releases();
    EXPECT_EQ(h.requeued.size(), 1u) << "the newest frame is held for the keepalive";
    EXPECT_EQ(h.frames->buffers_out(), 1u);

    // A comes round again: a new delivery of the same buffer, held anew; B goes back.
    EXPECT_EQ(h.frames->on_buffer(&a.pw), Delivery::Held);
    GstBuffer* fa2 = sink.pull();
    ASSERT_TRUE(fa2);
    h.settle();
    ASSERT_EQ(h.requeued.size(), 2u);
    EXPECT_EQ(h.requeued[1], &b.pw);
    gst_buffer_unref(fa2);
    EXPECT_EQ(h.frames->counts().fresh, 3u);
    EXPECT_EQ(h.frames->counts().repeated, 1u);
    h.frames.reset(); // the held one is dropped with the reader, nothing is given back after it
    EXPECT_EQ(h.requeued.size(), 2u);
}

TEST(CaptureMemory, aRemovedBufferIsNeverGivenBackAndAStaleReleaseIsIgnored) {
    Harness h;
    Sink sink;
    FakeBuffer a(true, 16, 16), b(true, 16, 16);
    h.frames->set_format(dmabuf_format(16, 16));
    h.frames->add_buffer(&a.pw);
    h.frames->add_buffer(&b.pw);
    h.frames->attach(sink.appsrc);
    h.frames->on_buffer(&a.pw);
    GstBuffer* fa = sink.pull();
    ASSERT_TRUE(fa);
    h.frames->on_buffer(&b.pw);
    GstBuffer* fb = sink.pull();
    // A renegotiation removes every buffer while the pipeline still holds A's frame.
    h.frames->remove_buffer(&a.pw);
    h.frames->remove_buffer(&b.pw);
    EXPECT_FALSE(h.frames->has_frame()) << "the newest frame went with its buffer";
    gst_buffer_unref(fa);
    gst_buffer_unref(fb);
    h.settle();
    EXPECT_TRUE(h.requeued.empty()) << "a buffer PipeWire removed was queued back";
    h.frames.reset();
}

TEST(CaptureMemory, aNewPipelineGetsTheNewestFrameAtOnceMarkedAsARepeat) {
    Harness h;
    FakeBuffer a(true, 16, 16);
    h.frames->set_format(dmabuf_format(16, 16));
    h.frames->add_buffer(&a.pw);
    EXPECT_EQ(h.frames->on_buffer(&a.pw), Delivery::Held); // no pipeline yet: held all the same
    Sink sink;
    h.frames->attach(sink.appsrc);
    GstBuffer* first = sink.pull();
    ASSERT_TRUE(first);
    EXPECT_TRUE(media::is_repeat(first));
    EXPECT_EQ(h.frames->counts().repeated, 1u);
    EXPECT_EQ(h.frames->counts().fresh, 1u);
    gst_buffer_unref(first);
    h.frames.reset();
}

TEST(CaptureMemory, sharedMemoryIsCopiedAndQueuedBackAtOnceAndItsRepeatsAreMarked) {
    Harness h;
    Sink sink;
    FakeBuffer s(false, 8, 4);
    NegotiatedFormat f;
    f.format = SPA_VIDEO_FORMAT_BGRx;
    f.width = 8;
    f.height = 4;
    h.frames->set_format(f);
    h.frames->add_buffer(&s.pw);
    h.frames->attach(sink.appsrc);
    EXPECT_EQ(h.frames->on_buffer(&s.pw), Delivery::Copied) << "shared memory is copied and goes straight back";
    GstBuffer* got = sink.pull();
    ASSERT_TRUE(got);
    EXPECT_EQ(gst_buffer_get_size(got), 8u * 4 * 4);
    EXPECT_FALSE(media::is_repeat(got));
    h.frames->keepalive(0);
    GstBuffer* again = sink.pull();
    ASSERT_TRUE(again);
    EXPECT_TRUE(media::is_repeat(again));
    EXPECT_EQ(h.frames->counts().repeated, 1u);
    gst_buffer_unref(got);
    gst_buffer_unref(again);
    h.frames.reset();
}

TEST(CaptureMemory, sharedMemoryPipeWireDidNotMapIsMappedByTheReader) {
    // A stream that offered DMA-BUF links without MAP_BUFFERS; mutter may still pick a MemFd.
    Harness h;
    FakeBuffer s(false, 8, 4);
    const int fd = ::memfd_create("fake-shm", MFD_CLOEXEC);
    ASSERT_EQ(::ftruncate(fd, s.data.maxsize), 0);
    std::vector<std::uint8_t> px(s.data.maxsize, 0x42);
    ASSERT_EQ(::write(fd, px.data(), px.size()), static_cast<ssize_t>(px.size()));
    s.data.type = SPA_DATA_MemFd;
    s.data.fd = fd;
    s.data.data = nullptr;
    NegotiatedFormat f;
    f.format = SPA_VIDEO_FORMAT_BGRx;
    f.width = 8;
    f.height = 4;
    h.frames->set_format(f);
    h.frames->add_buffer(&s.pw);
    Sink sink;
    h.frames->attach(sink.appsrc);
    EXPECT_EQ(h.frames->on_buffer(&s.pw), Delivery::Copied);
    GstBuffer* got = sink.pull();
    ASSERT_TRUE(got);
    GstMapInfo map;
    ASSERT_TRUE(gst_buffer_map(got, &map, GST_MAP_READ));
    EXPECT_EQ(map.data[0], 0x42);
    gst_buffer_unmap(got, &map);
    gst_buffer_unref(got);
    h.frames->remove_buffer(&s.pw);
    h.frames.reset();
    ::close(fd);
}

// ------------------------------------------------- settling, changing and falling back

TEST(CaptureMemory, theReasonForSharedMemoryIsTheOffersOrThatMutterChoseItOverTheDmaBufOffered) {
    const ProducerFormats mutter = mini_pc_mutter();
    MemoryRequest virt = physical({{XR24, VA_MOD}});
    virt.virtual_monitor = true;
    EXPECT_EQ(settled_shm_reason(plan_offer(virt, mutter), mutter), "a virtual monitor") << "not offered: the offer's own reason";
    EXPECT_EQ(settled_shm_reason(plan_offer(physical({{XR24, VA_MOD}}), mutter), mutter), "mutter chose shared memory over the DMA-BUF offered");
    // mutter's formats could not be read: the whole import list went out and mutter took SHM.
    const ProducerFormats unread;
    EXPECT_EQ(settled_shm_reason(plan_offer(physical({{XR24, VA_MOD}}), unread), unread),
              "mutter chose shared memory over the DMA-BUF offered (its formats could not be read first)");
}

TEST(CaptureMemory, aCaptureKeepsItsMemoryAndAChangeRestartsItAtMostThreeTimesNeverAVirtualOne) {
    EXPECT_EQ(settle_kind(std::nullopt, true), MemorySettle::First);
    EXPECT_EQ(settle_kind(true, true), MemorySettle::Same);
    EXPECT_EQ(settle_kind(false, false), MemorySettle::Same);
    EXPECT_EQ(settle_kind(true, false), MemorySettle::Changed);
    EXPECT_EQ(settle_kind(false, true), MemorySettle::Changed);
    EXPECT_TRUE(restart_for_memory_change(false, 0));
    EXPECT_TRUE(restart_for_memory_change(false, MAX_MEMORY_RESTARTS - 1));
    EXPECT_FALSE(restart_for_memory_change(false, MAX_MEMORY_RESTARTS)) << "past the limit the capture adopts the new memory";
    EXPECT_FALSE(restart_for_memory_change(true, 0)) << "a virtual monitor's restart would make another monitor";
}

TEST(CaptureMemory, aCaptureWhoseDmaBufCouldNotBeWrappedIsOfferedSharedMemoryForThatReason) {
    MemoryRequest r = physical({{XR24, VA_MOD}});
    r.fallback = "a DMA-BUF of 2 planes, which the reader does not take yet";
    const Offer o = plan_offer(r, mini_pc_mutter());
    EXPECT_FALSE(o.dmabuf_offered());
    EXPECT_EQ(o.shm_reason, r.fallback);
    EXPECT_EQ(settled_shm_reason(o, mini_pc_mutter()), r.fallback);
}

TEST(CaptureMemory, aParamThatDoesNotFitItsBuilderIsLeftOutNeverHandedOnAsNull) {
    const Offer o = plan_offer(physical({{XR24, VA_MOD}, {AR24, VA_MOD}}), mini_pc_mutter());
    std::uint8_t tiny[96];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(tiny, sizeof(tiny));
    for (const spa_pod* p : build_enum_formats(o, &b)) EXPECT_NE(p, nullptr);
    spa_pod_builder b2 = SPA_POD_BUILDER_INIT(tiny, sizeof(tiny));
    const auto params = build_buffer_params(true, &b2);
    EXPECT_LT(params.size(), 2u);
    for (const spa_pod* p : params) EXPECT_NE(p, nullptr);
}

TEST(CaptureMemory, aMultiPlaneDmaBufIsNotWrappedTheCaptureFallsBackAndItsFramesAreDropped) {
    // An auxiliary plane (DCC, CCS) is not verified on any hardware: never pushed with fewer or
    // more planes than VA counts, and the reader is told once, to restart on shared memory.
    Harness h;
    Sink sink;
    FakeBuffer a(true, 32, 16);
    const int aux = ::memfd_create("fake-aux", MFD_CLOEXEC);
    ASSERT_EQ(::ftruncate(aux, 4096), 0);
    spa_chunk aux_chunk{};
    aux_chunk.size = 4096;
    spa_data datas[2] = {a.data, {}};
    datas[1].type = SPA_DATA_DmaBuf;
    datas[1].fd = aux;
    datas[1].maxsize = 4096;
    datas[1].chunk = &aux_chunk;
    a.sb.n_datas = 2;
    a.sb.datas = datas;
    h.frames->set_format(dmabuf_format(32, 16));
    h.frames->add_buffer(&a.pw);
    h.frames->attach(sink.appsrc);
    auto why = h.frames->take_unwrappable();
    ASSERT_TRUE(why);
    EXPECT_NE(why->find("2 planes"), std::string::npos) << *why;
    EXPECT_FALSE(h.frames->take_unwrappable()) << "said once";
    const Delivery d = h.frames->on_buffer(&a.pw);
    EXPECT_EQ(d, Delivery::Dropped);
    EXPECT_TRUE(requeue_now(d));
    EXPECT_FALSE(made_frame(d)) << "no frame: the first-frame watch must still see none";
    EXPECT_EQ(sink.pull(), nullptr) << "nothing of it reached the pipeline";
    h.frames->remove_buffer(&a.pw);
    h.frames.reset();
    ::close(aux);
}

TEST(CaptureMemory, aNewFormatDropsTheNewestFrameMadeForTheOldOneAndGivesItsBufferBack) {
    Harness h;
    Sink sink;
    FakeBuffer a(true, 32, 16);
    h.frames->set_format(dmabuf_format(32, 16));
    h.frames->add_buffer(&a.pw);
    h.frames->attach(sink.appsrc);
    EXPECT_EQ(h.frames->on_buffer(&a.pw), Delivery::Held);
    GstBuffer* got = sink.pull();
    ASSERT_TRUE(got);
    gst_buffer_unref(got);
    h.frames->drain_releases();
    EXPECT_TRUE(h.requeued.empty()) << "the newest is held for the keepalive";
    h.frames->set_format(dmabuf_format(64, 32)); // a size change before PipeWire removed the old buffers
    EXPECT_FALSE(h.frames->has_frame()) << "the 32x16 buffer would have been re-sent as 64x32";
    h.frames->keepalive(0);
    EXPECT_EQ(sink.pull(), nullptr) << "nothing to repeat until a frame of the new size arrives";
    gst_element_set_state(sink.pipeline.get(), GST_STATE_NULL); // appsink keeps the newest buffer it got until the next
    h.settle();
    ASSERT_EQ(h.requeued.size(), 1u) << "no longer held: back to mutter";
    h.frames->set_format(dmabuf_format(64, 32)); // the same again: nothing changes
    h.frames->remove_buffer(&a.pw);
    h.frames.reset();
}

TEST(CaptureMemory, aSuspendedCaptureFeedsNothingUntilResumedThenTheNewestFrameAsARepeat) {
    // A renegotiation changed the memory under a pipeline built for the old one: nothing goes in
    // until the core loop decided, and adopting the new memory feeds the same appsrc again.
    Harness h;
    Sink sink;
    FakeBuffer a(true, 16, 16);
    h.frames->set_format(dmabuf_format(16, 16));
    h.frames->add_buffer(&a.pw);
    h.frames->attach(sink.appsrc);
    h.frames->suspend();
    EXPECT_EQ(h.frames->on_buffer(&a.pw), Delivery::Held) << "still taken and held";
    h.frames->keepalive(0);
    EXPECT_EQ(sink.pull(), nullptr) << "a suspended capture pushed a frame";
    EXPECT_EQ(h.frames->counts().fresh, 1u);
    EXPECT_EQ(h.frames->counts().repeated, 0u);
    h.frames->resume();
    GstBuffer* got = sink.pull();
    ASSERT_TRUE(got);
    EXPECT_TRUE(media::is_repeat(got));
    EXPECT_EQ(h.frames->counts().repeated, 1u);
    gst_buffer_unref(got);
    h.frames.reset();
}

TEST(CaptureMemory, sharedMemoryWhoseChunkDoesNotFitIsDroppedAndMakesNoFrame) {
    Harness h;
    FakeBuffer s(false, 8, 4);
    s.chunk.offset = s.data.maxsize - 4; // a size change in flight
    NegotiatedFormat f;
    f.format = SPA_VIDEO_FORMAT_BGRx;
    f.width = 8;
    f.height = 4;
    h.frames->set_format(f);
    h.frames->add_buffer(&s.pw);
    const Delivery d = h.frames->on_buffer(&s.pw);
    EXPECT_EQ(d, Delivery::Dropped);
    EXPECT_FALSE(made_frame(d));
    EXPECT_EQ(h.frames->counts().fresh, 0u);
    h.frames->remove_buffer(&s.pw);
}
