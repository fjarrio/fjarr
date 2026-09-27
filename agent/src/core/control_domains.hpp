#pragma once
// ControlDomains — who holds each docs/10 control domain, and when that ends.
// Pure bookkeeping: no loop, no sessions, time passed in. The SessionManager
// turns what it returns into release_all_input calls and control-state events.
// spec: docs/10-security.md#session-ownership · docs/23-agent-core-architecture.md (control domains)
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <fjarr/session_context.hpp>

namespace fjarr::core {

class ControlDomains {
  public:
    using Clock = std::chrono::steady_clock;
    /// docs/10: desktop is free this long after the holder's last input.
    static constexpr std::chrono::seconds DESKTOP_IDLE{5};
    /// docs/10: a holder without a heartbeat for this long loses every claim (fail-open).
    static constexpr std::chrono::seconds STALE{30};

    struct Holder {
        OperatorInfo op;
        std::int64_t since_ms = 0; // unix ms, as control-state and control-held carry it
        Clock::time_point last_input{};
        Clock::time_point last_heartbeat{};
    };

    /// A change of holder. `release` names the operator whose input must be released now, before
    /// anything else happens in the domain: the previous holder on a takeover, a release or a
    /// stale claim, or an idle desktop holder once someone else claims it.
    struct Change {
        std::string domain;
        std::optional<std::string> release;
        std::string why; // claim | take | release | idle | stale | gone
    };

    enum class Outcome { Allowed, Held };
    struct InputResult {
        Outcome outcome = Outcome::Allowed;
        std::optional<Change> change; // set when this input claimed a free domain
    };

    static bool known(const std::string& domain) { return domain == "desktop" || domain == "motion"; }

    /// A session of `op` sends input in `domain`: claims it when free, refreshes the idle clock
    /// when `op` holds it, and is refused when someone else does.
    InputResult input(const std::string& domain, const OperatorInfo& op, Clock::time_point now, std::int64_t now_ms);
    /// take-control: `op` holds `domain` from now, whoever held it. nullopt when `op` already did.
    std::optional<Change> take(const std::string& domain, const OperatorInfo& op, Clock::time_point now, std::int64_t now_ms);
    /// release-control: frees `domain` if `op` holds it (idempotent).
    std::optional<Change> release(const std::string& domain, const std::string& operator_id);
    /// `op`'s last session in `domain` is gone: its claim ends now (docs/10 fail-open, no 30 s wait).
    std::optional<Change> gone(const std::string& domain, const std::string& operator_id);
    /// A heartbeat from any session of `operator_id` keeps its claims alive.
    void heartbeat(const std::string& operator_id, Clock::time_point now);
    /// Idle desktop and stale claims end here; call it before reading and on a timer.
    std::vector<Change> expire(Clock::time_point now);

    const Holder* holder(const std::string& domain) const;
    bool any_held() const { return !held_.empty(); }

  private:
    std::map<std::string, Holder> held_;
    /// Desktop freed by idleness: the operator whose input is released when someone else claims.
    std::map<std::string, std::string> idle_previous_;
    Change claim(const std::string& domain, const OperatorInfo& op, Clock::time_point now, std::int64_t now_ms, std::string why);
};

} // namespace fjarr::core
