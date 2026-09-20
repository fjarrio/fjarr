#include "signaling_client.hpp"

#include <libsoup/soup.h>

#include "log.hpp"

namespace fjarr::core {

struct SignalingClient::Impl {
    glib::GObjectPtr<SoupSession> session;
    glib::GObjectPtr<SoupWebsocketConnection> conn;
    glib::GObjectPtr<GCancellable> cancellable;
    std::vector<glib::SignalConnection> signals;
};

SignalingClient::SignalingClient(CoreLoop& loop, SignalingConfig config, SignalingHooks hooks)
    : loop_(loop), config_(std::move(config)), hooks_(std::move(hooks)), impl_(std::make_unique<Impl>()) {}

SignalingClient::~SignalingClient() { stop(); }

void SignalingClient::start() {
    loop_.assert_owner("SignalingClient::start");
    stopped_ = false;
    if (!impl_->session) impl_->session.reset(soup_session_new()); // uses the thread-default context: the core loop
    connect();
}

void SignalingClient::begin_close(const char* reason) {
    stopped_ = true;
    reconnect_timer_.cancel();
    if (impl_->conn && soup_websocket_connection_get_state(impl_->conn.get()) == SOUP_WEBSOCKET_STATE_OPEN)
        soup_websocket_connection_close(impl_->conn.get(), SOUP_WEBSOCKET_CLOSE_NORMAL, reason);
}

bool SignalingClient::closed() const {
    return !impl_->conn || soup_websocket_connection_get_state(impl_->conn.get()) == SOUP_WEBSOCKET_STATE_CLOSED;
}

void SignalingClient::stop() {
    stopped_ = true;
    reconnect_timer_.cancel();
    if (impl_->cancellable) g_cancellable_cancel(impl_->cancellable.get());
    impl_->signals.clear();
    if (impl_->conn) {
        if (soup_websocket_connection_get_state(impl_->conn.get()) == SOUP_WEBSOCKET_STATE_OPEN)
            soup_websocket_connection_close(impl_->conn.get(), SOUP_WEBSOCKET_CLOSE_NORMAL, "agent-shutdown");
        impl_->conn.reset();
    }
    ready_ = false;
}

void SignalingClient::connect() {
    if (stopped_) return;
    log::info("signaling", "connecting", {{"url", config_.url}});
    glib::GObjectPtr<SoupMessage> msg(soup_message_new(SOUP_METHOD_GET, config_.url.c_str()));
    if (!msg) {
        log::error("signaling", "invalid server url", {{"url", config_.url}});
        schedule_reconnect("bad-url");
        return;
    }
    impl_->cancellable.reset(g_cancellable_new());
    std::weak_ptr<bool> alive = alive_;
    auto* ctx = new std::pair<SignalingClient*, std::weak_ptr<bool>>(this, alive);
    soup_session_websocket_connect_async(
        impl_->session.get(), msg.get(), nullptr, nullptr, G_PRIORITY_DEFAULT, impl_->cancellable.get(),
        [](GObject* source, GAsyncResult* res, gpointer data) {
            std::unique_ptr<std::pair<SignalingClient*, std::weak_ptr<bool>>> ctx(static_cast<std::pair<SignalingClient*, std::weak_ptr<bool>>*>(data));
            GError* err = nullptr;
            glib::GObjectPtr<SoupWebsocketConnection> conn(soup_session_websocket_connect_finish(SOUP_SESSION(source), res, &err));
            glib::GErrorPtr e(err);
            if (ctx->second.expired()) return; // the client is gone: the connection drops with our ref
            SignalingClient* self = ctx->first;
            if (!conn) {
                log::warn("signaling", "connect failed", {{"error", e ? e->message : "?"}});
                self->schedule_reconnect("connect-failed");
                return;
            }
            GObject* raw = G_OBJECT(conn.get());
            self->impl_->conn = std::move(conn);
            self->on_open(raw);
        },
        ctx);
}

void SignalingClient::on_open(GObject* conn) {
    impl_->signals.clear();
    impl_->signals.emplace_back(conn, "message", G_CALLBACK((+[](SoupWebsocketConnection*, gint type, GBytes* data, gpointer user) {
                                    if (type != SOUP_WEBSOCKET_DATA_TEXT) return;
                                    gsize len = 0;
                                    const char* p = static_cast<const char*>(g_bytes_get_data(data, &len));
                                    static_cast<SignalingClient*>(user)->on_text(std::string(p, len));
                                })),
                                this);
    impl_->signals.emplace_back(conn, "closed", G_CALLBACK((+[](SoupWebsocketConnection* c, gpointer user) {
                                    const auto code = soup_websocket_connection_get_close_code(c);
                                    static_cast<SignalingClient*>(user)->on_closed("socket-closed:" + std::to_string(code));
                                })),
                                this);
    impl_->signals.emplace_back(conn, "error", G_CALLBACK((+[](SoupWebsocketConnection*, GError* err, gpointer) {
                                    log::warn("signaling", "socket error", {{"error", err ? err->message : "?"}});
                                })),
                                this);
    soup_websocket_connection_set_max_incoming_payload_size(SOUP_WEBSOCKET_CONNECTION(conn), 4 * 1024 * 1024);
    soup_websocket_connection_set_keepalive_interval(SOUP_WEBSOCKET_CONNECTION(conn), 20);
    nlohmann::json hello = protocol::signaling_base("hello");
    hello["role"] = "agent";
    hello["auth"] = {{"scheme", "dev-token"}, {"robot_id", config_.robot_id}, {"dev_token", config_.dev_token}};
    hello["agent_info"] = {{"fjarr", config_.fjarr_version}, {"os", "linux"}, {"arch",
#if defined(__aarch64__)
                                                                               "arm64"
#else
                                                                               "amd64"
#endif
                                                                               },
                           {"capabilities", config_.capability_names}};
    hello["proto_versions"] = {protocol::PROTO_VERSION};
    const std::string text = hello.dump();
    soup_websocket_connection_send_text(SOUP_WEBSOCKET_CONNECTION(conn), text.c_str());
    log::info("signaling", "hello sent", {{"robot_id", config_.robot_id}});
}

void SignalingClient::on_text(const std::string& text) {
    bool higher = false;
    auto msg = protocol::parse_signaling(text, &higher);
    if (!msg) {
        if (higher) {
            nlohmann::json err = protocol::signaling_base("error");
            err["code"] = "payload-invalid";
            err["message"] = "unsupported protocol major";
            send(err);
        } else log::debug("signaling", "ignored unparseable message");
        return;
    }
    if (msg->type == "hello-ack") {
        ready_ = true;
        backoff_.mark_connected(std::chrono::steady_clock::now());
        log::info("signaling", "hello-ack: online");
        if (hooks_.on_ready) hooks_.on_ready();
        return;
    }
    if (msg->type == "error" && !ready_) {
        const std::string code = msg->body.value("code", ""), message = msg->body.value("message", "");
        log::error("signaling", "hello rejected", {{"code", code}, {"message", message}});
        if (hooks_.on_error) hooks_.on_error(code, message);
        return;
    }
    if (!hooks_.on_message) return;
    try {
        hooks_.on_message(*msg);
    } catch (const std::exception& e) {
        // A malformed-but-parseable message must not unwind through libsoup's signal emission.
        log::error("signaling", "message handler threw", {{"type", msg->type}, {"error", e.what()}});
    }
}

void SignalingClient::on_closed(const std::string& reason) {
    const bool was_ready = ready_;
    ready_ = false;
    impl_->signals.clear();
    impl_->conn.reset();
    log::warn("signaling", "socket closed", {{"reason", reason}});
    if (was_ready && hooks_.on_closed) hooks_.on_closed(reason);
    schedule_reconnect(reason);
}

void SignalingClient::schedule_reconnect(const std::string& why) {
    if (stopped_) return;
    backoff_.mark_disconnected(std::chrono::steady_clock::now());
    const auto delay = backoff_.next();
    log::info("signaling", "reconnect scheduled", {{"why", why}, {"delay_ms", std::to_string(delay.count())}});
    reconnect_timer_ = loop_.add_timeout(delay, [this] {
        connect();
        return false;
    });
}

bool SignalingClient::send(const nlohmann::json& message) {
    loop_.assert_owner("SignalingClient::send");
    if (!impl_->conn || soup_websocket_connection_get_state(impl_->conn.get()) != SOUP_WEBSOCKET_STATE_OPEN) return false;
    const std::string text = message.dump();
    soup_websocket_connection_send_text(impl_->conn.get(), text.c_str());
    return true;
}

} // namespace fjarr::core
