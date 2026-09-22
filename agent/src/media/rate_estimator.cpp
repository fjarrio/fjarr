// spec: docs/23-agent-core-architecture.md#rate-control-and-tier-switching
#include "rate_estimator.hpp"

#include <algorithm>

namespace fjarr::media {

RateEstimator::RateEstimator(RateLimits limits, double initial_bps, clock::time_point now)
    : limits_(limits), estimate_(0), last_good_(0), last_(now) {
    estimate_ = clamp(initial_bps);
    last_good_ = estimate_;
}

void RateEstimator::set_limits(RateLimits limits) {
    limits_ = limits;
    estimate_ = clamp(estimate_);
    last_good_ = clamp(last_good_);
}

double RateEstimator::clamp(double v) const { return std::min(std::max(v, limits_.floor_bps), limits_.ceiling_bps); }

void RateEstimator::update(const TwccSample& s, clock::time_point now) {
    const double dt = std::min(1.0, std::max(0.0, std::chrono::duration<double>(now - last_).count()));
    last_ = now;
    if (s.packets <= 0) {
        state_ = State::Hold;
        return;
    }
    // Loss and throughput over a rolling second: a 7-packet window with one loss reads 14 % on its
    // own, and one window's bitrates are distorted by jitter on the feedback path (a feedback packet
    // arriving early or late moves packets between windows); the packet counts over a second are not.
    const double acked = s.packets_recv >= 0 ? s.packets_recv : (s.bitrate_sent > 0 ? s.packets * s.bitrate_recv / s.bitrate_sent : s.packets);
    windows_.push_back({now, static_cast<double>(s.packets), s.packets * s.loss_pct / 100.0, acked});
    while (!windows_.empty() && now - windows_.front().at > std::chrono::seconds(1)) windows_.pop_front();
    double packets = 0, lost = 0, acked_sum = 0;
    for (const auto& w : windows_) {
        packets += w.packets;
        lost += w.lost;
        acked_sum += w.acked;
    }
    loss_pct_ = packets >= MIN_WINDOW_PACKETS ? 100.0 * lost / packets : 0.0;
    ratio_ = packets >= MIN_WINDOW_PACKETS ? std::min(1.0, acked_sum / packets) : 1.0;
    // Throughput carried, this window: bytes acknowledged over bytes sent is what a shaper collapses
    // (1.4 Mbps of 3.8 Mbps arrive) while packet counts barely move; the streak below takes care of
    // the odd window a late feedback packet distorts.
    const double ratio = s.bitrate_sent > 0 ? std::min(1.0, s.bitrate_recv / s.bitrate_sent) : ratio_;
    const bool overuse = s.avg_delta_of_delta_ns > OVERUSE_NS;
    overuse_streak_ = overuse ? overuse_streak_ + 1 : 0;
    collapse_streak_ = ratio < COLLAPSE_RATIO ? collapse_streak_ + 1 : 0;
    const double before = estimate_;
    if (overuse_streak_ >= OVERUSE_WINDOWS) {
        // Queues building for three windows in a row: settle below what arrived.
        const double base = s.bitrate_recv > 0 ? s.bitrate_recv : estimate_;
        estimate_ = clamp(std::min(estimate_, OVERUSE_FACTOR * base));
        overuse_streak_ = 0;
        state_ = State::Decrease;
    } else if (loss_pct_ > LOSS_HIGH_PCT && collapse_streak_ >= COLLAPSE_WINDOWS && (!loss_cut_seen_ || now - last_loss_cut_ >= LOSS_CUT_HOLD)) {
        // Loss *and* the peer receives far less than we send, window after window: over capacity.
        // Take half the loss fraction off and never keep more than what arrived — once per second,
        // so the cut's effect is seen before the next. Loss alone (a radio link at 15 % with
        // throughput intact) is not a capacity signal: retransmission repairs it and the rate
        // stays; and a single collapsed window is what a lost feedback packet looks like (the next
        // feedback reports everything it did not cover as lost), not a shaper.
        double cut = estimate_ * (1.0 - loss_pct_ / 200.0);
        if (s.bitrate_recv > 0) cut = std::min(cut, s.bitrate_recv);
        estimate_ = clamp(cut);
        last_loss_cut_ = now;
        loss_cut_seen_ = true;
        state_ = State::Decrease;
    } else if (!overuse && (loss_pct_ < LOSS_LOW_PCT || (loss_pct_ < LOSS_HIGH_PCT && ratio >= CARRIED_RATIO))) {
        const double rate = estimate_ < last_good_ ? RECOVER_PER_S : GROW_PER_S;
        double grown = estimate_ * (1.0 + rate * dt);
        // An encoder sending less than the estimate (a thumbnail viewer, a still scene) gives no
        // evidence above 1.5× what arrived — unless nothing is lost at all, when the estimate may
        // climb on faith and the delay rule catches an overshoot within two windows. Without that,
        // a demoted viewer could never earn its tier back.
        if (s.bitrate_recv > 0 && ratio < UNCAPPED_RATIO) grown = std::min(grown, std::max(estimate_, 1.5 * s.bitrate_recv));
        estimate_ = clamp(grown);
        state_ = estimate_ > before ? State::Increase : State::Hold;
    } else {
        state_ = State::Hold;
    }
    if (state_ == State::Decrease) {
        if (estimate_ < before) {
            last_good_ = std::max(last_good_, before);
            decreases_++;
        }
    } else if (estimate_ >= last_good_) {
        last_good_ = estimate_;
    }
}

const char* RateEstimator::state() const {
    switch (state_) {
    case State::Increase: return "increase";
    case State::Decrease: return "decrease";
    default: return "hold";
    }
}

} // namespace fjarr::media
