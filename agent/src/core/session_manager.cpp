#include "session_manager.hpp"

#include "log.hpp"

namespace fjarr::core {

SessionManager::SessionManager(SessionDeps deps, std::function<const RegisteredCapability*(const std::string&)> lookup)
    : deps_(std::move(deps)), lookup_(std::move(lookup)) {
    deps_.on_closed = [this](const SessionId& id) {
        // Posted (a session may close from inside its own callback) and guarded: shutdown
        // resets the manager in the same loop turn that closes every session.
        deps_.loop->post_guarded([alive = std::weak_ptr<bool>(alive_)] { return !alive.expired(); }, [this, id] {
            sessions_.erase(id);
            release_lease_if_unheld();
        });
    };
    deps_.on_ping = [this](const SessionId& id) { on_ping(id); };
}

bool SessionManager::lease_allows(const std::string& operator_id) const {
    if (lease_.operator_id.empty() || lease_.operator_id == operator_id) return true;
    // Fail-open: a stale lease (30 s without refresh) clears (docs/10).
    return std::chrono::steady_clock::now() - lease_.refreshed > LEASE_TTL;
}

void SessionManager::on_session_request(const protocol::SignalingMessage& msg) {
    deps_.loop->assert_owner("SessionManager::on_session_request");
    const SessionId id = msg.session_id;
    auto grants = protocol::parse_capabilities(msg.body.value("capabilities", nlohmann::json::array()));
    auto op = protocol::parse_operator(msg.body.value("operator", nlohmann::json()));
    std::optional<protocol::TurnCredentials> turn;
    if (msg.body.contains("turn")) turn = protocol::parse_turn(msg.body["turn"]);
    auto reject = [&](const std::string& reason) {
        nlohmann::json r = protocol::signaling_base("session-reject");
        r["session_id"] = id;
        r["reason"] = reason;
        deps_.send_signal(std::move(r));
        log::warn("sessions", "session rejected", {{"session", log::short_id(id)}, {"reason", reason}});
    };
    if (!grants || !op) {
        reject("payload-invalid");
        return;
    }
    if (sessions_.count(id)) {
        reject("session-unknown");
        return;
    }
    // Capability names re-checked against local config (docs/10).
    std::vector<AttachedCapability> caps;
    bool input_bearing = false;
    for (const auto& g : *grants) {
        const RegisteredCapability* rc = lookup_(g.name);
        if (!rc || !rc->enabled) {
            reject("capability-denied");
            return;
        }
        caps.push_back(AttachedCapability{rc->capability, rc->manifest, g.params});
        input_bearing = input_bearing || rc->manifest.input_bearing;
    }
    bool owner = true;
    if (input_bearing) {
        owner = lease_allows(op->id);
        if (owner) {
            lease_ = Lease{op->id, std::chrono::steady_clock::now()};
        } else {
            log::info("sessions", "input lease held by another operator: read-only", {{"session", log::short_id(id)}, {"owner", lease_.operator_id}});
        }
    }
    auto session = std::make_shared<Session>(deps_, id, *op, *grants, turn, owner);
    sessions_[id] = session;
    log::info("sessions", "session requested", {{"session", log::short_id(id)}, {"operator", op->label}, {"caps", std::to_string(caps.size())}});
    session->attach(std::move(caps));
}

void SessionManager::on_signal(const protocol::SignalingMessage& msg) {
    auto it = sessions_.find(msg.session_id);
    if (it == sessions_.end()) {
        if (msg.type != "peer-gone" && msg.type != "session-close") {
            nlohmann::json e = protocol::signaling_base("error");
            e["code"] = "session-unknown";
            e["message"] = "unknown session";
            e["caused_by"] = msg.event_id;
            deps_.send_signal(std::move(e));
        }
        return;
    }
    it->second->on_signal(msg);
}

void SessionManager::close_all(const std::string& reason, bool retry) {
    auto copy = sessions_;
    for (auto& [_, s] : copy) s->close(reason, retry, reason == "peer-gone");
    // Shutdown, socket loss and a plane rebuild must not wait for a session's flush window.
    for (auto& [_, s] : copy) s->flush_close();
}

void SessionManager::release_lease_if_unheld() {
    // docs/10: the claim ends with the owner's last session; nobody should wait out the 30 s fail-open.
    if (lease_.operator_id.empty()) return;
    for (const auto& [_, s] : sessions_)
        if (s->input_owner() && s->operator_info().id == lease_.operator_id) return;
    log::info("sessions", "input lease released", {{"operator", lease_.operator_id}});
    lease_ = Lease{};
}

std::shared_ptr<Session> SessionManager::get(const SessionId& id) const {
    auto it = sessions_.find(id);
    return it == sessions_.end() ? nullptr : it->second;
}

std::vector<std::shared_ptr<Session>> SessionManager::list() const {
    std::vector<std::shared_ptr<Session>> out;
    for (const auto& [_, s] : sessions_) out.push_back(s);
    return out;
}

void SessionManager::on_ping(const SessionId& id) {
    auto s = get(id);
    if (s && s->input_owner() && lease_.operator_id == s->operator_info().id) lease_.refreshed = std::chrono::steady_clock::now();
}

nlohmann::json SessionManager::stats() const {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& [_, s] : sessions_) {
        nlohmann::json j = s->describe();
        j["stats"] = s->last_stats();
        j["buffered_bytes"] = s->buffered_bytes();
        j["manifest_version"] = s->manifest_version();
        arr.push_back(std::move(j));
    }
    return arr;
}

std::size_t SessionManager::buffered_bytes() const {
    std::size_t n = 0;
    for (const auto& [_, s] : sessions_) n += s->buffered_bytes();
    return n;
}

nlohmann::json SessionManager::describe() const {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& [_, s] : sessions_) arr.push_back(s->describe());
    return nlohmann::json{{"sessions", arr}, {"lease", {{"operator", lease_.operator_id}}}};
}

} // namespace fjarr::core
