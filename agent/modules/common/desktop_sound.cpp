// spec: docs/23-agent-core-architecture.md#desktop-audio · #desktop-helper-protocol (Sound)
#include "common/desktop_sound.hpp"

#include "common/audio_reader.hpp"

namespace fjarr::desktop {

/// The desktop's sound (docs/23#desktop-audio): available while the helper reports an output; captured
/// only while the producer's pipeline is running, so a robot nobody listens to keeps its sink asleep.
/// The producer keeps a stopped pipeline built (in NULL) rather than releasing the source, so what
/// says "nobody listens" is the appsrc's state, watched on the core loop.
class SoundSource final : public VideoSource {
  public:
    /// `want(true)` when the pipeline starts running, `want(false)` when it stops; on the core loop.
    SoundSource(GMainContext* ctx, std::function<void(bool)> want) : ctx_(ctx), want_(std::move(want)) {}
    ~SoundSource() override {
        *alive_ = false;
        if (poll_) g_source_destroy(poll_), g_source_unref(poll_);
        if (appsrc_) gst_object_unref(appsrc_);
    }

    // What a desktop plays is music or a video's sound, not speech (docs/23#audio).
    SourceInfo describe() const override { return {"desktop:sound", {{"src", TrackKind::Audio, "audio/x-raw", AudioContent::Music}}}; }

    GstBin* create_bin() override {
        if (!available()) return nullptr;
        GstElement* src = gst_element_factory_make("appsrc", nullptr);
        if (!src) return nullptr;
        GstCaps* caps = AudioReader::caps();
        g_object_set(src, "is-live", TRUE, "format", GST_FORMAT_TIME, "do-timestamp", TRUE, "caps", caps, "max-buffers", static_cast<guint64>(50),
                     "leaky-type", 2 /* downstream: drop the oldest */, nullptr);
        gst_caps_unref(caps);
        GstElement* bin = gst_bin_new(nullptr);
        gst_bin_add(GST_BIN(bin), src);
        GstPad* pad = gst_element_get_static_pad(src, "src");
        gst_element_add_pad(bin, gst_ghost_pad_new("src", pad));
        gst_object_unref(pad);
        if (appsrc_) gst_object_unref(appsrc_);
        appsrc_ = GST_ELEMENT(gst_object_ref(src));
        if (reader_) reader_->attach(appsrc_);
        // The producer starts the pipeline right after building it, in this same dispatch: look once
        // that is done, and every 500 ms after for its stop and any restart.
        struct Job {
            std::shared_ptr<bool> alive;
            SoundSource* self;
        };
        GSource* once = g_idle_source_new();
        g_source_set_callback(once, [](gpointer d) -> gboolean {
            auto* j = static_cast<Job*>(d);
            if (*j->alive) j->self->check();
            return G_SOURCE_REMOVE;
        }, new Job{alive_, this}, [](gpointer d) { delete static_cast<Job*>(d); });
        g_source_attach(once, ctx_);
        g_source_unref(once);
        if (!poll_) {
            poll_ = g_timeout_source_new(500);
            g_source_set_callback(poll_, [](gpointer d) -> gboolean {
                static_cast<SoundSource*>(d)->check();
                return G_SOURCE_CONTINUE;
            }, this, nullptr);
            g_source_attach(poll_, ctx_);
        }
        return GST_BIN(bin);
    }

    bool available() const override { return available_; }
    void on_availability_changed(std::function<void(bool)> cb) override { availability_ = std::move(cb); }
    std::string unavailable_reason() const override { return available_ ? "" : reason_; }

    void set_available(bool now, const std::string& why = {}) {
        reason_ = why;
        if (now == available_) return;
        available_ = now;
        if (availability_) availability_(now);
    }
    /// The helper handed the sound over: its reader feeds the pipeline already built.
    void ready(std::unique_ptr<AudioReader> reader) {
        reader_ = std::move(reader);
        if (reader_ && appsrc_) reader_->attach(appsrc_);
    }
    void drop_reader() { reader_.reset(); }
    /// The backend is going; the media plane may hold this source a while longer.
    void detach() {
        want_ = nullptr;
        reader_.reset();
        set_available(false, "the desktop backend stopped");
    }

  private:
    /// Running = the appsrc is (or is going) PAUSED or PLAYING. Only a change is reported.
    void check() {
        GstState cur = GST_STATE_NULL, pending = GST_STATE_VOID_PENDING;
        if (appsrc_) gst_element_get_state(appsrc_, &cur, &pending, 0);
        const bool running = cur >= GST_STATE_PAUSED || pending >= GST_STATE_PAUSED;
        if (running != running_) {
            running_ = running;
            if (!running) reader_.reset();
            if (want_) want_(running);
        }
    }

