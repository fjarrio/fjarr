// spec: docs/23-agent-core-architecture.md#desktop-helper-protocol (The stream reader and the cursor)
#include "stream_reader.hpp"

#include <chrono>
#include <cstring>
#include <mutex>

#include <gst/app/gstappsrc.h>
#include <pipewire/pipewire.h>
#include <spa/buffer/meta.h>
#include <spa/param/buffers.h>
#include <spa/param/video/format-utils.h>

namespace fjarr::desktop::mutter {

namespace {
constexpr int MAX_SIDE = 384; // docs/08: cursor images at most 384×384
std::size_t meta_size(int side) { return sizeof(spa_meta_cursor) + sizeof(spa_meta_bitmap) + static_cast<std::size_t>(side) * side * 4; }
using Clock = std::chrono::steady_clock;
} // namespace

struct StreamReader::Impl {
    // Its own PipeWire thread: a buffer goes back to mutter only once it is returned, and nothing on
    // that path may wait for the core loop (a busy loop would stall the video).
    pw_thread_loop* thread = nullptr;
    pw_context* context = nullptr;
    pw_core* core = nullptr;
    pw_stream* stream = nullptr;
    spa_hook stream_listener{};
    spa_source* keepalive = nullptr;
    int keepalive_ms = 100;
    GMainContext* ctx = nullptr;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true); // posts that outlive the reader are dropped
    Shape shape;
    Position position;
    Log log;

    // Frames: guarded by `mu` (the PipeWire thread pushes, create_bin runs on the core loop).
    std::mutex mu;
    GstElement* appsrc = nullptr; // the current pipeline's source, ref held
    GstBuffer* last = nullptr;    // the newest frame, for the keepalive and a new pipeline
    GstCaps* caps = nullptr;
    Clock::time_point last_push{};

    // Touched only on the PipeWire thread:
    int width = 0, height = 0;
    spa_video_format format = SPA_VIDEO_FORMAT_UNKNOWN;
    std::string last_shape;
    int last_x = -1, last_y = -1;

    ~Impl() {
        *alive = false;
        if (thread) pw_thread_loop_stop(thread);
        if (stream) pw_stream_destroy(stream);
        if (core) pw_core_disconnect(core);
        if (context) pw_context_destroy(context);
        if (thread) pw_thread_loop_destroy(thread);
        std::lock_guard<std::mutex> lk(mu);
        if (appsrc) gst_app_src_end_of_stream(GST_APP_SRC(appsrc)), gst_object_unref(appsrc);
        if (last) gst_buffer_unref(last);
        if (caps) gst_caps_unref(caps);
    }

    /// Run `fn` on the core loop, unless the reader is gone by then.
    void post(std::function<void()> fn) {
        struct Job {
            std::shared_ptr<bool> alive;
            std::function<void()> fn;
        };
        g_main_context_invoke_full(ctx, G_PRIORITY_DEFAULT, [](gpointer d) -> gboolean {
            auto* j = static_cast<Job*>(d);
            if (*j->alive) j->fn();
            return G_SOURCE_REMOVE;
        }, new Job{alive, std::move(fn)}, [](gpointer d) { delete static_cast<Job*>(d); });
    }

    /// Push `buf` (a ref is taken) into the current appsrc, if there is one. Holds `mu`.
    void push_locked(GstBuffer* buf) {
        last_push = Clock::now();
        if (!appsrc) return;
        // A shallow copy: appsrc's do-timestamp stamps it, and the same frame may go out again.
        gst_app_src_push_buffer(GST_APP_SRC(appsrc), gst_buffer_copy(buf));
    }

