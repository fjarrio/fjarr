// The per-viewer tier override (docs/23#rate-control-and-tier-switching): demote after 2 s below
// the band, promote after 10 s above 1.2× its floor, never override a thumbnail demand, never
// demote where no lower tier exists, and never demote on an estimate the sender never tested.
#include <gtest/gtest.h>

#include "media/tier_policy.hpp"

using namespace fjarr::media;
using namespace std::chrono;

namespace {
TierPolicy::clock::time_point at(double s) { return TierPolicy::clock::time_point{} + duration_cast<TierPolicy::clock::duration>(duration<double>(s)); }
} // namespace

TEST(TierPolicy, demotesAfterTwoSecondsBelowTheBandAndPromotesAfterTenAbove) {
    TierPolicy p;
    const double low = 2'000'000; // the active band's floor
    EXPECT_FALSE(p.update(1'000'000, low, true, /*estimate_tested=*/true, at(0)).has_value());
    EXPECT_FALSE(p.update(1'000'000, low, true, /*estimate_tested=*/true, at(1.5)).has_value()); // not yet
    EXPECT_EQ(p.update(1'000'000, low, true, /*estimate_tested=*/true, at(2.1)), "thumbnail");
    EXPECT_TRUE(p.demoted());
    EXPECT_EQ(p.demanded(), "active");
    EXPECT_FALSE(p.update(2'100'000, low, true, /*estimate_tested=*/true, at(3)).has_value()); // above the floor but under the margin: stays
    EXPECT_FALSE(p.update(2'500'000, low, true, /*estimate_tested=*/true, at(4)).has_value());
    EXPECT_FALSE(p.update(2'500'000, low, true, /*estimate_tested=*/true, at(8)).has_value()); // 4 s above: not yet
    EXPECT_EQ(p.update(2'500'000, low, true, /*estimate_tested=*/true, at(9.1)), "active");
    EXPECT_FALSE(p.demoted());
}

TEST(TierPolicy, aDipThatEndsResetsTheClockAndADemandForThumbnailIsNeverOverridden) {
    TierPolicy p;
    const double low = 2'000'000;
    p.update(1'000'000, low, true, /*estimate_tested=*/true, at(0));
    p.update(3'000'000, low, true, /*estimate_tested=*/true, at(1)); // recovered before 2 s
    EXPECT_FALSE(p.update(1'000'000, low, true, /*estimate_tested=*/true, at(2.5)).has_value()); // a new dip starts its own 2 s
    EXPECT_EQ(p.update(1'000'000, low, true, /*estimate_tested=*/true, at(4.6)), "thumbnail");
    p.set_demanded("thumbnail"); // the client itself wants the low tier now
    EXPECT_EQ(p.effective(), "thumbnail");
    EXPECT_FALSE(p.demoted());
    EXPECT_FALSE(p.update(9'000'000, low, true, /*estimate_tested=*/true, at(30)).has_value()); // nothing to promote
    p.set_demanded("active");
    EXPECT_EQ(p.effective(), "active"); // the override was cleared with the thumbnail demand
}

TEST(TierPolicy, neverDemotesWhereNoLowerTierExists) {
    TierPolicy p;
    for (double t = 0; t < 10; t += 0.2) EXPECT_FALSE(p.update(100'000, 2'000'000, false, /*estimate_tested=*/true, at(t)).has_value());
    EXPECT_EQ(p.effective(), "active");
}

TEST(TierPolicy, neverDemotesOnAnEstimateTheSenderNeverPushedAgainst) {
    // The defect the nightly found on 2026-09-24. The estimator credits no more than 1.5x what
    // arrived, by design — an encoder sending less than the estimate proves nothing about the
    // ceiling. So a source that compresses well (a static scene, a camera on a white wall, our own
    // test pattern) emits far below its target, the estimate is pinned near that, and it lands
    // under the band floor while the link is *perfect*. Demoting there drops a viewer to a
    // thumbnail for a reason that has nothing to do with the network.
    TierPolicy p;
    const double low = 2'000'000;
    for (double t = 0; t <= 30; t += 0.5) {
        ASSERT_FALSE(p.update(1'500'000, low, true, /*estimate_tested=*/false, at(t)).has_value())
            << "demoted at t=" << t << " s on an estimate nothing was pushing against";
    }
    EXPECT_FALSE(p.demoted());
    EXPECT_EQ(p.effective(), "active");

    // And the moment the peer does push against it — the link really is the limit — the ordinary
    // rule applies again, from a clock that starts now rather than one that has been running.
    EXPECT_FALSE(p.update(1'500'000, low, true, /*estimate_tested=*/true, at(30.5)).has_value());
    EXPECT_FALSE(p.update(1'500'000, low, true, /*estimate_tested=*/true, at(32.0)).has_value());
    EXPECT_EQ(p.update(1'500'000, low, true, /*estimate_tested=*/true, at(32.6)), "thumbnail");
}

TEST(TierPolicy, anUntestedEstimateStillPromotesADemotedViewer) {
    // The mirror does NOT hold: a high estimate is evidence a viewer can be restored even if the
    // encoder is not currently pushing against it (a demoted viewer is sending the thumbnail's
    // bitrate by definition, so it never would). Refusing to promote on it would strand viewers.
    TierPolicy p;
    const double low = 2'000'000;
    EXPECT_FALSE(p.update(1'000'000, low, true, /*estimate_tested=*/true, at(0)).has_value());
    EXPECT_EQ(p.update(1'000'000, low, true, /*estimate_tested=*/true, at(2.1)), "thumbnail");
    ASSERT_TRUE(p.demoted());
    EXPECT_FALSE(p.update(2'500'000, low, true, /*estimate_tested=*/false, at(3)).has_value());
    EXPECT_EQ(p.update(2'500'000, low, true, /*estimate_tested=*/false, at(13.1)), "active");
    EXPECT_FALSE(p.demoted());
}
