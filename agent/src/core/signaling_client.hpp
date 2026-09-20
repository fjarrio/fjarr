#pragma once
// SignalingClient — the libsoup-3 WebSocket to fjarr-server: hello, backoff,
// dispatch; one outbound queue on the core loop.
// spec: docs/23-agent-core-architecture.md#signaling-client · ADR-0017
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "backoff.hpp"
#include "glib/raii.hpp"
#include "loop.hpp"
#include "protocol.hpp"

namespace fjarr::core {

struct SignalingConfig {
    std::string url;
    std::string robot_id;
    std::string dev_token;
    std::vector<std::string> capability_names;
    std::string fjarr_version;
};

struct SignalingHooks {
    std::function<void()> on_ready;                                    // hello-ack received
    std::function<void(const protocol::SignalingMessage&)> on_message; // validated, agent-bound
    std::function<void(const std::string& reason)> on_closed;          // every session ends (docs/23)
    std::function<void(const std::string& code, const std::string& message)> on_error; // server error before ready
};

class SignalingClient {
  public:
    SignalingClient(CoreLoop& loop, SignalingConfig config, SignalingHooks hooks);
    ~SignalingClient();
    void start();
    void stop();
    /// Send the WebSocket close handshake (after any queued frames); stop() finishes it.
    void begin_close(const char* reason);
    /// True once the socket is closed (or was never open).
    bool closed() const;
    bool connected() const { return ready_; }
    /// Send a message (adds nothing); false when not connected.
    bool send(const nlohmann::json& message);

  private:
    struct Impl;
    void connect();
    void schedule_reconnect(const std::string& why);
    void on_open(GObject* conn);
    void on_text(const std::string& text);
    void on_closed(const std::string& reason);

    CoreLoop& loop_;
    SignalingConfig config_;
    SignalingHooks hooks_;
    std::unique_ptr<Impl> impl_;
    Backoff backoff_;
    glib::SourceGuard reconnect_timer_;
    bool ready_ = false;
    bool stopped_ = true;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

} // namespace fjarr::core