    void on_param_changed(std::uint32_t id, const spa_pod* param) {
        if (!param || id != SPA_PARAM_Format) return;
        spa_video_info_raw info{};
        if (spa_format_video_raw_parse(param, &info) < 0) return;
        width = static_cast<int>(info.size.width);
        height = static_cast<int>(info.size.height);
        format = info.format;
        const char* fmt = format == SPA_VIDEO_FORMAT_BGRA ? "BGRA" : format == SPA_VIDEO_FORMAT_RGBx ? "RGBx" : format == SPA_VIDEO_FORMAT_RGBA ? "RGBA" : "BGRx";
        {
            std::lock_guard<std::mutex> lk(mu);
            if (caps) gst_caps_unref(caps);
            caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, fmt, "width", G_TYPE_INT, width, "height", G_TYPE_INT, height, "framerate",
                                       GST_TYPE_FRACTION, 0, 1, nullptr);
            if (appsrc) gst_app_src_set_caps(GST_APP_SRC(appsrc), caps);
        }
        // Shared memory we can map (not DMA-BUF: it failed on the spike machine, ADR-0006), and the cursor.
        std::uint8_t buf[512];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
        const spa_pod* params[2];
        params[0] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
            &b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers, SPA_PARAM_BUFFERS_dataType,
            SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr))));
        params[1] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
            &b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Cursor), SPA_PARAM_META_size,
            SPA_POD_CHOICE_RANGE_Int(static_cast<int>(meta_size(64)), static_cast<int>(meta_size(1)), static_cast<int>(meta_size(MAX_SIDE)))));
        pw_stream_update_params(stream, params, 2);
    }

    void on_process() {
        pw_buffer* b = pw_stream_dequeue_buffer(stream);
        if (!b) return;
        spa_buffer* sb = b->buffer;
        // The frame: a chunk with pixels (a cursor-only buffer has none, or is flagged corrupted).
        if (sb->n_datas > 0 && sb->datas[0].data && sb->datas[0].chunk && sb->datas[0].chunk->size > 0 &&
            !(sb->datas[0].chunk->flags & SPA_CHUNK_FLAG_CORRUPTED) && width > 0 && height > 0) {
            const spa_data& d = sb->datas[0];
            const int stride = d.chunk->stride > 0 ? d.chunk->stride : width * 4;
            const std::size_t row = static_cast<std::size_t>(width) * 4;
            GstBuffer* frame = gst_buffer_new_allocate(nullptr, row * height, nullptr);
            GstMapInfo map;
            if (gst_buffer_map(frame, &map, GST_MAP_WRITE)) {
                const auto* src = static_cast<const std::uint8_t*>(d.data) + d.chunk->offset;
                for (int y = 0; y < height; y++) std::memcpy(map.data + row * y, src + static_cast<std::size_t>(stride) * y, row);
                gst_buffer_unmap(frame, &map);
                std::lock_guard<std::mutex> lk(mu);
                if (last) gst_buffer_unref(last);
                last = frame;
                push_locked(last);
            } else {
                gst_buffer_unref(frame);
            }
        }
        auto* mc = static_cast<spa_meta_cursor*>(spa_buffer_find_meta_data(sb, SPA_META_Cursor, sizeof(spa_meta_cursor)));
        if (mc && spa_meta_cursor_is_valid(mc)) read_cursor(mc);
        pw_stream_queue_buffer(stream, b); // straight back: the producer must never wait on us
    }

    void on_keepalive() {
        std::lock_guard<std::mutex> lk(mu);
        if (last && Clock::now() - last_push >= std::chrono::milliseconds(keepalive_ms)) push_locked(last);
    }

    void read_cursor(const spa_meta_cursor* mc) {
        if (mc->bitmap_offset >= sizeof(spa_meta_cursor)) {
            const auto* bm = SPA_PTROFF(mc, mc->bitmap_offset, const spa_meta_bitmap);
            CursorImage img;
            if (bm->size.width == 0 || bm->size.height == 0) {
                img.visible = false; // mutter's empty shape: no cursor
            } else if (bm->size.width <= MAX_SIDE && bm->size.height <= MAX_SIDE &&
                       (bm->format == SPA_VIDEO_FORMAT_RGBA || bm->format == SPA_VIDEO_FORMAT_BGRA)) {
                img.width = static_cast<int>(bm->size.width);
                img.height = static_cast<int>(bm->size.height);
                img.hot_x = mc->hotspot.x;
                img.hot_y = mc->hotspot.y;
                img.rgba = to_straight_rgba(SPA_PTROFF(bm, bm->offset, const std::uint8_t), img.width, img.height, bm->stride,
                                            bm->format == SPA_VIDEO_FORMAT_BGRA);
            } else {
                const std::string why = "a cursor shape the reader cannot use (" + std::to_string(bm->size.width) + "x" + std::to_string(bm->size.height) +
                                        ", format " + std::to_string(bm->format) + ")";
                post([this, why] { log(2, why); });
            }
            if (!img.visible || img.width > 0) {
                img.shape_id = shape_id(img);
                if (img.shape_id != last_shape) {
                    last_shape = img.shape_id;
                    post([this, img = std::move(img)] {
                        if (shape) shape(img);
                    });
                }
            }
        }
        const int x = mc->position.x, y = mc->position.y;
        if (width > 0 && height > 0 && x >= 0 && y >= 0 && x < width && y < height && (x != last_x || y != last_y)) {
            last_x = x;
            last_y = y;
            const double nx = static_cast<double>(x) / width, ny = static_cast<double>(y) / height;
            post([this, nx, ny] {
                if (position) position(nx, ny);
            });
        }
    }
};

