// fjarr-desktop-session: the desktop user's side of the handover (ADR-0028). It runs in the user's
// session, talks to mutter over the session bus, and hands the agent descriptors over
// /run/fjarr/desktop.sock — a PipeWire connection per capture, narrowed to that capture's node, and
// mutter's EIS socket for input —
// so frames and input events never pass through it. It connects out to the agent and reconnects.
// spec: docs/23-agent-core-architecture.md#desktop-helper-protocol
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <memory>
#include <string>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <gio/gio.h>
#include <glib-unix.h>

#include "desktop/clipboard_types.hpp"
#include "desktop/helper_protocol.hpp"
#include "mutter.hpp"
#include "pipewire.hpp"

using namespace fjarr::desktop;
using nlohmann::json;

namespace {

void say(const std::string& msg) { std::fprintf(stderr, "fjarr-desktop-session: %s\n", msg.c_str()); }

std::string env_or(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);
    return v && *v ? v : fallback;
}

class Helper {
  public:
    Helper(GMainLoop* loop, GDBusConnection* bus, std::string socket_path) : loop_(loop), bus_(bus), socket_path_(std::move(socket_path)) {}

    void start() { try_connect(); }

  private:
    // --- the agent's socket ------------------------------------------------------------------
    void try_connect() {
        const int fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::snprintf(addr.sun_path, sizeof addr.sun_path, "%s", socket_path_.c_str());
        if (fd < 0 || ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
            if (fd >= 0) ::close(fd);
            if (!waiting_logged_) say("waiting for the agent at " + socket_path_);
            waiting_logged_ = true;
            retry_later();
            return;
        }
        waiting_logged_ = false;
        sock_ = fd;
        watch_ = g_unix_fd_add(sock_, static_cast<GIOCondition>(G_IO_IN | G_IO_HUP | G_IO_ERR), &Helper::on_readable, this);
        send({{"type", "hello"}, {"protocol", proto::PROTOCOL}, {"helper", "fjarr-desktop-session"},
              {"session", {{"type", "wayland"}, {"desktop", env_or("XDG_CURRENT_DESKTOP", "GNOME")}, {"compositor", "mutter"}}}});
        say("connected to the agent; hello sent");
    }

    void retry_later() {
        g_timeout_add_seconds(backoff_s_, [](gpointer d) {
            static_cast<Helper*>(d)->try_connect();
            return G_SOURCE_REMOVE;
        }, this);
        backoff_s_ = std::min(backoff_s_ * 2, 10u);
    }

    void disconnect(const std::string& why) {
        say("agent connection closed: " + why);
        if (watch_) g_source_remove(watch_);
        watch_ = 0;
        if (sock_ >= 0) ::close(sock_);
        sock_ = -1;
        welcomed_ = false;
        captures_.clear();
        clipboard_.reset();
        transfers_.clear();
        if (monitors_sub_) g_dbus_connection_signal_unsubscribe(bus_, monitors_sub_);
        monitors_sub_ = 0;
        input_.reset(); // mutter's sessions stop with them: nothing is captured for an agent that is gone
        backoff_s_ = 1;
        retry_later();
    }

    void send(const json& body, const std::vector<int>& fds = {}) {
        std::string err;
        if (sock_ >= 0 && !proto::send(sock_, body, fds, &err)) say("send " + body.value("type", "?") + ": " + err);
    }

    static gboolean on_readable(gint, GIOCondition, gpointer d) {
        auto* self = static_cast<Helper*>(d);
        proto::Message m;
        std::string err;
        switch (proto::receive(self->sock_, m, &err)) {
        case proto::Received::Message: self->handle(m); return G_SOURCE_CONTINUE;
        case proto::Received::WouldBlock:
        case proto::Received::Invalid: return G_SOURCE_CONTINUE; // dropped; its descriptors closed
        case proto::Received::Closed: self->watch_ = 0; self->disconnect("the agent hung up"); return G_SOURCE_REMOVE;
        case proto::Received::Error: self->watch_ = 0; self->disconnect(err); return G_SOURCE_REMOVE;
        }
        return G_SOURCE_CONTINUE;
    }

