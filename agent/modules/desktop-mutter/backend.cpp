// Backend E (ADR-0006): GNOME's mutter, reached through fjarr-desktop-session (ADR-0028). This
// module runs in the agent's process as the agent's own account and never opens the desktop user's
// bus. It listens on the helper socket, and the helper hands it one PipeWire connection per capture
// (and, from slice 3.2, mutter's EIS socket for input). Frames flow from that descriptor into
// module E's stream reader and an appsrc, and on into the media plane like any other video source.
// spec: docs/23-agent-core-architecture.md#desktop-helper-protocol · docs/09 (DesktopBackend)
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <set>
#include <memory>
#include <string>

#include <grp.h>
#include <linux/input-event-codes.h>
#include <pwd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <glib-unix.h>
#include <gst/gst.h>

#include "desktop/clipboard_types.hpp"
#include "desktop/helper_protocol.hpp"
#include "desktop/module.hpp"
#include "desktop/monitor_identity.hpp"
#include "stream_reader.hpp"
#include "eis_input.hpp"

using namespace fjarr;
using nlohmann::json;

namespace {

constexpr const char* COMPONENT = "desktop";

/// The track's source for one monitor: unavailable until the helper hands the stream over. Its frames
/// come from the capture's stream reader through an appsrc the source owns, so a restarted capture's
/// new reader feeds the pipeline already built (docs/23#desktop-helper-protocol).
class MonitorSource final : public VideoSource {
  public:
    explicit MonitorSource(std::string identity) : identity_(std::move(identity)) {}
    ~MonitorSource() override {
        if (appsrc_) gst_object_unref(appsrc_);
    }

    SourceInfo describe() const override { return {identity_, {{"src", TrackKind::Video, "video/x-raw"}}}; }

    /// An appsrc fed by the reader, starting with the last frame; the producer may build the bin
    /// more than once, and the newest appsrc is the one fed.
    GstBin* create_bin() override {
        if (!reader_) return nullptr;
        GstElement* src = gst_element_factory_make("appsrc", nullptr);
        if (!src) return nullptr;
        g_object_set(src, "is-live", TRUE, "format", GST_FORMAT_TIME, "do-timestamp", TRUE, "max-buffers", static_cast<guint64>(4), "leaky-type",
                     2 /* downstream: drop the oldest */, nullptr);
        GstElement* bin = gst_bin_new(nullptr);
        gst_bin_add(GST_BIN(bin), src);
        GstPad* pad = gst_element_get_static_pad(src, "src");
        gst_element_add_pad(bin, gst_ghost_pad_new("src", pad));
        gst_object_unref(pad);
        if (appsrc_) gst_object_unref(appsrc_);
        appsrc_ = GST_ELEMENT(gst_object_ref(src));
        reader_->attach(appsrc_);
        return GST_BIN(bin);
    }

    bool available() const override { return reader_ != nullptr; }
    void on_availability_changed(std::function<void(bool)> cb) override { availability_ = std::move(cb); }
    std::string unavailable_reason() const override { return reader_ ? "" : reason_; }

    /// The helper handed the stream over and the reader is linked. A restarted capture's reader takes
    /// over the appsrc a pipeline already has.
    void ready(std::unique_ptr<desktop::mutter::StreamReader> reader) {
        const bool was = reader_ != nullptr;
        reader_ = std::move(reader);
        reason_.clear();
        if (reader_ && appsrc_) reader_->attach(appsrc_);
        if (availability_ && was != (reader_ != nullptr)) availability_(reader_ != nullptr);
    }
    /// The stream ended (monitor gone, stream stopped, helper gone).
    void lost(const std::string& why) {
        const bool was = reader_ != nullptr;
        reader_.reset();
        reason_ = why;
        if (was && availability_) availability_(false);
    }

