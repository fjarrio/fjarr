// spec: docs/23-agent-core-architecture.md#desktop-helper-protocol · ADR-0034
#include "desktop/helper_listener.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <glib-unix.h>
#include <grp.h>
#include <pwd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace fjarr::desktop {

struct HelperListener::Impl {
    GMainContext* ctx = nullptr;
    std::string path;
    int uid = -1, gid = -1;
    Log log;
    OnMessage on_message;
    OnPresence on_presence;
    int listen_fd = -1;
    GSource* listen_watch = nullptr;
    int conn = -1;
    GSource* conn_watch = nullptr;
    bool welcomed = false;

    ~Impl() {
        drop("the agent is stopping", /*notify=*/false);
        if (listen_watch) g_source_destroy(listen_watch), g_source_unref(listen_watch);
        if (listen_fd >= 0) {
            ::close(listen_fd);
            ::unlink(path.c_str());
        }
    }

    void raw_send(int fd, const nlohmann::json& body) {
        std::string err;
        if (!proto::send(fd, body, {}, &err)) log(2, "to the helper: " + err);
    }

    void on_accept() {
        const int fd = ::accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (fd < 0) return;
        const auto peer = proto::peer_uid(fd);
        if (!peer || static_cast<int>(*peer) != uid) {
            raw_send(fd, {{"type", "refuse"}, {"reason", "not the desktop account"}});
            ::close(fd);
            log(2, "refused a helper connection from uid " + (peer ? std::to_string(*peer) : std::string("?")));
            return;
        }
        if (conn >= 0) {
            raw_send(fd, {{"type", "refuse"}, {"reason", "a helper is already connected"}});
            ::close(fd);
            return;
        }
        conn = fd;
        conn_watch = g_unix_fd_source_new(conn, static_cast<GIOCondition>(G_IO_IN | G_IO_HUP | G_IO_ERR));
        GUnixFDSourceFunc on_data = [](gint, GIOCondition, gpointer d) -> gboolean { return static_cast<Impl*>(d)->on_readable(); };
        g_source_set_callback(conn_watch, G_SOURCE_FUNC(on_data), this, nullptr);
        g_source_attach(conn_watch, ctx);
    }

    gboolean on_readable() {
        proto::Message m;
        std::string err;
        switch (proto::receive(conn, m, &err)) {
        case proto::Received::Message: handle(m); return conn >= 0 ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
        case proto::Received::WouldBlock: return G_SOURCE_CONTINUE;
        case proto::Received::Invalid: log(2, "dropped a message from the helper: " + err); return G_SOURCE_CONTINUE;
        case proto::Received::Closed: drop("the desktop session's helper left"); return G_SOURCE_REMOVE;
        case proto::Received::Error: drop(err); return G_SOURCE_REMOVE;
        }
        return G_SOURCE_CONTINUE;
    }

    void handle(proto::Message& m) {
        if (m.type() == "hello") {
            if (m.body.value("protocol", std::string{}) != proto::PROTOCOL) {
                raw_send(conn, {{"type", "refuse"}, {"reason", std::string("this agent speaks ") + proto::PROTOCOL}});
                drop("the helper speaks " + m.body.value("protocol", std::string{"nothing"}));
                return;
            }
            welcomed = true;
            raw_send(conn, {{"type", "welcome"}, {"protocol", proto::PROTOCOL}});
            log(1, "the desktop session's helper connected (" + m.body.value("helper", std::string{"?"}) + ")");
            if (on_presence) on_presence(true);
        } else if (welcomed && on_message) {
            on_message(m);
        }
    }

    /// Closes the helper's connection. Its watch is destroyed here, so from inside that watch's own
    /// callback the caller returns G_SOURCE_REMOVE, which GLib tolerates for a destroyed source.
    void drop(const std::string& why, bool notify = true) {
        if (conn < 0) return;
        const bool was = welcomed;
        if (conn_watch) g_source_destroy(conn_watch), g_source_unref(conn_watch);
        conn_watch = nullptr;
        ::close(conn);
        conn = -1;
        welcomed = false;
        log(1, why);
        if (was && notify && on_presence) on_presence(false);
    }
};

HelperListener::HelperListener(GMainContext* ctx, std::string path, int uid, int gid, Log log, OnMessage on_message, OnPresence on_presence)
    : impl_(std::make_unique<Impl>()) {
    impl_->ctx = ctx;
    impl_->path = std::move(path);
    impl_->uid = uid;
    impl_->gid = gid;
    impl_->log = std::move(log);
    impl_->on_message = std::move(on_message);
    impl_->on_presence = std::move(on_presence);
}

HelperListener::~HelperListener() = default;

bool HelperListener::listen() {
    auto& i = *impl_;
    ::unlink(i.path.c_str());
    i.listen_fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof addr.sun_path, "%s", i.path.c_str());
    if (i.listen_fd < 0 || ::bind(i.listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || ::listen(i.listen_fd, 2) != 0) {
        i.log(3, "cannot listen on " + i.path + ": " + std::strerror(errno));
        return false;
    }
    // Only the agent and the desktop group may connect; SO_PEERCRED then decides (ADR-0028).
    if (i.gid >= 0 && ::chown(i.path.c_str(), static_cast<uid_t>(-1), static_cast<gid_t>(i.gid)) != 0)
        i.log(2, "cannot give " + i.path + " its group: " + std::strerror(errno));
    ::chmod(i.path.c_str(), 0660);
    i.listen_watch = g_unix_fd_source_new(i.listen_fd, G_IO_IN);
    GUnixFDSourceFunc on_listen = [](gint, GIOCondition, gpointer d) -> gboolean {
        static_cast<Impl*>(d)->on_accept();
        return G_SOURCE_CONTINUE;
    };
    g_source_set_callback(i.listen_watch, G_SOURCE_FUNC(on_listen), impl_.get(), nullptr);
    g_source_attach(i.listen_watch, i.ctx);
    i.log(1, "waiting for the desktop session's helper on " + i.path + " (uid " + std::to_string(i.uid) + ")");
    return true;
}

void HelperListener::send(const nlohmann::json& body) {
    if (impl_->conn >= 0 && impl_->welcomed) impl_->raw_send(impl_->conn, body);
}

int helper_uid(const nlohmann::json& config) {
    if (config.contains("uid") && config["uid"].is_number_integer()) return config["uid"].get<int>();
    if (config.contains("user") && config["user"].is_string())
        if (const passwd* pw = ::getpwnam(config["user"].get<std::string>().c_str())) return static_cast<int>(pw->pw_uid);
    return -1;
}

int helper_gid(const nlohmann::json& config) {
    if (config.contains("gid") && config["gid"].is_number_integer()) return config["gid"].get<int>();
    if (config.contains("group") && config["group"].is_string())
        if (const group* gr = ::getgrnam(config["group"].get<std::string>().c_str())) return static_cast<int>(gr->gr_gid);
    return -1;
}

} // namespace fjarr::desktop
