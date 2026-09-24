// spec: docs/23-agent-core-architecture.md#rate-control-and-tier-switching
#include "tier_policy.hpp"

namespace fjarr::media {

void TierPolicy::set_demanded(const std::string& tier) {
    demanded_ = tier;
    if (tier != "active") { // a demand at the lower tier: nothing to override, and a standing override ends
        overridden_ = false;
        below_since_.reset();
        above_since_.reset();
    }
    effective_ = overridden_ ? "thumbnail" : tier;
}

std::optional<std::string> TierPolicy::update(double allotment_bps, double active_low_bps, bool lower_possible, bool estimate_tested, clock::time_point now) {
    if (demanded_ != "active") return std::nullopt;
    if (!demoted()) {
        // An estimate the sender never pushed against is untested, not low: a scene that compresses
        // well emits far below its target, the estimator will not credit more than 1.5x what
        // arrived, and the viewer would be demoted on a perfect link (docs/23; found by the nightly
        // 2026-09-24). The timer does not even start until the estimate is a real constraint.
        if (!lower_possible || !estimate_tested || allotment_bps >= active_low_bps) {
            below_since_.reset();
            return std::nullopt;
        }
        if (!below_since_) below_since_ = now;
        if (now - *below_since_ < DEMOTE_AFTER) return std::nullopt;
        below_since_.reset();
        overridden_ = true;
        effective_ = "thumbnail";
        return effective_;
    }
    if (allotment_bps < PROMOTE_MARGIN * active_low_bps) {
        above_since_.reset();
        return std::nullopt;
    }
    if (!above_since_) above_since_ = now;
    if (now - *above_since_ < PROMOTE_AFTER) return std::nullopt;
    above_since_.reset();
    overridden_ = false;
    effective_ = "active";
    return effective_;
}

} // namespace fjarr::media