  private:
    std::string identity_;
    std::unique_ptr<desktop::mutter::StreamReader> reader_;
    GstElement* appsrc_ = nullptr; // the newest pipeline's source, which every reader of this capture feeds
    std::string reason_ = "waiting for the desktop session to hand the stream over";
    std::function<void(bool)> availability_;
};

class MutterBackend final : public DesktopBackend, public ClipboardHandle {
  public:
    explicit MutterBackend(const desktop::ModuleHost& host, const json& config)
        : ctx_(host.context), log_(host.log), socket_path_(config.value("socket", std::string{"/run/fjarr/desktop.sock"})),
          keepalive_ms_(config.value("keepalive_ms", 100)), input_(host.context, [this](int level, const std::string& msg) { say(level, msg); }) {
        input_.on_disconnected([this] { input_requested_ = false; });
        if (config.contains("uid")) desktop_uid_ = config["uid"].get<int>();
        else if (config.contains("user")) {
            if (const passwd* pw = ::getpwnam(config["user"].get<std::string>().c_str())) desktop_uid_ = static_cast<int>(pw->pw_uid);
        }
        if (config.contains("group")) {
            if (const group* gr = ::getgrnam(config["group"].get<std::string>().c_str())) socket_gid_ = static_cast<int>(gr->gr_gid);
        }
    }

    ~MutterBackend() override {
        drop_helper("the agent is stopping");
        if (listen_watch_) g_source_destroy(listen_watch_), g_source_unref(listen_watch_);
        if (listen_fd_ >= 0) {
            ::close(listen_fd_);
            ::unlink(socket_path_.c_str());
        }
    }

    /// Listen for the helper. False (logged) when the socket cannot be made.
    bool listen() {
        if (desktop_uid_ < 0) return fail("no desktop account configured (capabilities.\"fjarr.desktop\".helper.user or .uid)");
        ::unlink(socket_path_.c_str());
        listen_fd_ = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::snprintf(addr.sun_path, sizeof addr.sun_path, "%s", socket_path_.c_str());
        if (listen_fd_ < 0 || ::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || ::listen(listen_fd_, 2) != 0)
            return fail("cannot listen on " + socket_path_ + ": " + std::strerror(errno));
        // Only the agent and the desktop group may connect; SO_PEERCRED then decides (ADR-0028).
        if (socket_gid_ >= 0 && ::chown(socket_path_.c_str(), static_cast<uid_t>(-1), static_cast<gid_t>(socket_gid_)) != 0)
            say(2, "cannot give " + socket_path_ + " its group: " + std::strerror(errno));
        ::chmod(socket_path_.c_str(), 0660);
        listen_watch_ = g_unix_fd_source_new(listen_fd_, G_IO_IN);
        GUnixFDSourceFunc on_listen = [](gint, GIOCondition, gpointer d) -> gboolean {
            static_cast<MutterBackend*>(d)->on_accept();
            return G_SOURCE_CONTINUE;
        };
        g_source_set_callback(listen_watch_, G_SOURCE_FUNC(on_listen), this, nullptr); // GLib's own cast for fd sources
        g_source_attach(listen_watch_, ctx_);
        say(1, "waiting for the desktop session's helper on " + socket_path_ + " (uid " + std::to_string(desktop_uid_) + ")");
        return true;
    }

    // --- DesktopBackend ----------------------------------------------------------------------
    Features features() override {
        Features f;
        f.clipboard = true;    // through the helper's input session (docs/23#desktop-helper-protocol)
        f.local_cursor = true; // mutter's cursor metadata, through module E's stream reader
        f.virtual_monitors = true; // RecordVirtual, through the helper (docs/23, Virtual monitors)
        return f;
    }
    std::vector<Monitor> monitors() override { return monitors_; }
    void on_monitors_changed(std::function<void(std::vector<Monitor>)> cb) override { monitors_cb_ = std::move(cb); }
    void on_capture_lost(std::function<void(MonitorId, CaptureLost)> cb) override { lost_cb_ = std::move(cb); }

    std::shared_ptr<VideoSource> start_capture(MonitorId monitor, CaptureOptions options) override {
        // A virtual monitor we made is captured by the stream that made it (docs/23, Virtual monitors).
        if (auto v = captures_.find(monitor); v != captures_.end() && v->second.is_virtual) return v->second.source;
        const Monitor* m = find(monitor);
        if (!m) return nullptr;
        auto& c = captures_[monitor];
        if (!c.source) c.source = std::make_shared<MonitorSource>("desktop:" + m->wire_id);
        c.id = next_id_++;
        c.connector = m->connector;
        // A monitor whose metadata capture gave no frame stays embedded (docs/23, The stream reader).
        c.cursor = options.cursor_in_video || embedded_.count(monitor) ? "embedded" : "metadata";
        send_start(monitor, c);
        return c.source;
    }