    // --- the protocol --------------------------------------------------------------------------
    void handle(const proto::Message& m) {
        const std::string type = m.type();
        if (type == "welcome") {
            welcomed_ = true;
            say("welcomed by the agent");
            send_monitors();
            monitors_sub_ = helper::watch_monitors(bus_, [this] { send_monitors(); });
        } else if (type == "refuse") {
            say("refused by the agent: " + m.body.value("reason", std::string{"no reason given"}));
        } else if (!welcomed_) {
            say("ignored '" + type + "' before welcome");
        } else if (type == "start-capture") {
            start_capture(m.body);
        } else if (type == "add-virtual") {
            start_capture(m.body, /*virtual_monitor=*/true);
        } else if (type == "stop-capture") {
            captures_.erase(m.body.value("id", 0)); // its ScreenCast session stops; no other is touched
        } else if (type == "open-input") {
            open_input();
        } else if (type == "clipboard-read") {
            clipboard_read(m.body);
        } else if (type == "clipboard-set") {
            clipboard_set(m);
        } // unknown types are ignored: fjarr-desktop-1 grows by adding them
    }

    void send_monitors() {
        std::string err;
        const auto monitors = helper::current_monitors(bus_, &err);
        // docs/26#ghost-screens: a ghost is never primary while a real monitor is connected. The
        // change raises MonitorsChanged, and the corrected layout is what gets sent then.
        if (enforce_primary(monitors)) return;
        // A virtual monitor's connector, once the layout shows it (docs/23, Virtual monitors): the
        // monitor appears when the stream's consumer has negotiated its size, after capture-started.
        for (auto& [id, rec] : captures_) {
            if (!rec.is_virtual || !rec.connector.empty()) continue;
            for (const auto& m : monitors) {
                if (!m.is_virtual || rec.before.count(m.connector)) continue;
                const bool claimed = std::any_of(captures_.begin(), captures_.end(), [&](const auto& o) { return o.second.connector == m.connector; });
                if (claimed) continue;
                rec.connector = m.connector;
                send({{"type", "virtual-connector"}, {"id", id}, {"connector", m.connector}});
                say("virtual monitor " + std::to_string(id) + " is " + m.connector + ", " + std::to_string(m.width) + "x" + std::to_string(m.height));
                break;
            }
        }
        // docs/23#desktop-helper-protocol: mutter says nothing when a recorded monitor goes away.
        for (auto it = captures_.begin(); it != captures_.end();) {
            const bool there = std::any_of(monitors.begin(), monitors.end(), [&](const helper::MonitorInfo& m) { return m.connector == it->second.connector; });
            if (there || it->second.connector.empty() || it->second.is_virtual) { // a virtual capture is the monitor
                ++it;
                continue;
            }
            send({{"type", "capture-lost"}, {"id", it->first}, {"reason", "monitor-gone"}});
            say("capture " + std::to_string(it->first) + " of '" + it->second.connector + "' lost: the monitor is gone");
            it = captures_.erase(it);
        }
        json list = json::array();
        for (const auto& m : monitors)
            list.push_back({{"connector", m.connector}, {"name", m.name},
                            {"identity", {{"vendor", m.vendor}, {"product", m.product}, {"serial", m.serial}}},
                            {"x", m.x}, {"y", m.y}, {"width", m.width}, {"height", m.height}, {"scale", m.scale},
                            {"primary", m.primary}, {"kind", m.is_virtual ? "virtual" : "physical"}});
        if (!err.empty()) say("monitors: " + err);
        send({{"type", "monitors"}, {"monitors", list}});
    }

