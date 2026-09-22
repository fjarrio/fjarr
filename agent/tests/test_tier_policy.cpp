// The per-viewer tier override (docs/23#rate-control-and-tier-switching): demote after 2 s below
// the band, promote after 10 s above 1.2× its floor, never override a thumbnail demand, never
// demote where no lower tier exists.
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
    EXPECT_FALSE(p.update(1'000'000, low, true, at(0)).has_value());
    EXPECT_FALSE(p.update(1'000'000, low, true, at(1.5)).has_value()); // not yet
    EXPECT_EQ(p.update(1'000'000, low, true, at(2.1)), "thumbnail");
    EXPECT_TRUE(p.demoted());
    EXPECT_EQ(p.demanded(), "active");
    EXPECT_FALSE(p.update(2'100'000, low, true, at(3)).has_value()); // above the floor but under the margin: stays
    EXPECT_FALSE(p.update(2'500'000, low, true, at(4)).has_value());
    EXPECT_FALSE(p.update(2'500'000, low, true, at(8)).has_value()); // 4 s above: not yet
    EXPECT_EQ(p.update(2'500'000, low, true, at(9.1)), "active");
    EXPECT_FALSE(p.demoted());
}

TEST(TierPolicy, aDipThatEndsResetsTheClockAndADemandForThumbnailIsNeverOverridden) {
    TierPolicy p;
    const double low = 2'000'000;
    p.update(1'000'000, low, true, at(0));
    p.update(3'000'000, low, true, at(1)); // recovered before 2 s
    EXPECT_FALSE(p.update(1'000'000, low, true, at(2.5)).has_value()); // a new dip starts its own 2 s
    EXPECT_EQ(p.update(1'000'000, low, true, at(4.6)), "thumbnail");
    p.set_demanded("thumbnail"); // the client itself wants the low tier now
    EXPECT_EQ(p.effective(), "thumbnail");
    EXPECT_FALSE(p.demoted());
    EXPECT_FALSE(p.update(9'000'000, low, true, at(30)).has_value()); // nothing to promote
    p.set_demanded("active");
    EXPECT_EQ(p.effective(), "active"); // the override was cleared with the thumbnail demand
}

TEST(TierPolicy, neverDemotesWhereNoLowerTierExists) {
    TierPolicy p;
    for (double t = 0; t < 10; t += 0.2) EXPECT_FALSE(p.update(100'000, 2'000'000, false, at(t)).has_value());
    EXPECT_EQ(p.effective(), "active");
}