    void stop_capture(MonitorId monitor) override {
        auto it = captures_.find(monitor);
        if (it == captures_.end()) return;
        if (it->second.is_virtual) return; // its own stream; it goes with destroy_virtual_monitor
        if (conn_ >= 0) send({{"type", "stop-capture"}, {"id", it->second.id}});
        it->second.source->lost("capture stopped");
        captures_.erase(it);
        embedded_.erase(monitor); // a new capture of it tries metadata again
    }

    MonitorId create_virtual_monitor(int width, int height) override {
        if (conn_ < 0 || !welcomed_ || width <= 0 || height <= 0) return INVALID_MONITOR;
        const MonitorId id = next_monitor_++;
        auto& c = captures_[id];
        c.source = std::make_shared<MonitorSource>("desktop:virtual");
        c.id = next_id_++;
        c.cursor = "metadata";
        c.is_virtual = true;
        c.want_width = width;
        c.want_height = height;
        send({{"type", "add-virtual"}, {"id", c.id}, {"width", width}, {"height", height}, {"cursor", c.cursor}});
        say(1, "virtual monitor asked for: " + std::to_string(width) + "x" + std::to_string(height));
        return id;
    }
    void destroy_virtual_monitor(MonitorId monitor) override {
        auto it = captures_.find(monitor);
        if (it == captures_.end() || !it->second.is_virtual) return;
        if (conn_ >= 0) send({{"type", "stop-capture"}, {"id", it->second.id}}); // the session ends, and the monitor with it
        it->second.source->lost("the virtual monitor was removed");
        if (!it->second.connector.empty()) virtual_by_connector_.erase(it->second.connector);
        captures_.erase(it);
    }
    std::shared_ptr<VideoSource> start_audio_capture() override { return nullptr; }
    void stop_audio_capture() override {}
    void on_cursor_shape(std::function<void(const CursorShape&)> cb) override {
        cursor_shape_cb_ = std::move(cb);
        if (cursor_shape_cb_ && have_shape_) cursor_shape_cb_(shape_);
    }
    void on_cursor_position(std::function<void(MonitorId, double, double)> cb) override { cursor_position_cb_ = std::move(cb); }
    // Input through the EIS socket the helper hands over (docs/23#desktop-helper-protocol).
    void pointer_motion(MonitorId monitor, double nx, double ny) override {
        // The pointer's region is the stream's logical rectangle: its position plus the point.
        auto it = captures_.find(monitor);
        const Monitor* m = find(monitor);
        if (it != captures_.end() && it->second.width > 0)
            input_.pointer_absolute(it->second.x + nx * it->second.width, it->second.y + ny * it->second.height);
        else if (m)
            input_.pointer_absolute(m->x + nx * m->width / m->scale, m->y + ny * m->height / m->scale);
    }
    void pointer_button(MouseButton button, bool down) override {
        static constexpr std::uint32_t codes[] = {BTN_LEFT, BTN_MIDDLE, BTN_RIGHT, BTN_SIDE, BTN_EXTRA};
        input_.button(codes[static_cast<int>(button)], down);
    }
    void pointer_wheel(double dx, double dy) override { input_.scroll(dx, dy); }
    void key(LinuxKeycode code, bool down) override { input_.key(code, down); }
    std::vector<std::string> type_text(const std::string& utf8, std::string& error) override { return input_.type_text(utf8, &error); }
    void release_all_input() override { input_.release_all(); }
    ClipboardHandle* clipboard() override { return this; }

    // --- ClipboardHandle (docs/09; docs/23#desktop-helper-protocol, Clipboard) -----------------
    void on_changed(std::function<void(std::vector<std::string>)> cb) override { clipboard_changed_ = std::move(cb); }

