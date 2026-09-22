#pragma once
// RateEstimator — one peer's congestion estimate from webrtcbin's transport-wide feedback
// (rtpsession `twcc-stats`, one window per update): a loss rule, a delay-gradient rule, bounded,
// with time-based growth that is faster while recovering toward the level before the last cut.
// spec: docs/23-agent-core-architecture.md#rate-control-and-tier-switching · docs/16 (react ~2 s, recover ~10 s)
#include <chrono>
#include <cstdint>
#include <deque>

namespace fjarr::media {

/// One `twcc-stats` window as rtpsession reports it.
struct TwccSample {
    double bitrate_sent = 0; // bps over the window
    double bitrate_recv = 0; // bps the peer acknowledged
    double loss_pct = 0;     // 0..100
    std::int64_t avg_delta_of_delta_ns = 0; // inter-arrival vs inter-departure drift: positive = queues building
    int packets = 0;         // packets sent in the window; 0 = no feedback yet
    int packets_recv = -1;   // packets the peer acknowledged in the window; -1 = unknown (bitrates decide instead)
};

struct RateLimits {
    double floor_bps = 250'000;
    double ceiling_bps = 4'800'000;
};

class RateEstimator {
  public:
    using clock = std::chrono::steady_clock;
    static constexpr double LOSS_HIGH_PCT = 10.0;        // loss above this, with throughput collapsing: cut
    static constexpr double LOSS_LOW_PCT = 2.0;          // below: growth is free
    static constexpr double COLLAPSE_RATIO = 0.7;        // received / sent below this = the link is not carrying what we send
    static constexpr int COLLAPSE_WINDOWS = 3;           // …for this many windows in a row (a lost feedback packet collapses one window; a shaper, all of them)
    static constexpr double CARRIED_RATIO = 0.8;         // received / sent above this = loss is random, not capacity: growth allowed under 10 % loss
    static constexpr double UNCAPPED_RATIO = 0.9;        // received ≈ sent: growth needs no evidence beyond "nothing is lost"
    static constexpr std::int64_t OVERUSE_NS = 3'000'000; // avg delta-of-delta above 3 ms for OVERUSE_WINDOWS in a row: queues are building (jitter flips sign; a shaper does not)
    static constexpr int OVERUSE_WINDOWS = 3;
    static constexpr int MIN_WINDOW_PACKETS = 10;        // loss is judged over ≥ this many packets (a rolling second)
    static constexpr double GROW_PER_S = 0.05;           // at or above the last good level
    static constexpr double RECOVER_PER_S = 0.30;        // below the level before the last cut (docs/16: back within ~10 s; a demoted viewer climbs from the floor to its promotion line in ~8 s)
    static constexpr double OVERUSE_FACTOR = 0.85;
    static constexpr std::chrono::milliseconds LOSS_CUT_HOLD{1000}; // one loss cut per second: see its effect before the next

    RateEstimator(RateLimits limits, double initial_bps, clock::time_point now = clock::now());
    void set_limits(RateLimits limits);
    /// One feedback window; `now` is when it was read. A window with no packets carries no
    /// evidence: the estimate holds (no growth on silence).
    void update(const TwccSample& s, clock::time_point now);
    double estimate_bps() const { return estimate_; }
    /// The estimate before the most recent cut (growth is faster until it is back).
    double last_good_bps() const { return last_good_; }
    /// Loss over the rolling second, percent.
    double loss_pct() const { return loss_pct_; }
    /// Packets acknowledged / packets sent over the rolling second (1 when unknown).
    double carried_ratio() const { return ratio_; }
    const char* state() const;
    int decreases() const { return decreases_; }

  private:
    enum class State { Hold, Increase, Decrease };
    struct Window {
        clock::time_point at;
        double packets, lost, acked;
    };
    double clamp(double v) const;
    RateLimits limits_;
    double estimate_;
    double last_good_;
    clock::time_point last_;
    clock::time_point last_loss_cut_{};
    bool loss_cut_seen_ = false;
    std::deque<Window> windows_; // the rolling second
    double loss_pct_ = 0;
    double ratio_ = 1.0;
    int overuse_streak_ = 0;
    int collapse_streak_ = 0;
    State state_ = State::Hold;
    int decreases_ = 0;
};

} // namespace fjarr::media
