#pragma once
// SessionManager — session_id → Session, the docs/10 ownership leases, and
// what happens to every session when the signaling socket goes.
// spec: docs/23-agent-core-architecture.md#object-model · docs/10-security.md#session-ownership
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

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
    /// Lease refresh from an input-owning session's ping (docs/10).
    void on_ping(const SessionId& id);
    nlohmann::json describe() const;

  private:
    struct Lease {
        std::string operator_id;
        std::chrono::steady_clock::time_point refreshed{};
    };
    bool lease_allows(const std::string& operator_id) const;

    SessionDeps deps_;
    std::function<const RegisteredCapability*(const std::string&)> lookup_;
    std::map<SessionId, std::shared_ptr<Session>> sessions_;
    Lease lease_;
    void release_lease_if_unheld();
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true); // guards posts that outlive this manager
    static constexpr std::chrono::seconds LEASE_TTL{30};
};

} // namespace fjarr::core