    void read(const std::string& type, std::size_t max_bytes, std::function<void(std::optional<std::string>, std::string)> done) override {
        const std::string mime = desktop::clipboard::compositor_type_for(type, robot_types_);
        if (conn_ < 0 || !welcomed_) return done(std::nullopt, "no desktop session");
        if (mime.empty()) return done(std::nullopt, "the robot's clipboard has no " + type);
        const int id = next_clip_id_++;
        auto r = std::make_unique<ClipRead>();
        r->self = this;
        r->id = id;
        r->max = max_bytes;
        r->done = std::move(done);
        // A reader that never closes must not hold the request forever.
        r->timeout = g_timeout_source_new_seconds(5);
        g_source_set_callback(r->timeout, [](gpointer d) -> gboolean {
            auto* cr = static_cast<ClipRead*>(d);
            cr->timeout = nullptr;
            cr->self->finish_read(cr->id, std::nullopt, "the robot's clipboard did not answer within 5 s");
            return G_SOURCE_REMOVE;
        }, r.get(), nullptr);
        g_source_attach(r->timeout, ctx_);
        reads_[id] = std::move(r);
        send({{"type", "clipboard-read"}, {"id", id}, {"type", mime}});
    }

    void write(const std::string& type, std::string bytes, std::function<void(bool, std::string)> done) override {
        if (type != desktop::clipboard::TEXT) return done(false, "only text/plain is supported");
        if (conn_ < 0 || !welcomed_) return done(false, "no desktop session");
        // The bytes travel as a memfd beside the message (one datagram is at most 64 KiB).
        const int fd = ::memfd_create("fjarr-clipboard", MFD_CLOEXEC | MFD_ALLOW_SEALING);
        if (fd < 0) return done(false, std::string("memfd_create: ") + std::strerror(errno));
        std::size_t off = 0;
        while (off < bytes.size()) {
            const ssize_t n = ::write(fd, bytes.data() + off, bytes.size() - off);
            if (n < 0 && errno == EINTR) continue;
            if (n < 0) {
                ::close(fd);
                return done(false, std::string("memfd: ") + std::strerror(errno));
            }
            off += static_cast<std::size_t>(n);
        }
        ::fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL);
        const int id = next_clip_id_++;
        writes_[id] = std::move(done);
        std::string err;
        if (!desktop::proto::send(conn_, {{"type", "clipboard-set"}, {"id", id}, {"types", json::array({desktop::clipboard::TEXT})}}, {fd}, &err)) {
            auto cb = std::move(writes_[id]);
            writes_.erase(id);
            cb(false, "to the helper: " + err);
        }
        ::close(fd);
    }

  private:
    /// A capture that streamed without a frame: a new ScreenCast session for the same monitor, whose
    /// start asks mutter for a frame again (docs/23, The stream reader). At most three times.
    void restart_capture(MonitorId monitor) {
        auto it = captures_.find(monitor);
        if (it == captures_.end()) return;
        Capture& c = it->second;
        if (c.is_virtual) {
            // Restarting would make another monitor; a virtual one draws what its stream asks for.
            say(2, "virtual monitor " + c.connector + " streamed without a frame");
            return;
        }
        if (c.restarts >= 3) {
            say(3, "capture of " + c.connector + " has no frame after " + std::to_string(c.restarts) + " restarts; leaving it");
            return;
        }
        c.restarts++;
        // Embedded from now on: mutter renders that frame itself, where metadata mode copies a painted
        // image the monitor may not have yet (docs/23, The stream reader). Clients are told.
        const bool fell_back = c.cursor != "embedded";
        c.cursor = "embedded";
        embedded_.insert(monitor);
        say(2, "capture of " + c.connector + " restarted (" + std::to_string(c.restarts) + "): it streamed without a frame" +
                   (fell_back ? "; the cursor is drawn into its video from now on" : ""));
        if (conn_ >= 0) send({{"type", "stop-capture"}, {"id", c.id}});
        c.id = next_id_++;
        send_start(monitor, c);
        if (fell_back) {
            for (auto& m : monitors_)
                if (m.id == monitor) m.cursor_in_video = true;
            if (monitors_cb_) monitors_cb_(monitors_);
        }
    }

