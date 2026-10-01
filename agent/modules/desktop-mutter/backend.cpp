// Backend E (ADR-0006): GNOME's mutter, reached through fjarr-desktop-session (ADR-0028). This
// module runs in the agent's process as the agent's own account and never opens the desktop user's
// bus. It listens on the helper socket, and the helper hands it one PipeWire connection per capture
// (and, from slice 3.2, mutter's EIS socket for input). Frames flow from that descriptor into
// `pipewiresrc` and on into the media plane like any other video source.
// spec: docs/23-agent-core-architecture.md#desktop-helper-protocol · docs/09 (DesktopBackend)
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <memory>
#include <string>

#include <grp.h>
#include <linux/input-event-codes.h>
#include <pwd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <glib-unix.h>
#include <gst/gst.h>

#include "desktop/helper_protocol.hpp"
#include "desktop/module.hpp"
#include "desktop/monitor_identity.hpp"
#include "eis_input.hpp"

using namespace fjarr;
using nlohmann::json;

namespace {

constexpr const char* COMPONENT = "desktop";

/// The track's source for one monitor: unavailable until the helper hands the stream over.
class MonitorSource final : public VideoSource {
  public:
    MonitorSource(std::string identity, int keepalive_ms) : identity_(std::move(identity)), keepalive_ms_(keepalive_ms) {}
    ~MonitorSource() override {
        if (fd_ >= 0) ::close(fd_);
    }

    SourceInfo describe() const override { return {identity_, {{"src", TrackKind::Video, "video/x-raw"}}}; }

    /// `pipewiresrc` on a duplicate of the handed connection: PipeWire takes ownership of the
    /// descriptor it is given, and the producer may build the bin more than once.
    GstBin* create_bin() override {
        if (fd_ < 0) return nullptr;
        const int fd = ::fcntl(fd_, F_DUPFD_CLOEXEC, 3);
        if (fd < 0) return nullptr;
        // always-copy: the DMA-BUF path failed on the spike machine, so capture copies (ADR-0006).
        // keepalive-time: mutter's stream is damage-driven and silent on a still screen; the last
        // frame is re-sent so the encoder keeps producing (docs/23#desktop-helper-protocol).
        const std::string desc = "pipewiresrc fd=" + std::to_string(fd) + " path=" + std::to_string(node_) +
                                 " keepalive-time=" + std::to_string(keepalive_ms_) + " always-copy=true do-timestamp=true";
        GError* e = nullptr;
        GstElement* bin = gst_parse_bin_from_description(desc.c_str(), TRUE, &e);
        if (!bin) {
            if (e) g_error_free(e);
            ::close(fd);
            return nullptr;
        }
        return GST_BIN(bin);
    }

    bool available() const override { return fd_ >= 0; }
    void on_availability_changed(std::function<void(bool)> cb) override { availability_ = std::move(cb); }
    std::string unavailable_reason() const override { return fd_ >= 0 ? "" : reason_; }

    /// The helper handed the stream over.
    void ready(int fd, std::uint32_t node) {
        if (fd_ >= 0) ::close(fd_);
        fd_ = fd;
        node_ = node;
        reason_.clear();
        if (availability_) availability_(true);
    }
    /// The stream ended (monitor gone, stream stopped, helper gone).
    void lost(const std::string& why) {
        const bool was = fd_ >= 0;
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
        reason_ = why;
        if (was && availability_) availability_(false);
    }

  private:
    std::string identity_;
    int keepalive_ms_;
    int fd_ = -1;
    std::uint32_t node_ = 0;
    std::string reason_ = "waiting for the desktop session to hand the stream over";
    std::function<void(bool)> availability_;
};

class MutterBackend final : public DesktopBackend {
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
    Features features() override { return {}; } // cursor, virtual monitors, audio, clipboard: later slices
    std::vector<Monitor> monitors() override { return monitors_; }
    void on_monitors_changed(std::function<void(std::vector<Monitor>)> cb) override { monitors_cb_ = std::move(cb); }
    void on_capture_lost(std::function<void(MonitorId, CaptureLost)> cb) override { lost_cb_ = std::move(cb); }

    std::shared_ptr<VideoSource> start_capture(MonitorId monitor, CaptureOptions options) override {
        const Monitor* m = find(monitor);
        if (!m) return nullptr;
        auto& c = captures_[monitor];
        if (!c.source) c.source = std::make_shared<MonitorSource>("desktop:" + m->wire_id, keepalive_ms_);
        c.id = next_id_++;
        c.connector = m->connector;
        c.cursor = options.cursor_in_video ? "embedded" : "metadata";
        send_start(monitor, c);
        return c.source;
    }

    void stop_capture(MonitorId monitor) override {
        auto it = captures_.find(monitor);
        if (it == captures_.end()) return;
        if (conn_ >= 0) send({{"type", "stop-capture"}, {"id", it->second.id}});
        it->second.source->lost("capture stopped");
        captures_.erase(it);
    }

    MonitorId create_virtual_monitor(int, int) override { return INVALID_MONITOR; }
    void destroy_virtual_monitor(MonitorId) override {}
    std::shared_ptr<VideoSource> start_audio_capture() override { return nullptr; }
    void stop_audio_capture() override {}
    void on_cursor_shape(std::function<void(const CursorShape&)>) override {}
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
    ClipboardHandle* clipboard() override { return nullptr; }

  private:
    struct Capture {
        int id = 0;
        int x = 0, y = 0, width = 0, height = 0; // the stream's logical rectangle, from capture-started
        std::string connector, cursor;
        std::shared_ptr<MonitorSource> source;
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
                    c.source->ready(m.fds[0].release(), m.body.value("node", 0u));
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
        input_requested_ = false;
        say(1, why);
        for (auto& [monitor, c] : captures_) {
            c.source->lost("the desktop session ended");
            if (lost_cb_) lost_cb_(monitor, CaptureLost::SessionEnded);
        }
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
