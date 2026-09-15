#pragma once
// The embedding surface: how a robot application hosts Fjarr in-process.
// spec: docs/09-interfaces.md#embedding
//
// M0 STATUS: API skeleton. run() logs and idles; the core (signaling client,
// session lifecycle, FrameHub, DC router — docs/02) lands in M1.
#include <functional>
#include <memory>
#include <string>

#include <fjarr/capability.hpp>

namespace fjarr {

struct AgentConfig {
    /// The CUSTOMER'S canonical robot id — Fjarr never invents identity.
    std::string robot_id;
    /// Per-device credential from enrollment (docs/10#device-identity).
    std::string credential;
    /// wss:// endpoint of fjarr-server / Fjarr Cloud.
    std::string server_url;

    /// Load /etc/fjarr/fjarr.toml-style config (M1).
    static AgentConfig from_file(const std::string& path);
};

struct SessionEvent {
    SessionId id = 0;
    std::string type; // "started" | "ended" | ...
    std::string operator_label;
};

class Agent {
  public:
    explicit Agent(AgentConfig config);
    ~Agent();

    Agent(const Agent&) = delete;
    Agent& operator=(const Agent&) = delete;

    /// Register a capability plugin (docs/05). Must precede run()/start().
    void register_capability(std::unique_ptr<Capability> capability);

    /// Audit/session hook for the embedding application.
    void on_session_event(std::function<void(const SessionEvent&)> callback);

    /// Blocking run (reference daemon style)…
    void run();
    /// …or host-loop integration.
    void start();
    void stop();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fjarr
