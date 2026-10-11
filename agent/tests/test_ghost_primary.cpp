// docs/26#ghost-screens: the session helper's primary rule (agent/desktop-session/main.cpp).
#include <gtest/gtest.h>

#include "desktop/ghost_primary.hpp"

using fjarr::desktop::primary_to_make;
using fjarr::desktop::Screen;
using fjarr::desktop::sole_primary;

TEST(GhostPrimary, aPrimaryGhostHandsItToTheLeftmostRealMonitor) {
    const std::vector<Screen> s{{"HDMI-1", true, true, false, 0, 0}, {"DP-6", false, false, false, 3840, 0}, {"DP-4", false, false, false, 1920, 0}};
    EXPECT_EQ(primary_to_make(s), "DP-4");
}

TEST(GhostPrimary, aGhostMirroredWithThePrimaryRealMonitorIsLeftAlone) {
    // The mini-PC, two ghosts and a Dell (2026-10-11): mutter mirrored DP-2 with DP-4 in the primary
    // logical monitor, both primary. Making DP-4 primary again looped and exhausted memory.
    const std::vector<Screen> s{{"DP-2", true, true, false, 0, 0}, {"DP-4", true, false, false, 0, 0}, {"HDMI-1", false, true, false, 1920, 0}};
    EXPECT_EQ(primary_to_make(s), std::nullopt);
}

TEST(GhostPrimary, onlyGhostsOrNoGhostPrimaryChangeNothing) {
    EXPECT_EQ(primary_to_make({{"DP-2", true, true, false, 0, 0}, {"HDMI-1", false, true, false, 1920, 0}}), std::nullopt);
    EXPECT_EQ(primary_to_make({{"DP-4", true, false, false, 0, 0}, {"HDMI-1", false, true, false, 1920, 0}}), std::nullopt);
    EXPECT_EQ(primary_to_make({{"Meta-0", false, false, true, 0, 0}, {"HDMI-1", true, true, false, 1920, 0}}), std::nullopt) << "a virtual monitor is never made primary";
}

TEST(GhostPrimary, severalPrimariesAreReportedAsOneRealOne) {
    // mutter reported DP-2 (a ghost) and DP-5 (the Dell, re-plugged under a new name) both primary.
    const std::vector<Screen> s{{"DP-2", true, true, false, 0, 0}, {"DP-5", true, false, false, 1920, 0}, {"HDMI-1", false, true, false, 3840, 0}};
    EXPECT_EQ(sole_primary(s), "DP-5");
    EXPECT_EQ(sole_primary({{"DP-4", true, false, false, 1920, 0}, {"DP-6", true, false, false, 0, 0}}), "DP-6") << "the leftmost of two real ones";
    EXPECT_EQ(sole_primary({{"DP-4", true, false, false, 0, 0}, {"DP-2", false, true, false, 1920, 0}}), std::nullopt) << "one primary is left as it is";
}