    /// Makes the leftmost real monitor primary when a ghost is (docs/26#ghost-screens). True when it
    /// asked mutter for the change.
    bool enforce_primary(const std::vector<helper::MonitorInfo>& monitors) {
        auto ghost = [](const helper::MonitorInfo& m) { return m.vendor == "FJR"; };
        const auto primary = std::find_if(monitors.begin(), monitors.end(), [](const helper::MonitorInfo& m) { return m.primary; });
        if (primary == monitors.end() || !ghost(*primary)) return false;
        const helper::MonitorInfo* best = nullptr;
        for (const auto& m : monitors)
            if (!ghost(m) && !m.is_virtual && (!best || m.x < best->x || (m.x == best->x && m.y < best->y))) best = &m;
        if (!best) return false; // only ghosts: one of them is primary, as it must be
        std::string err;
        if (!helper::make_primary(bus_, best->connector, &err)) {
            say("could not make " + best->connector + " primary instead of the ghost " + primary->connector + ": " + err);
            return false;
        }
        say(best->connector + " made primary: " + primary->connector + " is a ghost screen");
        return true;
    }

    void start_capture(const json& req, bool virtual_monitor = false) {
        const int id = req.value("id", 0);
        const std::string connector = virtual_monitor ? "" : req.value("connector", std::string{});
        std::set<std::string> before; // the virtual connectors there were, so the new one can be told apart
        if (virtual_monitor) {
            std::string e;
            for (const auto& m : helper::current_monitors(bus_, &e))
                if (m.is_virtual) before.insert(m.connector);
        }
        auto recorded = [this, id, connector](bool ok, helper::StreamInfo info, std::string error) {
            if (!ok) {
                send({{"type", "capture-failed"}, {"id", id}, {"reason", error}});
                say("capture " + std::to_string(id) + " of '" + connector + "' failed: " + error);
                captures_.erase(id);
                return;
            }
            std::string err;
            const int pw = helper::open_narrowed_pipewire(info.node, &err); // the screen and nothing else (docs/23)
            if (pw < 0) {
                send({{"type", "capture-failed"}, {"id", id}, {"reason", err}});
                captures_.erase(id);
                return;
            }
            send({{"type", "capture-started"}, {"id", id}, {"node", info.node}, {"x", info.x}, {"y", info.y},
                  {"width", info.width}, {"height", info.height}},
                 {pw});
            ::close(pw); // the agent holds its own copy now
            say("capture " + std::to_string(id) + " of '" + connector + "' started: node " + std::to_string(info.node) + ", " +
                std::to_string(info.width) + "x" + std::to_string(info.height));
        };
        auto cursor = req.value("cursor", std::string{"embedded"});
        auto cap = virtual_monitor ? helper::Capture::start_virtual(bus_, cursor, recorded) : helper::Capture::start(bus_, connector, cursor, recorded);
        if (!cap) return; // failed, and said so
        cap->on_closed([this, id] {
            // Deferred: this runs inside the capture's own signal handler.
            g_idle_add([](gpointer d) {
                auto* p = static_cast<std::pair<Helper*, int>*>(d);
                auto [self, cid] = *p;
                delete p;
                if (self->captures_.erase(cid)) {
                    self->send({{"type", "capture-lost"}, {"id", cid}, {"reason", "stream-stopped"}});
                    say("capture " + std::to_string(cid) + ": mutter ended it");
                }
                return G_SOURCE_REMOVE;
            }, new std::pair<Helper*, int>(this, id));
        });
        captures_[id] = Recording{connector, std::move(cap), virtual_monitor, std::move(before)};
    }

    helper::InputSession& input() {
        if (!input_) {
            input_ = std::make_unique<helper::InputSession>(bus_);
            input_->on_closed([this] {
                // GNOME's stop button, or mutter ending it otherwise: the session is dead, so the next
                // open-input makes a new one (docs/23#desktop-sharing-stopped). Deferred: this runs
                // inside the session's own signal handler.
                say("mutter closed the input session; the agent asks again");
                g_idle_add([](gpointer d) {
                    auto* self = static_cast<Helper*>(d);
                    self->input_.reset();
                    self->robot_types_.clear();
                    // Only the stop closes it: an unplug ends a capture, never the input (docs/23).
                    self->send({{"type", "sharing-stopped"}});
                    return G_SOURCE_REMOVE;
                }, this);
            });
            // The clipboard (docs/23#desktop-helper-protocol): the robot's copies go to the agent;
            // our own selection's echo does not, and a copy on the robot ends our selection.
            input_->on_selection_owner([this](bool ours, std::vector<std::string> types) {
                say(std::string("clipboard owner: ") + (ours ? "this session" : "the robot (" + std::to_string(types.size()) + " types)"));
                robot_types_ = ours ? std::vector<std::string>{} : types;
                if (ours) return;
                clipboard_.reset();
                send({{"type", "clipboard-changed"}, {"types", types}});
            });
            input_->on_selection_transfer([this](std::string mime, std::uint32_t serial) { answer_transfer(mime, serial); });
        }
        return *input_;
    }