namespace {
const pw_stream_events EVENTS = [] {
    pw_stream_events e{};
    e.version = PW_VERSION_STREAM_EVENTS;
    e.param_changed = [](void* d, std::uint32_t id, const spa_pod* param) { static_cast<StreamReader::Impl*>(d)->on_param_changed(id, param); };
    e.process = [](void* d) { static_cast<StreamReader::Impl*>(d)->on_process(); };
    return e;
}();
} // namespace

std::unique_ptr<StreamReader> StreamReader::start(GMainContext* ctx, int fd, std::uint32_t node, int keepalive_ms, Shape shape, Position position, Log log) {
    static const bool initialized = [] {
        pw_init(nullptr, nullptr);
        return true;
    }();
    (void)initialized;
    auto impl = std::make_unique<Impl>();
    impl->ctx = ctx;
    impl->keepalive_ms = keepalive_ms > 0 ? keepalive_ms : 100;
    impl->shape = std::move(shape);
    impl->position = std::move(position);
    impl->log = std::move(log);
    impl->thread = pw_thread_loop_new("fjarr-capture", nullptr);
    impl->context = impl->thread ? pw_context_new(pw_thread_loop_get_loop(impl->thread), nullptr, 0) : nullptr;
    if (!impl->context || pw_thread_loop_start(impl->thread) < 0) {
        ::close(fd);
        impl->log(2, "the stream reader could not start a PipeWire loop");
        return nullptr;
    }
    pw_thread_loop_lock(impl->thread);
    impl->core = pw_context_connect_fd(impl->context, fd, nullptr, 0); // takes the fd
    bool ok = impl->core != nullptr;
    if (ok) {
        impl->stream = pw_stream_new(impl->core, "fjarr-capture",
                                     pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Screen", nullptr));
        pw_stream_add_listener(impl->stream, &impl->stream_listener, &EVENTS, impl.get());
        std::uint8_t buf[512];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
        const spa_pod* params[1];
        params[0] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
            &b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), SPA_FORMAT_mediaSubtype,
            SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), SPA_FORMAT_VIDEO_format,
            SPA_POD_CHOICE_ENUM_Id(5, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_RGBA)));
        ok = pw_stream_connect(impl->stream, PW_DIRECTION_INPUT, node, static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
                               params, 1) >= 0;
        if (ok) {
            // The keepalive: mutter is silent on a still screen, and the encoder must keep producing.
            impl->keepalive = pw_loop_add_timer(pw_thread_loop_get_loop(impl->thread), [](void* d, std::uint64_t) { static_cast<Impl*>(d)->on_keepalive(); },
                                                impl.get());
            timespec value{0, static_cast<long>(impl->keepalive_ms) * 1'000'000L}, interval = value;
            pw_loop_update_timer(pw_thread_loop_get_loop(impl->thread), impl->keepalive, &value, &interval, false);
        }
    }
    pw_thread_loop_unlock(impl->thread);
    if (!ok) {
        impl->log(2, "the stream reader could not link to node " + std::to_string(node));
        return nullptr;
    }
    return std::unique_ptr<StreamReader>(new StreamReader(std::move(impl)));
}

GstBin* StreamReader::create_bin() {
    GstElement* src = gst_element_factory_make("appsrc", nullptr);
    if (!src) return nullptr;
    g_object_set(src, "is-live", TRUE, "format", GST_FORMAT_TIME, "do-timestamp", TRUE, "max-buffers", static_cast<guint64>(4), "leaky-type", 2 /* downstream: drop old */, nullptr);
    GstElement* bin = gst_bin_new(nullptr);
    gst_bin_add(GST_BIN(bin), src);
    GstPad* pad = gst_element_get_static_pad(src, "src");
    gst_element_add_pad(bin, gst_ghost_pad_new("src", pad));
    gst_object_unref(pad);
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (impl_->caps) gst_app_src_set_caps(GST_APP_SRC(src), impl_->caps);
    if (impl_->appsrc) gst_object_unref(impl_->appsrc);
    impl_->appsrc = GST_ELEMENT(gst_object_ref(src));
    if (impl_->last) impl_->push_locked(impl_->last); // a new viewer's first frame now, not at the next damage
    return GST_BIN(bin);
}

StreamReader::StreamReader(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
StreamReader::~StreamReader() = default;

} // namespace fjarr::desktop::mutter