    std::unique_ptr<desktop::mutter::StreamReader> start_reader(MonitorId monitor, int fd, std::uint32_t node, int width = 0, int height = 0) {
        return desktop::mutter::StreamReader::start(
            ctx_, fd, node, keepalive_ms_,
            [this](const desktop::mutter::CursorImage& img) {
                if (have_shape_ && img.shape_id == shape_.shape_id) return; // another monitor's reader, same shape
                shape_.shape_id = img.shape_id;
                shape_.visible = img.visible;
                shape_.width = img.width;
                shape_.height = img.height;
                shape_.hot_x = img.hot_x;
                shape_.hot_y = img.hot_y;
                shape_.rgba = img.rgba;
                have_shape_ = true;
                if (cursor_shape_cb_) cursor_shape_cb_(shape_);
            },
            [this, monitor](double nx, double ny) {
                if (cursor_position_cb_) cursor_position_cb_(monitor, nx, ny);
            },
            [this](int level, const std::string& msg) { say(level, msg); }, width, height);
    }

    /// One read of the robot's clipboard: the descriptor mutter handed over, drained as it fills.
    struct ClipRead {
        MutterBackend* self = nullptr;
        int id = 0;
        std::size_t max = 0;
        std::string bytes;
        int fd = -1;
        GSource* watch = nullptr;
        GSource* timeout = nullptr;
        std::function<void(std::optional<std::string>, std::string)> done;
        ClipRead() = default;
        ClipRead(const ClipRead&) = delete; // it owns the descriptor and its sources
        ClipRead& operator=(const ClipRead&) = delete;
        ~ClipRead() {
            if (watch) g_source_destroy(watch), g_source_unref(watch);
            if (timeout) g_source_destroy(timeout), g_source_unref(timeout);
            if (fd >= 0) ::close(fd);
        }
    };

    gboolean drain(ClipRead* r, int fd) {
        char buf[65536];
        for (;;) {
            const ssize_t n = ::read(fd, buf, sizeof buf);
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return G_SOURCE_CONTINUE;
            if (n < 0) {
                finish_read(r->id, std::nullopt, std::string("reading the clipboard: ") + std::strerror(errno));
                return G_SOURCE_REMOVE;
            }
            if (n == 0) {
                finish_read(r->id, std::move(r->bytes), {});
                return G_SOURCE_REMOVE;
            }
            r->bytes.append(buf, static_cast<std::size_t>(n));
            if (r->bytes.size() > r->max) {
                finish_read(r->id, std::nullopt, "too-large");
                return G_SOURCE_REMOVE;
            }
        }
    }

    /// Completes a read once: its callback runs, its sources go. Safe from inside its own sources.
    void finish_read(int id, std::optional<std::string> bytes, std::string error) {
        auto it = reads_.find(id);
        if (it == reads_.end()) return;
        auto r = std::move(it->second);
        reads_.erase(it);
        // The source whose callback is running returns REMOVE itself; destroying it here is harmless.
        auto done = std::move(r->done);
        done(std::move(bytes), std::move(error));
    }

    struct Capture {
        int id = 0;
        int x = 0, y = 0, width = 0, height = 0; // the stream's logical rectangle, from capture-started
        std::string connector, cursor;
        std::shared_ptr<MonitorSource> source;
        int restarts = 0; // after a capture that never gave a frame
        bool is_virtual = false;            // made by RecordVirtual for a session (docs/23, Virtual monitors)
        int want_width = 0, want_height = 0; // its size, asked for in the reader's format
    };

    void say(int level, const std::string& msg) {
        if (log_) log_(level, COMPONENT, msg.c_str());
    }
    bool fail(const std::string& msg) {
        say(3, msg);
        return false;
    }
    const Monitor* find(MonitorId id) const {
        for (const auto& m : monitors_)
            if (m.id == id) return &m;
        return nullptr;
    }
    void send(const json& body) {
        std::string err;
        if (conn_ >= 0 && !desktop::proto::send(conn_, body, {}, &err)) say(2, "to the helper: " + err);
    }
    void send_start(MonitorId, const Capture& c) {
        if (!welcomed_) return; // sent again when a helper arrives
        send({{"type", "start-capture"}, {"id", c.id}, {"connector", c.connector}, {"cursor", c.cursor}});
    }

