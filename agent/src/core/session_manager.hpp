#pragma once
// SessionManager — session_id → Session, the docs/10 control domains, and
// what happens to every session when the signaling socket goes.
// spec: docs/23-agent-core-architecture.md#object-model · docs/10-security.md#session-ownership
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "control_domains.hpp"
#include "glib/raii.hpp"
#include "session.hpp"

namespace fjarr::core {

struct RegisteredCapability {
    Capability* capability = nullptr;
    CapabilityManifest manifest;
    bool enabled = true;
};

class SessionManager {
  public:
    SessionManager(SessionDeps deps, std::function<const RegisteredCapability*(const std::string&)> lookup);
    void on_session_request(const protocol::SignalingMessage& msg);
    void on_signal(const protocol::SignalingMessage& msg);
    /// Every session ends (socket loss, shutdown).
    /// Close every session now (no flush window): shutdown, socket loss, plane rebuild.
    void close_all(const std::string& reason, bool retry = false);
    std::shared_ptr<Session> get(const SessionId& id) const;
    std::vector<std::shared_ptr<Session>> list() const;
    std::size_t size() const { return sessions_.size(); }
    /// Heartbeat: a ping keeps its operator's control claims alive (docs/10).
    void on_ping(const SessionId& id);
    /// Ends idle desktop and stale claims as of `now`. The loop's timer calls it; tests pass a time.
    void control_tick(ControlDomains::Clock::time_point now);
    /// The claims, for tests and GET /status.
    const ControlDomains& control() const { return control_; }
    nlohmann::json describe() const;
    /// GET /stats: every session's description with its last stats sample.
    nlohmann::json stats() const;
    std::size_t buffered_bytes() const;

  private:
    std::optional<nlohmann::json> control_input(Session& s, const std::string& domain);
    std::optional<std::pair<std::string, std::string>> control_request(Session& s, const std::string& type, const std::string& domain);
    nlohmann::json control_state(const Session& s) const;
    nlohmann::json held_data(const std::string& domain) const;
    /// Run a change of holder: release the old holder's input first (docs/15), then tell every session.
    void apply(const ControlDomains::Change& c);
    /// Claims whose operator has no live session in the domain end now; `keep` (an operator whose
    /// session closed to be retried) keeps them across the gap, bounded by the 30 s fail-open.
    void drop_orphaned_claims(const std::string& keep = "");

    SessionDeps deps_;
    std::function<const RegisteredCapability*(const std::string&)> lookup_;
    std::map<SessionId, std::shared_ptr<Session>> sessions_;
    ControlDomains control_;
    glib::SourceGuard control_timer_;
    bool control_ticking_ = false;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true); // guards posts that outlive this manager
};

} // namespace fjarr::core
