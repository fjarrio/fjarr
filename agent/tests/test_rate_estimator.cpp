// The per-peer estimator (docs/23#rate-control-and-tier-switching) against docs/16's numbers and
// what the lab found: a cut within three windows of a shaper or of queue growth, none on random loss
// or on a lost feedback packet, growth back within ~10 s, a climb out of the thumbnail tier, bounds.
#include <gtest/gtest.h>

#include "media/rate_estimator.hpp"

using namespace fjarr::media;
using namespace std::chrono;

namespace {
RateEstimator::clock::time_point at(double s) { return RateEstimator::clock::time_point{} + duration_cast<RateEstimator::clock::duration>(duration<double>(s)); }
TwccSample clean(double recv_bps) { return TwccSample{recv_bps, recv_bps, 0.0, 0, 30, 30}; }
const TwccSample SHAPED{4'000'000, 1'400'000, 20.0, 0, 30, 11}; // a 1.5 Mbit shaper: 35 % of what we send arrives, 20 % lost
} // namespace

TEST(RateEstimator, growsFivePercentPerSecondOnCleanWindowsUpToTheCeiling) {
    RateEstimator e({250'000, 4'800'000}, 4'000'000, at(0));
    for (int i = 1; i <= 8; i++) e.update(clean(4'000'000), at(i * 0.25)); // 2 s
    EXPECT_STREQ(e.state(), "increase");
    EXPECT_NEAR(e.estimate_bps(), 4'000'000 * 1.05 * 1.05, 4'000'000 * 0.02); // ~5 %/s, compounded per window
    for (int i = 9; i <= 40; i++) e.update(clean(4'000'000), at(i * 0.25)); // 10 s in all
    EXPECT_LE(e.estimate_bps(), 4'800'000);
}

TEST(RateEstimator, cutsAfterThreeCollapsedWindowsOncePerSecondAndNeverAboveWhatArrived) {
    RateEstimator e({250'000, 4'800'000}, 4'000'000, at(0));
    e.update(SHAPED, at(0.25));
    e.update(SHAPED, at(0.5));
    EXPECT_EQ(e.decreases(), 0); // two collapsed windows: what a lost feedback packet looks like
    e.update(SHAPED, at(0.75)); // three in a row: a shaper
    EXPECT_STREQ(e.state(), "decrease");
    EXPECT_LE(e.estimate_bps(), 1'400'000); // ≤ what arrived
    EXPECT_EQ(e.decreases(), 1);
    EXPECT_DOUBLE_EQ(e.last_good_bps(), 4'000'000);
    e.update(TwccSample{1'400'000, 900'000, 20.0, 0, 30, 19}, at(1.0)); // still lossy: no second cut within the second
    EXPECT_EQ(e.decreases(), 1);
    EXPECT_GE(e.estimate_bps(), 1'000'000);
}

TEST(RateEstimator, randomLossWithThroughputIntactIsNotACapacitySignal) {
    // A radio link losing 15 % of everything: what we send arrives at 85 % and retransmission
    // repairs the rest; sending less would not reduce the loss (docs/23) — the estimate stays.
    RateEstimator e({250'000, 4'800'000}, 4'000'000, at(0));
    for (int i = 1; i <= 40; i++) e.update(TwccSample{4'000'000, 3'400'000, 15.0, (i % 2) ? 800'000 : -900'000, 30, 26}, at(i * 0.25));
    EXPECT_GE(e.estimate_bps(), 4'000'000);
    EXPECT_EQ(e.decreases(), 0);
    EXPECT_NEAR(e.loss_pct(), 15.0, 0.5);
    // one lost packet in a seven-packet window is not 14 % loss
    RateEstimator f({250'000, 4'800'000}, 4'000'000, at(0));
    f.update(TwccSample{4'000'000, 1'000'000, 14.3, 0, 7, 6}, at(0.25));
    EXPECT_EQ(f.decreases(), 0);
    // jitter on the feedback path: windows alternate between "acked more than sent" and "acked half"
    RateEstimator h({250'000, 4'800'000}, 4'000'000, at(0));
    for (int i = 1; i <= 40; i++) h.update((i % 2) ? TwccSample{2'000'000, 3'800'000, 5.0, 0, 8, 15} : TwccSample{6'000'000, 3'000'000, 5.0, 0, 22, 10}, at(i * 0.25));
    EXPECT_EQ(h.decreases(), 0);
    // a lost feedback packet every so often: the window after it reports half of everything as lost
    RateEstimator g({250'000, 4'800'000}, 4'000'000, at(0));
    for (int i = 1; i <= 40; i++) {
        const bool after_lost_feedback = i % 8 == 0;
        g.update(after_lost_feedback ? TwccSample{4'000'000, 1'900'000, 50.0, 0, 24, 12} : TwccSample{4'000'000, 3'800'000, 5.0, 0, 12, 11}, at(i * 0.25));
    }
    EXPECT_EQ(g.decreases(), 0);
}

TEST(RateEstimator, cutsOnThreeWindowsOfQueueGrowthNotTwo) {
    RateEstimator e({250'000, 4'800'000}, 4'000'000, at(0));
    e.update(TwccSample{4'000'000, 3'900'000, 0.0, 4'500'000, 30, 30}, at(0.25)); // 4.5 ms of queue growth
    e.update(TwccSample{4'000'000, 3'900'000, 0.0, 4'500'000, 30, 30}, at(0.5));
    EXPECT_NE(e.state(), std::string("decrease")); // two windows: no cut yet
    EXPECT_GE(e.estimate_bps(), 4'000'000);
    e.update(TwccSample{4'000'000, 3'900'000, 0.0, 4'500'000, 30, 30}, at(0.75));
    EXPECT_STREQ(e.state(), "decrease");
    EXPECT_NEAR(e.estimate_bps(), 0.85 * 3'900'000, 1.0);
    // jitter that flips sign never makes three in a row
    RateEstimator j({250'000, 4'800'000}, 4'000'000, at(0));
    for (int i = 1; i <= 40; i++) j.update(TwccSample{4'000'000, 3'950'000, 0.0, (i % 3) ? 5'000'000 : -2'500'000, 30, 30}, at(i * 0.25));
    EXPECT_EQ(j.decreases(), 0);
}

TEST(RateEstimator, recoversToNinetyPercentWithinTenSecondsAndHoldsTheFloor) {
    RateEstimator e({250'000, 4'800'000}, 4'000'000, at(0));
    for (int i = 1; i <= 3; i++) e.update(TwccSample{4'000'000, 1'500'000, 30.0, 0, 30, 11}, at(i * 0.25)); // 37 % arrives, three windows: over capacity
    EXPECT_LE(e.estimate_bps(), 1'500'000);
    double t = 0.75, reached = 0;
    for (int i = 0; i < 40; i++) { // 10 s of clean windows; the peer receives what we send
        t += 0.25;
        e.update(clean(e.estimate_bps()), at(t));
        if (e.estimate_bps() >= 3'600'000 && reached == 0) reached = t - 0.75;
    }
    EXPECT_GT(reached, 0) << "did not reach 90 % of the level before the cut within 10 s (docs/16)";
    EXPECT_LE(reached, 10.0);
    // the floor: repeated collapse never takes the estimate below it
    for (int i = 0; i < 20; i++) e.update(TwccSample{300'000, 100'000, 50.0, 0, 30, 10}, at(20 + i * 0.5));
    EXPECT_DOUBLE_EQ(e.estimate_bps(), 250'000);
}

TEST(RateEstimator, climbsOutOfAThumbnailWhenNothingIsLost) {
    // A demoted viewer receives 300 kbps: the estimate may climb on faith while nothing is lost
    // (the delay rule guards an overshoot) so the tier can be earned back within ~10 s.
    RateEstimator e({250'000, 4'800'000}, 4'000'000, at(0));
    for (int i = 1; i <= 3; i++) e.update(SHAPED, at(i * 0.25)); // the cut
    double t = 0.75, reached = 0;
    for (int i = 0; i < 60 && reached == 0; i++) {
        t += 0.25;
        e.update(TwccSample{300'000, 300'000, 0.0, 0, 8, 8}, at(t));
        if (e.estimate_bps() >= 2'400'000) reached = t - 0.75;
    }
    EXPECT_GT(reached, 0);
    EXPECT_LE(reached, 12.0) << "the promotion line (1.2× the active band's floor) within ~10 s";
}

TEST(RateEstimator, holdsWithoutFeedbackAndDoesNotInflateBeyondWhatArrives) {
    RateEstimator e({250'000, 4'800'000}, 4'000'000, at(0));
    e.update(TwccSample{}, at(0.25)); // no packets: nothing learned
    EXPECT_STREQ(e.state(), "hold");
    EXPECT_DOUBLE_EQ(e.estimate_bps(), 4'000'000);
    for (int i = 1; i <= 40; i++) e.update(TwccSample{1'000'000, 850'000, 3.0, 0, 30, 26}, at(i * 0.25)); // a still scene with a little loss: 1 Mbps sent, 85 % arrives
    EXPECT_LE(e.estimate_bps(), 4'000'000); // never grew on traffic that was not there
}