    void on_accept() {
        const int fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (fd < 0) return;
        const auto uid = desktop::proto::peer_uid(fd);
        if (!uid || static_cast<int>(*uid) != desktop_uid_) {
            desktop::proto::send(fd, {{"type", "refuse"}, {"reason", "not the desktop account"}});
            ::close(fd);
            say(2, "refused a helper connection from uid " + (uid ? std::to_string(*uid) : std::string("?")));
            return;
        }
        if (conn_ >= 0) {
            desktop::proto::send(fd, {{"type", "refuse"}, {"reason", "a helper is already connected"}});
            ::close(fd);
            return;
        }
        conn_ = fd;
        conn_watch_ = g_unix_fd_source_new(conn_, static_cast<GIOCondition>(G_IO_IN | G_IO_HUP | G_IO_ERR));
        GUnixFDSourceFunc on_data = [](gint, GIOCondition, gpointer d) -> gboolean { return static_cast<MutterBackend*>(d)->on_readable(); };
        g_source_set_callback(conn_watch_, G_SOURCE_FUNC(on_data), this, nullptr);
        g_source_attach(conn_watch_, ctx_);
    }

    gboolean on_readable() {
        desktop::proto::Message m;
        std::string err;
        switch (desktop::proto::receive(conn_, m, &err)) {
        case desktop::proto::Received::Message: handle(m); return G_SOURCE_CONTINUE;
        case desktop::proto::Received::WouldBlock: return G_SOURCE_CONTINUE;
        case desktop::proto::Received::Invalid: say(2, "dropped a message from the helper: " + err); return G_SOURCE_CONTINUE;
        case desktop::proto::Received::Closed: drop_helper("the desktop session's helper left"); return G_SOURCE_REMOVE;
        case desktop::proto::Received::Error: drop_helper(err); return G_SOURCE_REMOVE;
        }
        return G_SOURCE_CONTINUE;
    }

    void handle(desktop::proto::Message& m) {
        const std::string type = m.type();
        if (type == "hello") {
            if (m.body.value("protocol", std::string{}) != desktop::proto::PROTOCOL) {
                send({{"type", "refuse"}, {"reason", std::string("this agent speaks ") + desktop::proto::PROTOCOL}});
                drop_helper("the helper speaks " + m.body.value("protocol", std::string{"nothing"}));
                return;
            }
            welcomed_ = true;
            send({{"type", "welcome"}, {"protocol", desktop::proto::PROTOCOL}});
            say(1, "the desktop session's helper connected (" + m.body["session"].value("compositor", std::string{"?"}) + ")");
        } else if (!welcomed_) {
            return;
        } else if (type == "monitors") {
            set_monitors(m.body.value("monitors", json::array()));
        } else if (type == "capture-started") {
            const int id = m.body.value("id", 0);
            for (auto& [monitor, c] : captures_)
                if (c.id == id && !m.fds.empty()) {
                    c.x = m.body.value("x", 0);
                    c.y = m.body.value("y", 0);
                    c.width = m.body.value("width", 0);
                    c.height = m.body.value("height", 0);
                    // The node's only consumer, linked now: mutter sends the cursor's shape, and on a
                    // still screen the only frame, to whoever is linked when it is produced (spikes).
                    auto reader = start_reader(monitor, m.fds[0].release(), m.body.value("node", 0u), c.want_width, c.want_height);
                    if (reader) reader->on_stalled([this, monitor] { restart_capture(monitor); });
                    c.source->ready(std::move(reader));
                    request_input();
                    say(1, "capture of " + c.connector + " ready: node " + std::to_string(m.body.value("node", 0u)) + ", " +
                               std::to_string(m.body.value("width", 0)) + "x" + std::to_string(m.body.value("height", 0)));
                }
        } else if (type == "input-opened") {
            input_requested_ = false;
            if (!m.fds.empty()) input_.attach(m.fds[0].release());
        } else if (type == "input-failed") {
            input_requested_ = false;
            say(2, "the helper could not open input: " + m.body.value("reason", std::string{"?"}));
        } else if (type == "clipboard-changed") {
            robot_types_.clear();
            for (const auto& t : m.body.value("types", json::array()))
                if (t.is_string()) robot_types_.push_back(t.get<std::string>());
            if (clipboard_changed_) clipboard_changed_(desktop::clipboard::fjarr_types(robot_types_));
        } else if (type == "clipboard-data") {
            const int id = m.body.value("id", 0);
            auto it = reads_.find(id);
            if (it == reads_.end() || m.fds.empty()) return;
            ClipRead* r = it->second.get();
            r->fd = m.fds[0].release();
            ::fcntl(r->fd, F_SETFL, ::fcntl(r->fd, F_GETFL) | O_NONBLOCK);
            r->watch = g_unix_fd_source_new(r->fd, static_cast<GIOCondition>(G_IO_IN | G_IO_HUP | G_IO_ERR));
            GUnixFDSourceFunc on_data = [](gint fd, GIOCondition, gpointer d) -> gboolean { return static_cast<ClipRead*>(d)->self->drain(static_cast<ClipRead*>(d), fd); };
            g_source_set_callback(r->watch, G_SOURCE_FUNC(on_data), r, nullptr);
            g_source_attach(r->watch, ctx_);
        } else if (type == "clipboard-set-done" || (type == "clipboard-failed" && writes_.count(m.body.value("id", 0)))) {
            auto it = writes_.find(m.body.value("id", 0));
            if (it == writes_.end()) return;
            auto cb = std::move(it->second);
            writes_.erase(it);
            cb(type == "clipboard-set-done", m.body.value("reason", std::string{}));
        } else if (type == "clipboard-failed") {
            finish_read(m.body.value("id", 0), std::nullopt, m.body.value("reason", std::string{"the helper could not read the clipboard"}));
        } else if (type == "virtual-connector") {
            // The connector a virtual monitor got: it keeps the MonitorId it was asked for under.
            const int id = m.body.value("id", 0);
            for (auto& [monitor, c] : captures_)
                if (c.id == id && c.is_virtual) {
                    c.connector = m.body.value("connector", std::string{});
                    virtual_by_connector_[c.connector] = monitor;
                    say(1, "virtual monitor is " + c.connector);
                }
        } else if (type == "capture-failed") {
            say(2, "the helper could not capture: " + m.body.value("reason", std::string{"?"}));
        } else if (type == "capture-lost") {
            const int id = m.body.value("id", 0);
            for (auto& [monitor, c] : captures_)
                if (c.id == id) {
                    c.source->lost("the stream stopped");
                    if (lost_cb_) lost_cb_(monitor, m.body.value("reason", std::string{}) == "monitor-gone" ? CaptureLost::MonitorGone : CaptureLost::SourceStopped);
                }
        }
    }

