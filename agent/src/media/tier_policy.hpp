#pragma once
// TierPolicy — one viewer's tier on one track: the client's demand, and the agent's override
// below it while the viewer's link is under the tier's band (demote after 2 s, promote back after
// 5 s above 1.2× the band's floor). Pure state, no timers: the session ticks it.
// spec: docs/23-agent-core-architecture.md#rate-control-and-tier-switching
#include <chrono>
#include <optional>
#include <string>

namespace fjarr::media {

class TierPolicy {
  public:
    using clock = std::chrono::steady_clock;
    static constexpr std::chrono::milliseconds DEMOTE_AFTER{2000};
    static constexpr std::chrono::milliseconds PROMOTE_AFTER{5000};
    static constexpr double PROMOTE_MARGIN = 1.2;

    /// What the client asked for (select-tracks). Resets the override when the demand itself is lower.
    void set_demanded(const std::string& tier);
    const std::string& demanded() const { return demanded_; }
    /// What the agent sends: the demand, or the lower tier while demoted.
    const std::string& effective() const { return effective_; }
    bool demoted() const { return overridden_; }

    /// One tick with this viewer's allotment for the track; `active_low_bps` is the active band's
    /// floor (the demotion line and, ×1.2, the promotion line); `lower_possible` = the track can
    /// serve a thumbnail tier. Returns the new effective tier when it changed.
    std::optional<std::string> update(double allotment_bps, double active_low_bps, bool lower_possible, clock::time_point now);

  private:
    std::string demanded_ = "active";
    std::string effective_ = "active";
    bool overridden_ = false; // the agent holds this viewer below its demand
    std::optional<clock::time_point> below_since_, above_since_;
};

} // namespace fjarr::media
