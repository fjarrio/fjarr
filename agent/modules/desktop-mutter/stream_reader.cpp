// spec: docs/23-agent-core-architecture.md#desktop-helper-protocol (The stream reader and the cursor)
// spec: docs/23-agent-core-architecture.md#desktop-capture-memory (ADR-0041: DMA-BUF first, SHM as the fallback)
#include "stream_reader.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <unistd.h>

#include <pipewire/pipewire.h>
#include <spa/buffer/meta.h>
#include <spa/param/video/format-utils.h>

namespace fjarr::desktop::mutter {

namespace {
constexpr int MAX_SIDE = 384; // docs/08: cursor images at most 384×384
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
    spa_source* first_frame_watch = nullptr; // re-links when a stream stays without a frame (below)
    spa_source* release_event = nullptr;     // GStreamer released a DMA-BUF frame: give it back to mutter
    std::uint32_t node = 0;
    Options options;
    GMainContext* ctx = nullptr;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true); // posts that outlive the reader are dropped
    Shape shape;
    Position position;
    Log log;
    std::function<void()> stalled;
    std::function<void(bool)> memory_cb;
    std::function<bool(const std::string&)> memory_changed_cb;
    std::function<void(const std::string&)> fallback_cb, failed_cb;
    std::unique_ptr<Frames> frames;

    // What mutter's node offers, asked before linking (docs/23#desktop-capture-memory), PipeWire thread.
    pw_registry* registry = nullptr;
    spa_hook registry_listener{};
    pw_proxy* node_proxy = nullptr;
    spa_hook node_listener{};
    spa_hook core_listener{};
    bool core_listening = false;
    int sync_seq = -1;
    int sync_phase = 0;
    std::vector<spa_pod*> producer_params; // copies of the node's EnumFormat params
    ProducerFormats producer;              // what was read of them, for the offer and the SHM reason
    Offer offer;

    // The memory, settled at the first fixed format; read by status() on the core loop.
    mutable std::mutex state_mu;
    bool memory_settled = false;
    bool memory_dmabuf = false;
    std::string drm_format, shm_reason;
    bool latest_dmabuf = false;  // the latest fixed format's memory, which a capture that keeps
    std::string latest_drm;      // its stream after a change adopts (adopt_memory)
    bool change_pending = false; // a change of memory went to the core loop and is not decided yet

    // Touched only on the PipeWire thread:
    int width = 0, height = 0;
    spa_video_format format = SPA_VIDEO_FORMAT_UNKNOWN;
    ReaderCursor cursor; // what each buffer's cursor metadata hands on (desktop/cursor_image.hpp)
    int last_x = -1, last_y = -1;
    bool first_frame = true;
    bool fallen_back = false; // its DMA-BUF could not be wrapped: restarted on shared memory
    unsigned buffers = 0, empty = 0;
    std::string last_empty; // what the last buffer without pixels looked like, for the log
    static constexpr long FIRST_FRAME_MS = 500;

    bool wants_producer_formats() const {
        const auto& m = options.memory;
        return !m.virtual_monitor && !m.force_shm && !m.imports.empty();
    }