    /// Input belongs to the remote-desktop session a capture starts, so it is asked for once one has.
    void request_input() {
        if (input_.attached() || input_requested_) return;
        input_requested_ = true;
        send({{"type", "open-input"}});
    }

    void set_monitors(const json& list) {
        std::vector<desktop::MonitorKey> keys;
        for (const auto& j : list)
            keys.push_back({j["identity"].value("vendor", std::string{}), j["identity"].value("product", std::string{}),
                            j["identity"].value("serial", std::string{}), j.value("connector", std::string{}), j.value("kind", std::string{}) == "virtual"});
        const auto ids = desktop::wire_ids(keys);
        std::vector<Monitor> next;
        for (std::size_t i = 0; i < ids.size(); i++) {
            const auto& j = list[i];
            Monitor m;
            m.wire_id = ids[i];
            auto known = ids_by_wire_.find(m.wire_id);
            if (auto v = virtual_by_connector_.find(keys[i].connector); keys[i].is_virtual && v != virtual_by_connector_.end())
                m.id = ids_by_wire_[m.wire_id] = v->second; // one of ours: the id it was asked for under
            else
                m.id = known != ids_by_wire_.end() ? known->second : (ids_by_wire_[m.wire_id] = next_monitor_++);
            m.identity = {keys[i].vendor, keys[i].model, keys[i].serial};
            m.kind = keys[i].is_virtual ? MonitorKind::Virtual : MonitorKind::Physical;
            m.connector = keys[i].connector;
            m.label = j.value("name", std::string{});
            m.primary = j.value("primary", false);
            m.x = j.value("x", 0);
            m.y = j.value("y", 0);
            m.width = j.value("width", 0);
            m.height = j.value("height", 0);
            m.scale = j.value("scale", 1.0);
            m.cursor_in_video = embedded_.count(m.id) > 0;
            next.push_back(m);
        }
        monitors_ = std::move(next);
        say(1, std::to_string(monitors_.size()) + " monitor(s) from the desktop session");
        // Captures asked for before the helper arrived go out now.
        for (auto& [monitor, c] : captures_)
            if (find(monitor) && !c.source->available()) send_start(monitor, c);
        if (monitors_cb_) monitors_cb_(monitors_);
    }