    void open_input() {
        std::string err;
        const int eis = input().connect_eis(&err);
        if (eis < 0) {
            send({{"type", "input-failed"}, {"reason", err}});
            return;
        }
        send({{"type", "input-opened"}}, {eis});
        ::close(eis);
        say("input opened");
        if (!input().enable_clipboard(&err)) say("no clipboard: " + err); // input works without it
    }

    // --- the clipboard -------------------------------------------------------------------------
    void clipboard_read(const json& req) {
        const int id = req.value("id", 0);
        const std::string mime = req.value("mime", std::string{});
        // An application answers only the names it offers: any other reads nothing (docs/23).
        if (std::find(robot_types_.begin(), robot_types_.end(), mime) == robot_types_.end()) {
            say("copy from the robot refused: it offers no '" + mime + "'");
            send({{"type", "clipboard-failed"}, {"id", id}, {"reason", "the robot's clipboard offers no '" + mime + "'"}});
            return;
        }
        std::string err;
        const int fd = input().enable_clipboard(&err) ? input().selection_read(mime, &err) : -1;
        if (fd < 0) {
            send({{"type", "clipboard-failed"}, {"id", id}, {"reason", err}});
            return;
        }
        say("copy from the robot: " + mime);
        send({{"type", "clipboard-data"}, {"id", id}}, {fd}); // the module reads it, at most 1 MiB
        ::close(fd);
    }

    void clipboard_set(const proto::Message& m) {
        const int id = m.body.value("id", 0);
        auto failed = [&](const std::string& why) { send({{"type", "clipboard-failed"}, {"id", id}, {"reason", why}}); };
        if (m.fds.empty()) return failed("no content came with clipboard-set");
        // What it is, in Fjarr's names (docs/23): text is offered under every text name, a PNG as image/png.
        const auto types = m.body.value("types", std::vector<std::string>{clipboard::TEXT});
        const std::string type = types.empty() ? std::string(clipboard::TEXT) : types.front();
        const std::size_t limit = clipboard::max_bytes(type);
        if (limit == 0) return failed("the clipboard takes text/plain or image/png, not " + type);
        // The content is a memfd the agent filled; read it whole, at most the type's limit (docs/08).
        std::string bytes;
        char buf[65536];
        ::lseek(m.fds[0].get(), 0, SEEK_SET);
        for (;;) {
            const ssize_t n = ::read(m.fds[0].get(), buf, sizeof buf);
            if (n < 0 && errno == EINTR) continue;
            if (n < 0) return failed(std::string("reading the content: ") + std::strerror(errno));
            if (n == 0) break;
            bytes.append(buf, static_cast<std::size_t>(n));
            if (bytes.size() > limit) return failed("larger than " + std::to_string(limit) + " bytes");
        }
        std::string err;
        if (!input().set_selection(clipboard::compositor_names(type), &err)) return failed(err);
        say("the agent set the clipboard: " + std::to_string(bytes.size()) + " bytes of " + type);
        clipboard_ = std::make_unique<std::string>(std::move(bytes));
        send({{"type", "clipboard-set-done"}, {"id", id}});
    }

    /// One paste on the robot: write our content to mutter's descriptor as it drains, never blocking
    /// the loop on an application that reads slowly or not at all.
    void answer_transfer(const std::string& mime, std::uint32_t serial) {
        std::string err;
        if (!clipboard_) {
            say("paste of " + mime + " on the robot: nothing to give");
            input().selection_write_done(serial, false);
            return;
        }
        const int fd = input().selection_write(serial, &err);
        if (fd < 0) {
            say("paste of " + mime + " on the robot: " + err);
            input().selection_write_done(serial, false);
            return;
        }
        // Built in place: a temporary Transfer's destructor would close mutter's descriptor before a
        // byte was written (it did: every paste was empty).
        auto t = std::make_unique<Transfer>();
        t->self = this;
        t->fd = fd;
        t->data = *clipboard_;
        t->serial = serial;
        Transfer* raw = t.get();
        raw->watch = g_unix_fd_add(fd, static_cast<GIOCondition>(G_IO_OUT | G_IO_ERR | G_IO_HUP), &Helper::on_writable, raw);
        transfers_[serial] = std::move(t);
    }