    /// Ask mutter's node for its EnumFormat (the modifiers it can allocate), then link. Two round
    /// trips on the narrowed connection, which grants the node R and X (docs/23#desktop-descriptor-handover).
    void ask_producer_formats() {
        static const pw_registry_events REG = [] {
            pw_registry_events e{};
            e.version = PW_VERSION_REGISTRY_EVENTS;
            e.global = [](void* d, std::uint32_t id, std::uint32_t, const char* type, std::uint32_t, const spa_dict*) { static_cast<Impl*>(d)->on_global(id, type); };
            return e;
        }();
        static const pw_core_events CORE = [] {
            pw_core_events e{};
            e.version = PW_VERSION_CORE_EVENTS;
            e.done = [](void* d, std::uint32_t id, int seq) { static_cast<Impl*>(d)->on_done(id, seq); };
            e.error = [](void* d, std::uint32_t id, int seq, int res, const char* message) { static_cast<Impl*>(d)->on_core_error(id, seq, res, message); };
            return e;
        }();
        registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
        pw_registry_add_listener(registry, &registry_listener, &REG, this);
        pw_core_add_listener(core, &core_listener, &CORE, this);
        core_listening = true;
        sync_phase = 1;
        sync_seq = pw_core_sync(core, PW_ID_CORE, 0);
    }
    void on_global(std::uint32_t id, const char* type) {
        if (id != node || node_proxy || std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0) return;
        static const pw_node_events NODE = [] {
            pw_node_events e{};
            e.version = PW_VERSION_NODE_EVENTS;
            e.param = [](void* d, int, std::uint32_t id, std::uint32_t, std::uint32_t, const spa_pod* param) {
                auto* self = static_cast<Impl*>(d);
                if (id != SPA_PARAM_EnumFormat || !param) return;
                auto* copy = static_cast<spa_pod*>(std::malloc(SPA_POD_SIZE(param)));
                std::memcpy(copy, param, SPA_POD_SIZE(param));
                self->producer_params.push_back(copy);
            };
            return e;
        }();
        node_proxy = static_cast<pw_proxy*>(pw_registry_bind(registry, id, type, PW_VERSION_NODE, 0));
        if (!node_proxy) return;
        pw_proxy_add_object_listener(node_proxy, &node_listener, &NODE, this);
        pw_node_enum_params(reinterpret_cast<pw_node*>(node_proxy), 0, SPA_PARAM_EnumFormat, 0, UINT32_MAX, nullptr);
    }
    void on_done(std::uint32_t id, int seq) {
        if (id != PW_ID_CORE || seq != sync_seq || sync_phase == 0) return;
        if (sync_phase == 1 && node_proxy) {
            sync_phase = 2; // the params asked for after the first sync arrive before this one's done
            sync_seq = pw_core_sync(core, PW_ID_CORE, 0);
            return;
        }
        if (!node_proxy) {
            // docs/23#desktop-capture-memory: the whole import list goes out, and the mini-PC run
            // must be able to see that it did.
            post([this] { log(2, "capture of " + options.name + ": mutter's formats could not be read: node " + std::to_string(node) + " is not in the registry"); });
        }
        finish_asking(node_proxy != nullptr);
    }
    void on_core_error(std::uint32_t id, int, int res, const char* message) {
        if (sync_phase == 0) return;
        post([this, id, res, m = std::string(message ? message : "")] {
            log(2, "capture of " + options.name + ": mutter's formats could not be read (" + std::to_string(id) + ": " + std::to_string(res) + " " + m + ")");
        });
        finish_asking(false);
    }
    void finish_asking(bool known) {
        sync_phase = 0;
        ProducerFormats pf;
        if (known && producer_params.empty()) {
            post([this] { log(2, "capture of " + options.name + ": mutter's formats could not be read: its node listed none"); });
        } else if (known) {
            std::vector<const spa_pod*> params(producer_params.begin(), producer_params.end());
            pf = parse_producer_formats(params);
        }
        for (spa_pod* p : producer_params) std::free(p);
        producer_params.clear();
        drop_asking();
        // Linked from here, not from start(): a failure has no caller to return to, and a stream
        // never linked never streams, so no first-frame watch would ever notice it.
        if (!link(pf)) post([this] { fail("the stream reader could not link to the display server's stream"); });
    }
    /// The capture cannot go on (core loop): the backend marks its source lost, by name.
    void fail(const std::string& why) {
        auto cb = failed_cb; // the backend drops this reader in it
        if (cb) cb(why);
    }
    void drop_asking() {
        if (node_proxy) {
            spa_hook_remove(&node_listener);
            pw_proxy_destroy(node_proxy);
            node_proxy = nullptr;
        }
        if (registry) {
            spa_hook_remove(&registry_listener);
            pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry));
            registry = nullptr;
        }
        if (core_listening) {
            spa_hook_remove(&core_listener);
            core_listening = false;
        }
    }

    /// Link to the node with the offer (docs/23#desktop-capture-memory, Negotiation).
    bool link(const ProducerFormats& pf) {
        producer = pf;
        offer = plan_offer(options.memory, producer);
        std::uint8_t buf[4096];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
        auto params = build_enum_formats(offer, &b, options.width, options.height);
        if (params.size() < offer.dmabuf.size() + (offer.shm.empty() ? 0 : 1))
            post([this] { log(2, "capture of " + options.name + ": the format offer did not fit its 4 KiB builder; some formats were left out"); });
        // PipeWire maps shared memory for us, but a stream that offers DMA-BUF maps nothing:
        // Frames maps a MemFd itself should mutter pick shared memory after all.
        const auto flags = static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | (offer.dmabuf_offered() ? 0 : PW_STREAM_FLAG_MAP_BUFFERS));
        const int res = pw_stream_connect(stream, PW_DIRECTION_INPUT, node, flags, params.data(), static_cast<std::uint32_t>(params.size()));
        if (res < 0) post([this, res] { log(3, "capture of " + options.name + ": the stream reader could not link to node " + std::to_string(node) + " (" + std::to_string(res) + ")"); });
        return res >= 0;
    }

    /// A stream that streams without ever giving a frame: mutter sometimes does not send a freshly
    /// plugged monitor's first frame to a consumer that links the moment its node appears, and on a
    /// still screen no other comes (seen in 3 of 6 hot-plugs, 2026-10-03). A consumer that links later
    /// is sent one, so the reader links again — at most three times.
    void on_state(pw_stream_state state, const char* error) {
        if (state == PW_STREAM_STATE_ERROR) {
            const std::string why = error ? error : "?";
            post([this, why] { log(2, "capture of " + options.name + ": the stream failed: " + why); });
        }
        if (state != PW_STREAM_STATE_STREAMING || !first_frame) return;
        timespec value{0, FIRST_FRAME_MS * 1'000'000L}, none{0, 0};
        pw_loop_update_timer(pw_thread_loop_get_loop(thread), first_frame_watch, &value, &none, false);
    }
    void on_first_frame_watch() {
        if (!first_frame || fallen_back) return; // fallen back: restarted on shared memory already
        const std::string note = "no frame " + std::to_string(FIRST_FRAME_MS) + " ms into streaming (" + std::to_string(buffers) + " buffer(s), " +
                                 std::to_string(empty) + " without pixels, the last " + last_empty + ")";
        post([this, note] {
            log(2, note);
            if (stalled) stalled();
        });
    }

    ~Impl() {
        *alive = false;
        if (thread) pw_thread_loop_stop(thread);
        if (stream) pw_stream_destroy(stream); // its remove_buffer events still reach `frames`
        drop_asking();
        for (spa_pod* p : producer_params) std::free(p);
        frames.reset(); // buffers GStreamer still holds keep their own descriptors
        if (core) pw_core_disconnect(core);
        if (context) pw_context_destroy(context);
        if (thread) pw_thread_loop_destroy(thread);
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

    void on_param_changed(std::uint32_t id, const spa_pod* param) {
        if (!param || id != SPA_PARAM_Format) return;
        const auto nf = parse_format(param);
        if (!nf) return;
        // The buffers for that memory, and the cursor. Never SyncTimeline (ADR-0041: implicit sync).
        std::uint8_t buf[1024];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
        auto params = build_buffer_params(nf->dmabuf, &b);
        pw_stream_update_params(stream, params.data(), static_cast<std::uint32_t>(params.size()));
        if (!nf->fixed) return; // mutter, which allocates, fixates the modifier and sends the format again
        width = nf->width;
        height = nf->height;
        format = nf->format;
        MemorySettle kind;
        {
            std::lock_guard<std::mutex> lk(state_mu);
            latest_dmabuf = nf->dmabuf;
            latest_drm = nf->dmabuf ? nf->drm_format() : "";
            kind = settle_kind(memory_settled ? std::optional<bool>(memory_dmabuf) : std::nullopt, nf->dmabuf);
        }
        if (kind == MemorySettle::Changed) {
            // spec: docs/23-agent-core-architecture.md#desktop-capture-memory (a capture keeps its memory)
            // The track's pipeline was built for the old memory: nothing more goes into it until
            // the core loop has decided, restarting the capture or adopting the new memory.
            frames->suspend();
            frames->set_format(*nf);
            memory_changed(*nf);
            return;
        }
        frames->set_format(*nf);
        settle_memory(*nf, kind);
    }

    /// The first fixed format settles the capture's memory; logged once, with the reason for SHM.
    /// A later one in the same memory only follows a change of DMA-BUF modifier in /sources.
    void settle_memory(const NegotiatedFormat& nf, MemorySettle kind) {
        std::unique_lock<std::mutex> lk(state_mu);
        if (kind == MemorySettle::Same) {
            if (!nf.dmabuf || nf.drm_format() == drm_format) return;
            drm_format = nf.drm_format();
            lk.unlock();
            const std::string line = "capture of " + options.name + ": DMA-BUF " + nf.drm_format() + " from now on (renegotiated)";
            post([this, line] { log(1, line); });
            return;
        }
        memory_settled = true;
        memory_dmabuf = nf.dmabuf;
        std::string line;
        int level = 1;
        if (nf.dmabuf) {
            drm_format = nf.drm_format();
            line = "capture of " + options.name + ": DMA-BUF " + drm_format + ", into the encoder without a copy";
        } else {
            shm_reason = settled_shm_reason(offer, producer);
            line = "capture of " + options.name + ": shared memory (" + shm_reason + ")";
            if (!options.memory.virtual_monitor) {
                // A physical monitor on SHM still freezes behind a window scanned out directly (ADR-0041).
                level = 2;
                line += "; a fullscreen window scanned out directly will freeze this capture";
            }
        }
        lk.unlock();
        const bool dmabuf = nf.dmabuf;
        post([this, level, line, dmabuf] {
            log(level, line);
            if (memory_cb) memory_cb(dmabuf);
        });
    }

    /// A renegotiation changed the memory: the backend restarts the capture (true), or this reader
    /// adopts the latest memory and feeds the track again (docs/23#desktop-capture-memory).
    void memory_changed(const NegotiatedFormat& nf) {
        {
            std::lock_guard<std::mutex> lk(state_mu);
            if (change_pending) return; // one decision at a time; adopt_memory takes the latest
            change_pending = true;
        }
        const std::string why = std::string("its memory changed to ") + (nf.dmabuf ? "DMA-BUF " + nf.drm_format() : "shared memory");
        post([this, why] {
            auto cb = memory_changed_cb;
            if (cb && cb(why)) return; // restarted: a new reader replaces this one, which feeds nothing meanwhile
            adopt_memory();
        });
    }
    /// Core loop: the capture keeps this stream in its new memory. The source's caps follow it,
    /// and the producer built for the old memory fails to negotiate and is rebuilt from them.
    void adopt_memory() {
        bool dmabuf;
        std::string line;
        {
            std::lock_guard<std::mutex> lk(state_mu);
            change_pending = false;
            dmabuf = latest_dmabuf;
            memory_dmabuf = dmabuf;
            if (dmabuf) {
                drm_format = latest_drm;
                shm_reason.clear();
                line = "DMA-BUF " + drm_format;
            } else {
                drm_format.clear();
                shm_reason = "mutter moved the stream to shared memory in a renegotiation";
                line = "shared memory (" + shm_reason + ")";
            }
        }
        log(2, "capture of " + options.name + ": goes on in " + line + "; the track's pipeline is rebuilt for it");
        if (memory_cb) memory_cb(dmabuf);
        frames->resume();
    }

    /// mutter's buffer arrived. One that cannot be wrapped as DMA-BUF moves the capture to shared
    /// memory: the backend restarts it with that reason (docs/23#desktop-capture-memory).
    void on_add_buffer(pw_buffer* b) {
        frames->add_buffer(b);
        auto why = frames->take_unwrappable();
        if (!why) return;
        fallen_back = true;
        frames->suspend();
        post([this, why = *why] {
            log(2, "capture of " + options.name + ": " + why + "; restarting it on shared memory");
            auto cb = fallback_cb;
            if (cb) cb(why);
        });
    }

    void on_process() {
        pw_buffer* b = pw_stream_dequeue_buffer(stream);
        if (!b) return;
        spa_buffer* sb = b->buffer;
        buffers++;
        // The cursor first: a buffer queued back below is mutter's again.
        if (auto* mc = static_cast<spa_meta_cursor*>(spa_buffer_find_meta_data(sb, SPA_META_Cursor, sizeof(spa_meta_cursor)))) {
            if (spa_meta_cursor_is_valid(mc)) read_cursor(mc);
            else hidden_cursor();
        }
        // Pixels or only the cursor, from the chunk alone: a DMA-BUF is never mapped. A frame
        // counts for the first-frame watch only once one was made (pushed or held).
        const Delivery delivered = frames->on_buffer(b);
        if (made_frame(delivered)) {
            if (first_frame) {
                first_frame = false;
                const std::string note = "first frame " + std::to_string(width) + "x" + std::to_string(height) + " after " + std::to_string(buffers) +
                                         " buffer(s), " + std::to_string(empty) + " without a frame";
                post([this, note] { log(1, note); });
            }
        } else {
            empty++;
            if (sb->n_datas > 0) {
                const spa_data& d = sb->datas[0];
                last_empty = std::string(delivered == Delivery::Dropped ? "pixels but no frame made of them, " : "") + "type " + std::to_string(d.type) +
                             ", chunk " + (d.chunk ? std::to_string(d.chunk->size) + " bytes flags " + std::to_string(d.chunk->flags) : std::string("none")) +
                             ", size " + std::to_string(width) + "x" + std::to_string(height) + ", format " + std::to_string(format);
            } else {
                last_empty = "no datas";
            }
        }
        if (requeue_now(delivered)) pw_stream_queue_buffer(stream, b); // straight back: the producer must never wait on us
    }

    void send_shape(std::optional<CursorImage> img) {
        if (!img) return;
        post([this, img = std::move(*img)] {
            if (shape) shape(img);
        });
    }

    /// An invalid cursor id (0): the robot's application hid the pointer, or it is not on this
    /// monitor. Sent on as a hidden shape marked as such (docs/22#cursor-strategy); the backend
    /// tells the two apart by where the pointer was last seen (CursorArbiter).
    void hidden_cursor() {
        last_x = last_y = -1; // its next position is news, wherever it reappears
        send_shape(cursor.invalid_id());
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
            if (!img.visible || img.width > 0) send_shape(cursor.bitmap(std::move(img)));
        } else {
            send_shape(cursor.no_bitmap()); // back from an invalid id: the shape before it, if any
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
    e.state_changed = [](void* d, pw_stream_state, pw_stream_state state, const char* error) { static_cast<StreamReader::Impl*>(d)->on_state(state, error); };
    e.add_buffer = [](void* d, pw_buffer* b) { static_cast<StreamReader::Impl*>(d)->on_add_buffer(b); };
    e.remove_buffer = [](void* d, pw_buffer* b) { static_cast<StreamReader::Impl*>(d)->frames->remove_buffer(b); };
    e.process = [](void* d) { static_cast<StreamReader::Impl*>(d)->on_process(); };
    return e;
}();
} // namespace

