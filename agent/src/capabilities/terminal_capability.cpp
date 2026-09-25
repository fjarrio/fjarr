// spec: docs/06-capabilities.md · docs/08-protocol.md#terminal · docs/10-security.md#terminal
#include <fjarr/terminal_capability.hpp>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include <pty.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <fjarr/errors.hpp>
#include <fjarr/session_context.hpp>
#include <fjarr/version.hpp>

#include "core/log.hpp"

namespace fjarr {
namespace {

constexpr std::size_t READ_CHUNK = 32 * 1024; // well under the SCTP max message (docs/08)
constexpr std::size_t MAX_PENDING = 4 * 1024 * 1024;

/// The account the agent is actually running as.
std::string current_user() {
    const struct passwd* pw = ::getpwuid(::geteuid());
    return pw && pw->pw_name ? pw->pw_name : std::to_string(::geteuid());
}

std::string login_shell_of(const std::string& user) {
    const struct passwd* pw = ::getpwnam(user.c_str());
    return pw && pw->pw_shell && *pw->pw_shell ? pw->pw_shell : "/bin/sh";
}

struct Pty {
    pid_t pid = -1;
    int fd = -1;
    std::unique_ptr<FdWatch> watch;
    std::deque<std::string> pending; // output the channel was too full to take
    std::size_t pending_bytes = 0;
    bool drain_armed = false;
};

} // namespace

struct TerminalCapability::Impl {
    std::string user;  // required; empty = not configured, so the capability is `unavailable`
    std::string shell; // empty = the account's login shell
    bool enabled = false;
    std::map<SessionId, SessionContext*> sessions;
    std::map<SessionId, Pty> ptys;

    /// Why a pty cannot be started here, or empty when it can. Availability is state, not an
    /// exception: a robot without a terminal configured is a deployment choice, not a fault.
    std::string unavailable_reason() const {
        if (!enabled || user.empty()) return "no terminal is configured on this robot (capabilities.\"fjarr.terminal\".user)";
        const std::string running_as = current_user();
        if (user != running_as)
            return "configured for user '" + user + "' but the agent runs as '" + running_as +
                   "'; it cannot switch accounts (docs/10#terminal)";
        return {};
    }

