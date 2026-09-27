#include "control_domains.hpp"

namespace fjarr::core {

ControlDomains::Change ControlDomains::claim(const std::string& domain, const OperatorInfo& op, Clock::time_point now,
                                             std::int64_t now_ms, std::string why) {
    Change c{domain, std::nullopt, std::move(why)};
    if (auto it = held_.find(domain); it != held_.end() && it->second.op.id != op.id) c.release = it->second.op.id;
    // An idle desktop holder may still have a key or button down: released when someone else takes over.
    if (auto it = idle_previous_.find(domain); it != idle_previous_.end()) {
        if (!c.release && it->second != op.id) c.release = it->second;
        idle_previous_.erase(it);
    }
    held_[domain] = Holder{op, now_ms, now, now};
    return c;
}

ControlDomains::InputResult ControlDomains::input(const std::string& domain, const OperatorInfo& op, Clock::time_point now,
                                                  std::int64_t now_ms) {
    auto it = held_.find(domain);
    if (it == held_.end()) return InputResult{Outcome::Allowed, claim(domain, op, now, now_ms, "claim")};
    if (it->second.op.id != op.id) return InputResult{Outcome::Held, std::nullopt};
    it->second.last_input = now;
    return InputResult{Outcome::Allowed, std::nullopt};
}

std::optional<ControlDomains::Change> ControlDomains::take(const std::string& domain, const OperatorInfo& op, Clock::time_point now,
                                                           std::int64_t now_ms) {
    if (auto it = held_.find(domain); it != held_.end() && it->second.op.id == op.id) {
        it->second.last_input = now; // taking what you hold restarts the desktop idle clock
        return std::nullopt;
    }
    return claim(domain, op, now, now_ms, "take");
}

std::optional<ControlDomains::Change> ControlDomains::release(const std::string& domain, const std::string& operator_id) {
    auto it = held_.find(domain);
    if (it == held_.end() || it->second.op.id != operator_id) return std::nullopt;
    held_.erase(it);
    return Change{domain, operator_id, "release"};
}

std::optional<ControlDomains::Change> ControlDomains::gone(const std::string& domain, const std::string& operator_id) {
    if (auto it = idle_previous_.find(domain); it != idle_previous_.end() && it->second == operator_id) idle_previous_.erase(it);
    auto it = held_.find(domain);
    if (it == held_.end() || it->second.op.id != operator_id) return std::nullopt;
    held_.erase(it);
    // Every session of the operator released its input on its own close path already.
    return Change{domain, std::nullopt, "gone"};
}

void ControlDomains::heartbeat(const std::string& operator_id, Clock::time_point now) {
    for (auto& [_, h] : held_)
        if (h.op.id == operator_id) h.last_heartbeat = now;
}

std::vector<ControlDomains::Change> ControlDomains::expire(Clock::time_point now) {
    std::vector<Change> out;
    for (auto it = held_.begin(); it != held_.end();) {
        const auto& [domain, h] = *it;
        if (now - h.last_heartbeat > STALE) {
            // Fail-open (docs/10): release at once, a stuck holder must not keep the robot moving.
            out.push_back(Change{domain, h.op.id, "stale"});
            it = held_.erase(it);
        } else if (domain == "desktop" && now - h.last_input > DESKTOP_IDLE) {
            // Not released now: an idle holder may be mid-drag. Released when someone else claims.
            idle_previous_[domain] = h.op.id;
            out.push_back(Change{domain, std::nullopt, "idle"});
            it = held_.erase(it);
        } else {
            ++it;
        }
    }
    return out;
}

const ControlDomains::Holder* ControlDomains::holder(const std::string& domain) const {
    auto it = held_.find(domain);
    return it == held_.end() ? nullptr : &it->second;
}

} // namespace fjarr::core