    void drop_helper(const std::string& why) {
        if (conn_ < 0) return;
        if (conn_watch_) g_source_destroy(conn_watch_), g_source_unref(conn_watch_);
        conn_watch_ = nullptr;
        ::close(conn_);
        conn_ = -1;
        welcomed_ = false;
        input_.detach(); // mutter's session went with the helper; it released what was held
        robot_types_.clear();
        for (auto it = reads_.begin(); it != reads_.end();) finish_read((it++)->first, std::nullopt, "the desktop session ended");
        auto writes = std::move(writes_);
        writes_.clear();
        for (auto& [id, cb] : writes) cb(false, "the desktop session ended");
        input_requested_ = false;
        say(1, why);
        for (auto& [monitor, c] : captures_) {
            c.source->lost("the desktop session ended");
            if (lost_cb_) lost_cb_(monitor, CaptureLost::SessionEnded);
        }
        // Virtual monitors die with the helper's ScreenCast sessions: nothing to record again later.
        for (auto it = captures_.begin(); it != captures_.end();) it = it->second.is_virtual ? captures_.erase(it) : std::next(it);
        virtual_by_connector_.clear();
        monitors_.clear();
        if (monitors_cb_) monitors_cb_(monitors_);
    }

    GMainContext* ctx_;
    void (*log_)(int, const char*, const char*);
    std::string socket_path_;
    int keepalive_ms_;
    int desktop_uid_ = -1;
    int socket_gid_ = -1;
    int listen_fd_ = -1;
    GSource* listen_watch_ = nullptr;
    int conn_ = -1;
    GSource* conn_watch_ = nullptr;
    bool welcomed_ = false;
    std::vector<Monitor> monitors_;
    std::map<std::string, MonitorId> ids_by_wire_; // a monitor keeps its handle across replugs
    MonitorId next_monitor_ = 1;
    std::map<MonitorId, Capture> captures_;
    int next_id_ = 1;
    std::function<void(std::vector<Monitor>)> monitors_cb_;
    std::function<void(MonitorId, CaptureLost)> lost_cb_;
    desktop::mutter::EisInput input_;
    bool input_requested_ = false;
    std::set<MonitorId> embedded_; // monitors whose capture fell back to the cursor in the video
    std::map<std::string, MonitorId> virtual_by_connector_; // virtual monitors we made, by the connector they got
    std::function<void(const CursorShape&)> cursor_shape_cb_;
    std::function<void(MonitorId, double, double)> cursor_position_cb_;
    CursorShape shape_; // the latest, for a capability that subscribes later
    bool have_shape_ = false;
    std::vector<std::string> robot_types_; // what the robot's clipboard offers, in the compositor's names
    std::function<void(std::vector<std::string>)> clipboard_changed_;
    std::map<int, std::unique_ptr<ClipRead>> reads_;
    std::map<int, std::function<void(bool, std::string)>> writes_;
    int next_clip_id_ = 1;
};

const char* probe() { return nullptr; } // usable wherever it is installed: the helper says the rest

DesktopBackend* create(const desktop::ModuleHost* host) {
    if (!host || !host->context) return nullptr; // tools without a loop (--check) get no backend
    json config = json::parse(host->config_json ? host->config_json : "{}", nullptr, false);
    if (config.is_discarded() || !config.is_object()) config = json::object();
    auto b = std::make_unique<MutterBackend>(*host, config);
    if (!b->listen()) return nullptr;
    return b.release();
}

const desktop::ModuleV1 MODULE{
    /*display_server=*/"wayland",
    /*package=*/"fjarr-desktop-wayland",
    /*probe=*/&probe,
    /*create=*/&create,
};

} // namespace

extern "C" __attribute__((visibility("default"))) const desktop::ModuleV1* fjarr_desktop_module_v1() { return &MODULE; }
