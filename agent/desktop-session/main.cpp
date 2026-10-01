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
#include <memory>
#include <string>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <gio/gio.h>
#include <glib-unix.h>

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
        } else if (type == "stop-capture") {
            captures_.erase(m.body.value("id", 0)); // its ScreenCast session stops; no other is touched
        } else if (type == "open-input") {
            open_input();
        } // unknown types are ignored: fjarr-desktop-1 grows by adding them
    }

    void send_monitors() {
        std::string err;
        const auto monitors = helper::current_monitors(bus_, &err);
        // docs/26#ghost-screens: a ghost is never primary while a real monitor is connected. The
        // change raises MonitorsChanged, and the corrected layout is what gets sent then.
        if (enforce_primary(monitors)) return;
        // docs/23#desktop-helper-protocol: mutter says nothing when a recorded monitor goes away.
        for (auto it = captures_.begin(); it != captures_.end();) {
            const bool there = std::any_of(monitors.begin(), monitors.end(), [&](const helper::MonitorInfo& m) { return m.connector == it->second.connector; });
            if (there || it->second.connector.empty()) {
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

    void start_capture(const json& req) {
        const int id = req.value("id", 0);
        const std::string connector = req.value("connector", std::string{});
        auto cap = helper::Capture::start(bus_, connector, req.value("cursor", std::string{"embedded"}), [this, id, connector](bool ok, helper::StreamInfo info, std::string error) {
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
        });
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
        captures_[id] = Recording{connector, std::move(cap)};
    }

    void open_input() {
        if (!input_) {
            input_ = std::make_unique<helper::InputSession>(bus_);
            input_->on_closed([this] { say("mutter closed the input session; the agent asks again"); });
        }
        std::string err;
        const int eis = input_->connect_eis(&err);
        if (eis < 0) {
            send({{"type", "input-failed"}, {"reason", err}});
            return;
        }
        send({{"type", "input-opened"}}, {eis});
        ::close(eis);
        say("input opened");
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
        std::string connector;
        std::unique_ptr<helper::Capture> capture;
    };
    std::unique_ptr<helper::InputSession> input_;
    std::map<int, Recording> captures_; // by the agent's capture id
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