std::unique_ptr<StreamReader> StreamReader::start(GMainContext* ctx, int fd, std::uint32_t node, Options options, Shape shape, Position position, Log log) {
    static const bool initialized = [] {
        pw_init(nullptr, nullptr);
        return true;
    }();
    (void)initialized;
    auto impl = std::make_unique<Impl>();
    impl->ctx = ctx;
    impl->node = node;
    impl->options = std::move(options);
    if (impl->options.keepalive_ms <= 0) impl->options.keepalive_ms = 100;
    impl->shape = std::move(shape);
    impl->position = std::move(position);
    impl->log = std::move(log);
    impl->thread = pw_thread_loop_new("fjarr-capture", nullptr);
    // The package's own client configuration where it is installed, PipeWire's default otherwise
    // (a development tree): see packaging/pipewire/fjarr-pipewire-client.conf for why.
    static constexpr const char* OWN_CONF = "/usr/share/fjarr/pipewire-client.conf";
    pw_properties* conf = ::access(OWN_CONF, R_OK) == 0 ? pw_properties_new(PW_KEY_CONFIG_NAME, OWN_CONF, nullptr) : nullptr;
    impl->context = impl->thread ? pw_context_new(pw_thread_loop_get_loop(impl->thread), conf, 0) : nullptr;
    if (!impl->thread && conf) pw_properties_free(conf);
    if (!impl->context || pw_thread_loop_start(impl->thread) < 0) {
        ::close(fd);
        impl->log(2, "the stream reader could not start a PipeWire loop");
        return nullptr;
    }
    pw_thread_loop_lock(impl->thread);
    pw_loop* loop = pw_thread_loop_get_loop(impl->thread);
    // A released DMA-BUF frame is posted from whichever GStreamer thread let go of it, and given back
    // to mutter here, on the PipeWire thread (docs/23#desktop-capture-memory, Held while GStreamer holds the frame).
    impl->release_event = pw_loop_add_event(loop, [](void* d, std::uint64_t) { static_cast<Impl*>(d)->frames->drain_releases(); }, impl.get());
    Impl* raw = impl.get();
    impl->frames = std::make_unique<Frames>([raw](pw_buffer* b) { pw_stream_queue_buffer(raw->stream, b); },
                                            [loop, ev = impl->release_event] { pw_loop_signal_event(loop, ev); });
    impl->core = pw_context_connect_fd(impl->context, fd, nullptr, 0); // takes the fd
    bool ok = impl->core != nullptr;
    if (ok) {
        impl->stream = pw_stream_new(impl->core, "fjarr-capture",
                                     pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Screen", nullptr));
        pw_stream_add_listener(impl->stream, &impl->stream_listener, &EVENTS, impl.get());
        impl->first_frame_watch = pw_loop_add_timer(loop, [](void* d, std::uint64_t) { static_cast<Impl*>(d)->on_first_frame_watch(); }, impl.get());
        // A physical monitor with an import list: ask mutter's node what it can allocate first, so
        // the offer is the intersection (and an empty one is named). Otherwise link at once.
        if (impl->wants_producer_formats()) impl->ask_producer_formats();
        else ok = impl->link(ProducerFormats{});
        if (ok) {
            // The keepalive: mutter is silent on a still screen, and the encoder must keep producing.
            impl->keepalive = pw_loop_add_timer(loop, [](void* d, std::uint64_t) {
                auto* self = static_cast<Impl*>(d);
                self->frames->keepalive(self->options.keepalive_ms);
            }, impl.get());
            timespec value{0, static_cast<long>(impl->options.keepalive_ms) * 1'000'000L}, interval = value;
            pw_loop_update_timer(loop, impl->keepalive, &value, &interval, false);
        }
    }
    pw_thread_loop_unlock(impl->thread);
    if (!ok) {
        impl->log(2, "the stream reader could not link to node " + std::to_string(node));
        return nullptr;
    }
    return std::unique_ptr<StreamReader>(new StreamReader(std::move(impl)));
}

void StreamReader::attach(GstElement* appsrc) { impl_->frames->attach(appsrc); }
void StreamReader::on_stalled(std::function<void()> cb) { impl_->stalled = std::move(cb); }
void StreamReader::on_memory(std::function<void(bool)> cb) { impl_->memory_cb = std::move(cb); }
void StreamReader::on_memory_changed(std::function<bool(const std::string&)> cb) { impl_->memory_changed_cb = std::move(cb); }
void StreamReader::on_fallback(std::function<void(const std::string&)> cb) { impl_->fallback_cb = std::move(cb); }
void StreamReader::on_failed(std::function<void(const std::string&)> cb) { impl_->failed_cb = std::move(cb); }

nlohmann::json StreamReader::status() const {
    const FrameCounts c = impl_->frames->counts();
    nlohmann::json frames{{"new", c.fresh}, {"repeated", c.repeated}, {"cursor_only", c.cursor_only}, {"newest_age_ms", nullptr}};
    if (c.newest) frames["newest_age_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - *c.newest).count();
    nlohmann::json memory = nullptr;
    {
        std::lock_guard<std::mutex> lk(impl_->state_mu);
        if (impl_->memory_settled)
            memory = impl_->memory_dmabuf ? nlohmann::json{{"kind", "dmabuf"}, {"drm_format", impl_->drm_format}}
                                          : nlohmann::json{{"kind", "shm"}, {"reason", impl_->shm_reason}};
    }
    return {{"frames", frames}, {"memory", memory}};
}

StreamReader::StreamReader(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
StreamReader::~StreamReader() = default;

} // namespace fjarr::desktop::mutter