    struct Transfer {
        Helper* self = nullptr;
        int fd = -1;
        std::string data;
        std::size_t off = 0;
        std::uint32_t serial = 0;
        guint watch = 0;
        Transfer() = default;
        Transfer(const Transfer&) = delete; // it owns the descriptor and the watch
        Transfer& operator=(const Transfer&) = delete;
        ~Transfer() {
            if (watch) g_source_remove(watch);
            if (fd >= 0) ::close(fd);
        }
    };

    static gboolean on_writable(gint fd, GIOCondition cond, gpointer d) {
        auto* t = static_cast<Transfer*>(d);
        bool ok = true, done = false;
        if (cond & G_IO_OUT) {
            while (t->off < t->data.size()) {
                const ssize_t n = ::write(fd, t->data.data() + t->off, t->data.size() - t->off);
                if (n < 0 && errno == EINTR) continue;
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return G_SOURCE_CONTINUE;
                if (n < 0) {
                    ok = false;
                    break;
                }
                t->off += static_cast<std::size_t>(n);
            }
            done = true;
        } else {
            ok = false; // the reader went away
            done = true;
        }
        if (!done) return G_SOURCE_CONTINUE;
        Helper* self = t->self;
        const std::uint32_t serial = t->serial;
        say("paste " + std::to_string(serial) + " on the robot: " + std::to_string(t->off) + " of " + std::to_string(t->data.size()) + " bytes" + (ok ? "" : ", failed"));
        t->watch = 0; // removed by returning G_SOURCE_REMOVE
        ::close(t->fd);
        t->fd = -1;
        self->input().selection_write_done(serial, ok);
        self->transfers_.erase(serial);
        return G_SOURCE_REMOVE;
    }

    GMainLoop* loop_;
    GDBusConnection* bus_;
    std::string socket_path_;
    int sock_ = -1;
    guint watch_ = 0;
    guint monitors_sub_ = 0;
    unsigned backoff_s_ = 1;
    bool welcomed_ = false;
    bool waiting_logged_ = false;
    struct Recording {
        std::string connector; // a virtual monitor's: empty until the layout shows it
        std::unique_ptr<helper::Capture> capture;
        bool is_virtual = false;
        std::set<std::string> before; // virtual: the virtual connectors there were when it was asked for
    };
    std::unique_ptr<helper::InputSession> input_;
    std::map<int, Recording> captures_; // by the agent's capture id
    std::unique_ptr<std::string> clipboard_; // what the agent set, served to every paste until the robot copies
    std::vector<std::string> robot_types_;   // what the robot's own copy offers; empty while ours or none
    std::map<std::uint32_t, std::unique_ptr<Transfer>> transfers_; // pastes in progress, by mutter's serial
};

} // namespace

int main() {
    GError* e = nullptr;
    GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &e);
    if (!bus) {
        say(std::string("no session bus: ") + (e ? e->message : "?"));
        return 1;
    }
    GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
    g_unix_signal_add(SIGTERM, [](gpointer l) { g_main_loop_quit(static_cast<GMainLoop*>(l)); return G_SOURCE_REMOVE; }, loop);
    g_unix_signal_add(SIGINT, [](gpointer l) { g_main_loop_quit(static_cast<GMainLoop*>(l)); return G_SOURCE_REMOVE; }, loop);
    Helper helper(loop, bus, env_or("FJARR_DESKTOP_SOCK", "/run/fjarr/desktop.sock"));
    helper.start();
    g_main_loop_run(loop);
    say("stopping");
    g_main_loop_unref(loop);
    g_object_unref(bus);
    return 0;
}