    GMainContext* ctx_;
    std::function<void(bool)> want_;
    std::unique_ptr<AudioReader> reader_;
    GstElement* appsrc_ = nullptr; // the newest pipeline's source
    GSource* poll_ = nullptr; // every 500 ms on the core loop, from the first pipeline on
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    bool running_ = false;
    bool available_ = false;
    std::string reason_ = "waiting for the desktop session";
    std::function<void(bool)> availability_;
};


struct DesktopSound::Impl {
    GMainContext* ctx;
    Send send;
    Log log;
    std::shared_ptr<SoundSource> sound;
    bool present = false;  // a helper is welcomed
    std::string speakers;  // its default output; empty = none
    bool paused = false;
    std::string paused_why;
    bool wanted = false;   // the producer's pipeline runs
    int id = 0;            // the start-audio in flight or answered; 0 = none
    int next_id = 1;

    /// Available while a helper reports an output and the sound is not paused (docs/23#desktop-audio).
    void update() {
        if (!sound) return;
        if (!present) sound->set_available(false, "no desktop session");
        else if (speakers.empty()) sound->set_available(false, "the desktop has no output");
        else if (paused) sound->set_available(false, paused_why);
        else sound->set_available(true);
    }
    /// Asks the helper for the speakers while someone listens; a new request replaces any earlier one.
    void request() {
        if (!sound || !wanted || !sound->available()) return;
        id = next_id++;
        send({{"type", "start-audio"}, {"id", id}});
    }
    void drop(const std::string& why) {
        if (!sound) return;
        if (id || wanted) log(1, "desktop sound stopped: " + why);
        sound->drop_reader();
        id = 0;
    }
};

DesktopSound::DesktopSound(GMainContext* ctx, Send send, Log log) : impl_(std::make_unique<Impl>()) {
    impl_->ctx = ctx;
    impl_->send = std::move(send);
    impl_->log = std::move(log);
}

DesktopSound::~DesktopSound() {
    if (impl_->sound) impl_->sound->detach(); // the media plane may hold the source a while longer
}

std::shared_ptr<VideoSource> DesktopSound::source() {
    if (!impl_->sound) {
        impl_->sound = std::make_shared<SoundSource>(impl_->ctx, [this](bool want) {
            impl_->wanted = want;
            if (want) impl_->request();
            else impl_->id = 0; // a late audio-started is dropped; closing the reader ended the capture
        });
        impl_->update();
    }
    return impl_->sound;
}

void DesktopSound::helper(bool present) {
    if (present == impl_->present) return;
    impl_->present = present;
    if (!present) {
        impl_->speakers.clear();
        impl_->drop("the desktop session ended");
    }
    impl_->update();
}

void DesktopSound::pause(bool paused, const std::string& why) {
    if (paused == impl_->paused) return;
    impl_->paused = paused;
    impl_->paused_why = why;
    if (paused) impl_->drop(why);
    impl_->update();
    if (!paused) impl_->request();
}

bool DesktopSound::handle(proto::Message& m) {
    const std::string type = m.type();
    if (type == "speakers") {
        // A new default output restarts a running capture on it.
        const nlohmann::json sink = m.body.value("sink", nlohmann::json());
        const std::string name = sink.is_object() ? sink.value("name", std::string{}) : std::string{};
        if (name == impl_->speakers) return true;
        impl_->log(1, name.empty() ? std::string("the desktop has no output") : "the desktop's output is " + name);
        impl_->speakers = name;
        impl_->drop("the output changed");
        impl_->update();
        impl_->request();
    } else if (type == "audio-started") {
        if (m.body.value("id", 0) != impl_->id || !impl_->sound || m.fds.empty()) return true; // a stale answer: its descriptor closes
        const std::string name = m.body.value("name", std::string{});
        impl_->sound->ready(AudioReader::start(impl_->ctx, m.fds[0].release(), name, impl_->log));
        impl_->log(1, "desktop sound from " + name);
    } else if (type == "audio-failed") {
        if (m.body.value("id", 0) != impl_->id) return true;
        impl_->id = 0;
        impl_->log(2, "the helper could not hand the sound over: " + m.body.value("reason", std::string{"?"}));
    } else {
        return false;
    }
    return true;
}

} // namespace fjarr::desktop
