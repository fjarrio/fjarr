#include "session_manager.hpp"

#include <fjarr/errors.hpp>

#include "log.hpp"

namespace fjarr::core {

SessionManager::SessionManager(SessionDeps deps, std::function<const RegisteredCapability*(const std::string&)> lookup)
    : deps_(std::move(deps)), lookup_(std::move(lookup)) {
    deps_.on_closed = [this](const SessionId& id) {
        // Posted (a session may close from inside its own callback) and guarded: shutdown
        // resets the manager in the same loop turn that closes every session.
        deps_.loop->post_guarded([alive = std::weak_ptr<bool>(alive_)] { return !alive.expired(); }, [this, id] {
            auto it = sessions_.find(id);
            // docs/23: a media or ICE restart is a new session; the claim carries across the gap.
            const std::string keep = it != sessions_.end() && it->second->closing_retry() ? it->second->operator_info().id : "";
            sessions_.erase(id);
            drop_orphaned_claims(keep);
        });
    };
    deps_.on_ping = [this](const SessionId& id) { on_ping(id); };
    deps_.control_input = [this](Session& s, const std::string& domain) { return control_input(s, domain); };
    deps_.control_request = [this](Session& s, const std::string& type, const std::string& domain) { return control_request(s, type, domain); };
    deps_.control_state = [this](const Session& s) { return control_state(s); };
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
    for (const auto& g : *grants) {
        const RegisteredCapability* rc = lookup_(g.name);
        if (!rc || !rc->enabled) {
            reject("capability-denied");
            return;
        }
        caps.push_back(AttachedCapability{rc->capability, rc->manifest, g.params});
    }
    // docs/10: opening a session claims nothing; its first input in a domain does.
    auto session = std::make_shared<Session>(deps_, id, *op, *grants, turn);
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

// ---------------------------------------------------------------- control domains
// spec: docs/10-security.md#session-ownership · docs/08-protocol.md#fjarr-core · docs/23 (control domains)

std::optional<nlohmann::json> SessionManager::control_input(Session& s, const std::string& domain) {
    const auto now = ControlDomains::Clock::now();
    control_tick(now); // an idle desktop is free to whoever types next, timer or not
    auto r = control_.input(domain, s.operator_info(), now, protocol::now_ms());
    if (r.outcome == ControlDomains::Outcome::Held) return held_data(domain);
    if (r.change) apply(*r.change); // before this input reaches the capability
    return std::nullopt;
}

std::optional<std::pair<std::string, std::string>> SessionManager::control_request(Session& s, const std::string& type,
                                                                                   const std::string& domain) {
    const auto& op = s.operator_info();
    if (type == "release-control") {
        if (auto c = control_.release(domain, op.id)) apply(*c);
        return std::nullopt; // idempotent
    }
    if (!s.domains().count(domain)) return std::pair{std::string(error_codes::capability_denied), "no capability in the " + domain + " domain is granted"};
    if (!s.can_claim(domain)) return std::pair{std::string(error_codes::capability_denied), "this grant is view-only"};
    const auto now = ControlDomains::Clock::now();
    control_tick(now);
    // The previous holder's input is released inside apply(), so a motion takeover has stopped the
    // robot before the new holder hears ok (docs/08#fjarr-core).
    if (auto c = control_.take(domain, op, now, protocol::now_ms())) apply(*c);
    return std::nullopt;
}

nlohmann::json SessionManager::held_data(const std::string& domain) const {
    const auto* h = control_.holder(domain);
    if (!h) return nlohmann::json{{"domain", domain}, {"holder", nullptr}};
    return nlohmann::json{{"domain", domain}, {"holder", {{"id", h->op.id}, {"label", h->op.label}}}, {"since", h->since_ms}};
}

nlohmann::json SessionManager::control_state(const Session& s) const {
    nlohmann::json domains = nlohmann::json::object();
    for (const auto& d : s.domains()) {
        const auto* h = control_.holder(d);
        nlohmann::json e{{"holder", nullptr}, {"you", false}, {"view_only", !s.can_claim(d)}};
        if (h) {
            e["holder"] = {{"id", h->op.id}, {"label", h->op.label}};
            e["since"] = h->since_ms;
            e["you"] = h->op.id == s.operator_info().id; // a claim is the operator's, not the session's
        }
        domains[d] = std::move(e);
    }
    return nlohmann::json{{"domains", std::move(domains)}};
}

void SessionManager::apply(const ControlDomains::Change& c) {
    // Safety first (docs/15): nobody inherits a robot in motion or a key held down.
    if (c.release) {
        for (auto& [_, s] : sessions_)
            if (s->operator_info().id == *c.release && s->domains().count(c.domain)) s->release_domain_input(c.domain);
    }
    const auto* h = control_.holder(c.domain);
    log::info("control", h ? "domain claimed" : "domain free",
              {{"domain", c.domain}, {"why", c.why}, {"holder", h ? h->op.id : ""}, {"released", c.release.value_or("")}});
    for (auto& [_, s] : sessions_)
        if (s->domains().count(c.domain)) s->send_control_state();
    if (control_.any_held() && !control_ticking_ && deps_.loop) {
        control_ticking_ = true;
        control_timer_ = deps_.loop->add_timeout(std::chrono::milliseconds(250), [this] {
            control_tick(ControlDomains::Clock::now());
            control_ticking_ = control_.any_held();
            return control_ticking_;
        });
    }
}

void SessionManager::control_tick(ControlDomains::Clock::time_point now) {
    for (const auto& c : control_.expire(now)) {
        if (c.why == "stale") log::warn("control", "claim failed open: no heartbeat for 30 s", {{"domain", c.domain}, {"operator", c.release.value_or("")}});
        apply(c);
    }
}

void SessionManager::drop_orphaned_claims(const std::string& keep) {
    // docs/10: a claim ends with its operator's last session in the domain; nobody waits out the 30 s.
    for (const std::string domain : {"desktop", "motion"}) {
        const auto* h = control_.holder(domain);
        if (!h) continue;
        const std::string holder = h->op.id;
        bool live = holder == keep;
        for (const auto& [_, s] : sessions_)
            if (s->operator_info().id == holder && s->domains().count(domain) && s->state() != Session::State::Closed) live = true;
        if (!live)
            if (auto c = control_.gone(domain, holder)) apply(*c);
    }
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
    if (s) control_.heartbeat(s->operator_info().id, ControlDomains::Clock::now());
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
    nlohmann::json control = nlohmann::json::object();
    for (const std::string domain : {"desktop", "motion"}) control[domain] = held_data(domain);
    return nlohmann::json{{"sessions", arr}, {"control", std::move(control)}};
}

} // namespace fjarr::core
