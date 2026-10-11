// spec: docs/23-agent-core-architecture.md#desktop-audio
#include "desktop/audio_reader.hpp"

#include <algorithm>
#include <mutex>
#include <unistd.h>

#include <gst/app/gstappsrc.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

namespace fjarr::desktop {

namespace {
constexpr int RATE = 48000, CHANNELS = 2, FRAME_BYTES = CHANNELS * 2; // S16LE interleaved
} // namespace

struct AudioReader::Impl {
    pw_thread_loop* thread = nullptr;
    pw_context* context = nullptr;
    pw_core* core = nullptr;
    pw_stream* stream = nullptr;
    spa_hook stream_listener{};
    GMainContext* ctx = nullptr;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
    Log log;
    bool logged_first = false; // touched only on the PipeWire thread

    std::mutex mu;
    GstElement* appsrc = nullptr; // the current pipeline's source, ref held

    ~Impl() {
        *alive = false;
        if (thread) pw_thread_loop_stop(thread);
        if (stream) pw_stream_destroy(stream);
        if (core) pw_core_disconnect(core);
        if (context) pw_context_destroy(context);
        if (thread) pw_thread_loop_destroy(thread);
        std::lock_guard<std::mutex> lk(mu);
        if (appsrc) gst_object_unref(appsrc);
    }

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

    void on_process() {
        pw_buffer* b = pw_stream_dequeue_buffer(stream);
        if (!b) return;
        const spa_buffer* sb = b->buffer;
        if (sb->n_datas > 0 && sb->datas[0].data && sb->datas[0].chunk) {
            const spa_data& d = sb->datas[0];
            const std::uint32_t offset = std::min(d.chunk->offset, d.maxsize);
            const std::uint32_t size = std::min(d.chunk->size, d.maxsize - offset) / FRAME_BYTES * FRAME_BYTES;
            if (size > 0) {
                GstBuffer* out = gst_buffer_new_memdup(static_cast<const std::uint8_t*>(d.data) + offset, size);
                {
                    std::lock_guard<std::mutex> lk(mu);
                    if (appsrc) gst_app_src_push_buffer(GST_APP_SRC(appsrc), out); // takes it; do-timestamp stamps it
                    else gst_buffer_unref(out);
                }
                if (!logged_first) {
                    logged_first = true;
                    post([this, size] { log(1, "desktop sound: first buffer, " + std::to_string(size / FRAME_BYTES) + " frames"); });
                }
            }
        }
        pw_stream_queue_buffer(stream, b);
    }

    void on_state(pw_stream_state state, const char* error) {
        if (state != PW_STREAM_STATE_ERROR) return;
        const std::string why = error ? error : "unknown";
        post([this, why] { log(2, "desktop sound stream failed: " + why); });
    }
};

namespace {
const pw_stream_events EVENTS = [] {
    pw_stream_events e{};
    e.version = PW_VERSION_STREAM_EVENTS;
    e.state_changed = [](void* d, pw_stream_state, pw_stream_state state, const char* error) { static_cast<AudioReader::Impl*>(d)->on_state(state, error); };
    e.process = [](void* d) { static_cast<AudioReader::Impl*>(d)->on_process(); };
    return e;
}();
} // namespace

GstCaps* AudioReader::caps() {
    return gst_caps_new_simple("audio/x-raw", "format", G_TYPE_STRING, "S16LE", "rate", G_TYPE_INT, RATE, "channels", G_TYPE_INT, CHANNELS, "layout",
                               G_TYPE_STRING, "interleaved", "channel-mask", GST_TYPE_BITMASK, static_cast<guint64>(0x3), nullptr);
}

std::unique_ptr<AudioReader> AudioReader::start(GMainContext* ctx, int fd, const std::string& sink, Log log) {
    static const bool initialized = [] {
        pw_init(nullptr, nullptr);
        return true;
    }();
    (void)initialized;
    auto impl = std::make_unique<Impl>();
    impl->ctx = ctx;
    impl->log = std::move(log);
    impl->thread = pw_thread_loop_new("fjarr-sound", nullptr);
    // The package's own client configuration where it is installed, as the stream reader does
    // (packaging/pipewire/fjarr-pipewire-client.conf).
    static constexpr const char* OWN_CONF = "/usr/share/fjarr/pipewire-client.conf";
    pw_properties* conf = ::access(OWN_CONF, R_OK) == 0 ? pw_properties_new(PW_KEY_CONFIG_NAME, OWN_CONF, nullptr) : nullptr;
    impl->context = impl->thread ? pw_context_new(pw_thread_loop_get_loop(impl->thread), conf, 0) : nullptr;
    if (!impl->thread && conf) pw_properties_free(conf);
    if (!impl->context || pw_thread_loop_start(impl->thread) < 0) {
        ::close(fd);
        impl->log(2, "the audio reader could not start a PipeWire loop");
        return nullptr;
    }
    pw_thread_loop_lock(impl->thread);
    impl->core = pw_context_connect_fd(impl->context, fd, nullptr, 0); // takes the fd
    bool ok = impl->core != nullptr;
    if (ok) {
        // The sink's monitor: what its applications play (stream.capture.sink), named so the session
        // manager links nothing else (spikes/desktop-audio).
        impl->stream = pw_stream_new(impl->core, "fjarr-desktop-sound",
                                     pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Music",
                                                       PW_KEY_STREAM_CAPTURE_SINK, "true", PW_KEY_TARGET_OBJECT, sink.c_str(), nullptr));
        pw_stream_add_listener(impl->stream, &impl->stream_listener, &EVENTS, impl.get());
        std::uint8_t buf[512];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
        spa_audio_info_raw info{};
        info.format = SPA_AUDIO_FORMAT_S16_LE;
        info.rate = RATE;
        info.channels = CHANNELS;
        info.position[0] = SPA_AUDIO_CHANNEL_FL;
        info.position[1] = SPA_AUDIO_CHANNEL_FR;
        const spa_pod* params[1] = {spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info)};
        ok = pw_stream_connect(impl->stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                               static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS), params, 1) >= 0;
    }
    pw_thread_loop_unlock(impl->thread);
    if (!ok) {
        impl->log(2, "the audio reader could not link to " + sink);
        return nullptr;
    }
    return std::unique_ptr<AudioReader>(new AudioReader(std::move(impl)));
}

void AudioReader::attach(GstElement* appsrc) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (impl_->appsrc == appsrc) return;
    if (impl_->appsrc) gst_object_unref(impl_->appsrc);
    impl_->appsrc = appsrc ? GST_ELEMENT(gst_object_ref(appsrc)) : nullptr;
}

AudioReader::AudioReader(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
AudioReader::~AudioReader() = default;

} // namespace fjarr::desktop