    void pump(SessionContext& ctx, Pty& p);
    void close_pty(const SessionId& id, bool notify);
};

void TerminalCapability::Impl::pump(SessionContext& ctx, Pty& p) {
    while (!p.pending.empty()) {
        const std::string& front = p.pending.front();
        const std::span<const std::byte> view(reinterpret_cast<const std::byte*>(front.data()), front.size());
        if (!ctx.bulk().send_binary(view)) {
            if (!p.drain_armed) {
                p.drain_armed = true;
                // The session owns the sender; by `open` time the channel exists, so this registers.
                ctx.bulk().on_drain([this, id = ctx.id(), ctxp = &ctx] {
                    auto it = ptys.find(id);
                    if (it == ptys.end()) return;
                    it->second.drain_armed = false;
                    pump(*ctxp, it->second);
                });
            }
            return;
        }
        p.pending_bytes -= front.size();
        p.pending.pop_front();
    }
}

void TerminalCapability::Impl::close_pty(const SessionId& id, bool notify) {
    auto it = ptys.find(id);
    if (it == ptys.end()) return;
    Pty p = std::move(it->second);
    ptys.erase(it);
    p.watch.reset(); // stop watching before the fd goes
    if (p.pid > 0) {
        ::kill(p.pid, SIGHUP);
        int status = 0;
        // The shell gets a moment to die on SIGHUP before SIGKILL: no orphan shells is a safety
        // behaviour (docs/15), so this does not return until the child is reaped.
        for (int i = 0; i < 20 && ::waitpid(p.pid, &status, WNOHANG) == 0; i++) ::usleep(10 * 1000);
        if (::waitpid(p.pid, &status, WNOHANG) == 0) {
            ::kill(p.pid, SIGKILL);
            ::waitpid(p.pid, &status, 0);
        }
    }
    if (p.fd >= 0) ::close(p.fd);
    if (notify) {
        auto s = sessions.find(id);
        if (s != sessions.end() && s->second) s->second->event("exit", nlohmann::json{{"code", 0}});
    }
}

TerminalCapability::TerminalCapability() : impl_(std::make_unique<Impl>()) {}
TerminalCapability::~TerminalCapability() = default;

CapabilityManifest TerminalCapability::manifest() const {
    CapabilityManifest m;
    m.name = "fjarr.terminal";
    m.version = {0, 1, 0};
    m.channels = {{ChannelClass::Control}, {ChannelClass::Bulk, BulkFraming::Raw}};
    m.consumers.peer = true;
    m.input_bearing = true; // docs/10: the ownership lease, and release_all_input first on detach
    m.config_schema = nlohmann::json{
        {"type", "object"},
        {"additionalProperties", false},
        {"properties",
         {{"enabled", {{"type", "boolean"}}},
          {"user", {{"type", "string"}, {"description", "the account the shell runs as; required, no default (docs/10#terminal)"}}},
          {"shell", {{"type", "string"}, {"description", "default: that account's login shell"}}}}}};
    return m;
}

void TerminalCapability::configure(const nlohmann::json& validated_config, const SourceFactory&) {
    impl_->enabled = validated_config.value("enabled", true);
    impl_->user = validated_config.value("user", std::string{});
    impl_->shell = validated_config.value("shell", std::string{});
    if (impl_->enabled && impl_->user.empty())
        log::warn("terminal", "enabled without a user: no pty will start",
                  {{"hint", "set capabilities.\"fjarr.terminal\".user — there is deliberately no default (docs/10)"}});
}

void TerminalCapability::session_attached(SessionContext& ctx, const nlohmann::json&) { impl_->sessions[ctx.id()] = &ctx; }

void TerminalCapability::session_detached(const SessionId& id, DetachReason, std::string_view) {
    impl_->close_pty(id, /*notify=*/false);
    impl_->sessions.erase(id);
}

void TerminalCapability::release_all_input(const SessionId& id) { impl_->close_pty(id, /*notify=*/false); }

void TerminalCapability::on_binary(SessionContext& ctx, std::span<const std::byte> bytes) {
    auto it = impl_->ptys.find(ctx.id());
    if (it == impl_->ptys.end() || it->second.fd < 0) return; // no shell on this session: drop
    const char* p = reinterpret_cast<const char*>(bytes.data());
    std::size_t left = bytes.size();
    while (left > 0) {
        const ssize_t n = ::write(it->second.fd, p, left);
        if (n > 0) {
            p += n;
            left -= static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        break; // EAGAIN on a full pty buffer, or the shell is gone: keystrokes are droppable
    }
}

void TerminalCapability::on_message(SessionContext& ctx, const Envelope& msg) {
    if (msg.kind != "request") return;
    const auto& p = msg.payload;

    if (msg.type == "open") {
        if (impl_->ptys.count(ctx.id())) {
            ctx.fail(msg, error_codes::busy, "a terminal is already open on this session");
            return;
        }
        const std::string why = impl_->unavailable_reason();
        if (!why.empty()) {
            ctx.fail(msg, error_codes::unavailable, why);
            return;
        }
        const auto cols = static_cast<unsigned short>(p.value("cols", 80));
        const auto rows = static_cast<unsigned short>(p.value("rows", 24));
        const std::string term = p.value("term", std::string{"xterm-256color"});
        const std::string shell = impl_->shell.empty() ? login_shell_of(impl_->user) : impl_->shell;

        struct winsize ws {};
        ws.ws_col = cols ? cols : 80;
        ws.ws_row = rows ? rows : 24;
        int master = -1;
        const pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
        if (pid < 0) {
            ctx.fail(msg, error_codes::internal, std::string("forkpty failed: ") + std::strerror(errno));
            return;
        }
        if (pid == 0) {
            ::setenv("TERM", term.c_str(), 1);
            const std::size_t slash = shell.find_last_of('/');
            const std::string base = slash == std::string::npos ? shell : shell.substr(slash + 1);
            const std::string argv0 = "-" + base; // a login shell, as a real console would give
            ::execl(shell.c_str(), argv0.c_str(), static_cast<char*>(nullptr));
            ::_exit(127);
        }

        Pty pty;
        pty.pid = pid;
        pty.fd = master;
        auto& stored = impl_->ptys[ctx.id()];
        stored = std::move(pty);
        const SessionId sid = ctx.id();
        stored.watch = ctx.watch_readable(master, [this, sid, ctxp = &ctx]() -> bool {
            auto it = impl_->ptys.find(sid);
            if (it == impl_->ptys.end()) return false;
            Pty& q = it->second;
            std::string buf(READ_CHUNK, '\0');
            const ssize_t n = ::read(q.fd, buf.data(), buf.size());
            if (n > 0) {
                buf.resize(static_cast<std::size_t>(n));
                if (q.pending_bytes + buf.size() > MAX_PENDING) {
                    // A peer that stopped reading must not grow the robot's memory without bound.
                    log::warn("terminal", "output backlog full, closing", {{"session", short_session_id(sid)}});
                    impl_->close_pty(sid, /*notify=*/true);
                    return false;
                }
                q.pending_bytes += buf.size();
                q.pending.push_back(std::move(buf));
                impl_->pump(*ctxp, q);
                return true;
            }
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) return true;
            // EOF or EIO: the shell ended. Reap it and say how, then take the pty down.
            int status = 0;
            nlohmann::json payload = nlohmann::json::object();
            if (q.pid > 0 && ::waitpid(q.pid, &status, WNOHANG) == q.pid) {
                if (WIFEXITED(status)) payload["code"] = WEXITSTATUS(status);
                else if (WIFSIGNALED(status)) payload["signal"] = ::strsignal(WTERMSIG(status));
                q.pid = -1;
            } else {
                payload["code"] = 0;
            }
            log::info("terminal", "shell ended", {{"session", short_session_id(sid)}, {"operator", ctxp->operator_info().id}});
            ctxp->event("exit", payload);
            impl_->close_pty(sid, /*notify=*/false);
            return false;
        });
        log::info("terminal", "pty opened",
                  {{"session", short_session_id(ctx.id())}, {"operator", ctx.operator_info().id}, {"user", impl_->user}, {"shell", shell}});
        ctx.result(msg, nlohmann::json{{"ok", true}});
        return;
    }

    if (msg.type == "resize") {
        auto it = impl_->ptys.find(ctx.id());
        if (it == impl_->ptys.end()) {
            ctx.fail(msg, error_codes::payload_invalid, "no terminal is open on this session");
            return;
        }
        struct winsize ws {};
        ws.ws_col = static_cast<unsigned short>(p.value("cols", 80));
        ws.ws_row = static_cast<unsigned short>(p.value("rows", 24));
        ::ioctl(it->second.fd, TIOCSWINSZ, &ws); // the kernel sends SIGWINCH for us
        ctx.result(msg, nlohmann::json{{"ok", true}});
        return;
    }

    if (msg.type == "close") {
        log::info("terminal", "pty closed", {{"session", short_session_id(ctx.id())}, {"operator", ctx.operator_info().id}});
        impl_->close_pty(ctx.id(), /*notify=*/false);
        ctx.result(msg, nlohmann::json{{"ok", true}}); // idempotent: closing a closed terminal succeeded
        return;
    }

    throw FjarrError(std::string(error_codes::payload_invalid), "unknown fjarr.terminal request: " + msg.type);
}

void TerminalCapability::shutdown() {
    while (!impl_->ptys.empty()) impl_->close_pty(impl_->ptys.begin()->first, /*notify=*/false);
    impl_->sessions.clear();
}

} // namespace fjarr
