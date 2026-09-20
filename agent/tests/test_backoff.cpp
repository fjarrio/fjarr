// docs/08#reconnection numbers as a pure class.
#include <gtest/gtest.h>

#include "core/backoff.hpp"

using namespace std::chrono;

TEST(Backoff, doublesFromHalfASecondWithCapAndJitter) {
    fjarr::Backoff b({}, [] { return 0.5; }); // jitter factor 1.0
    EXPECT_EQ(b.next().count(), 500);
    EXPECT_EQ(b.next().count(), 1000);
    EXPECT_EQ(b.next().count(), 2000);
    for (int i = 0; i < 10; i++) b.next();
    EXPECT_EQ(b.next().count(), 30000);
}

TEST(Backoff, jitterIsPlusMinusTwentyPercent) {
    fjarr::Backoff lo({}, [] { return 0.0; });
    fjarr::Backoff hi({}, [] { return 1.0; });
    EXPECT_EQ(lo.next().count(), 400);
    EXPECT_EQ(hi.next().count(), 600);
}

TEST(Backoff, resetsOnlyAfterThirtySecondsStable) {
    fjarr::Backoff b({}, [] { return 0.5; });
    b.next();
    b.next();
    const auto t0 = steady_clock::now();
    b.mark_connected(t0);
    b.mark_disconnected(t0 + seconds(10)); // flapped: keeps counting
    EXPECT_EQ(b.next().count(), 2000);
    b.mark_connected(t0);
    b.mark_disconnected(t0 + seconds(31)); // stable: fresh budget
    EXPECT_EQ(b.next().count(), 500);
}
