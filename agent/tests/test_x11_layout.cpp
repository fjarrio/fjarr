// The X11 kiosk's output layout (docs/23#desktop-x11, fjarr-x11-session). The cases are ADR-0006's
// spike findings on the mini-PC's DisplayPort chain.
#include <gtest/gtest.h>

#include "desktop/x11_layout.hpp"

using fjarr::desktop::x11::Output;
using fjarr::desktop::x11::plan;

TEST(X11Layout, aBareKioskThatSwitchedOnOneOfThreeGetsAllThreeLeftToRight) {
    // Xorg switched on one of the three connected monitors (ADR-0006).
    const auto p = plan({{"DisplayPort-4", true, true, 0, 0, 1920, 1080, 1920, 1080},
                         {"DisplayPort-10", true, false, 0, 0, 0, 0, 1920, 1080},
                         {"DisplayPort-8", true, false, 0, 0, 0, 0, 1920, 1080}});
    EXPECT_TRUE(p.changes);
    ASSERT_EQ(p.on.size(), 3u);
    EXPECT_EQ(p.on[0].name, "DisplayPort-4"); // 4, 8, 10: the number compared as a number
    EXPECT_EQ(p.on[1].name, "DisplayPort-8");
    EXPECT_EQ(p.on[2].name, "DisplayPort-10");
    EXPECT_EQ(p.on[1].x, 1920);
    EXPECT_EQ(p.on[2].x, 3840);
    EXPECT_EQ(p.screen_w, 5760);
    EXPECT_EQ(p.screen_h, 1080);
}

TEST(X11Layout, anUnpluggedOutputStillHoldingACrtcIsSwitchedOffAndTheRestClosesUp) {
    // After a hot-plug, outputs came back under new names, disconnected but still holding a CRTC.
    const auto p = plan({{"DisplayPort-4", false, true, 0, 0, 1920, 1080, 1920, 1080},
                         {"DisplayPort-6", true, true, 1920, 0, 1920, 1080, 1920, 1080}});
    EXPECT_EQ(p.off, std::vector<std::string>{"DisplayPort-4"});
    ASSERT_EQ(p.on.size(), 1u);
    EXPECT_EQ(p.on[0].x, 0) << "positions are set explicitly: shrinking the root moved windows";
    EXPECT_EQ(p.screen_w, 1920);
}

TEST(X11Layout, aLayoutThatIsRightAlreadyChangesNothing) {
    const auto p = plan({{"HDMI-1", true, true, 0, 0, 1920, 1080, 1920, 1080}, {"HDMI-2", true, true, 1920, 0, 1280, 1024, 1280, 1024}});
    EXPECT_FALSE(p.changes);
    EXPECT_EQ(p.screen_w, 3200);
    EXPECT_EQ(p.screen_h, 1080);
}

TEST(X11Layout, noConnectedOutputIsAnEmptyPlanNotAnError) {
    const auto p = plan({{"HDMI-1", false, false, 0, 0, 0, 0, 0, 0}});
    EXPECT_TRUE(p.on.empty());
    EXPECT_TRUE(p.off.empty());
    EXPECT_FALSE(p.changes);
}

// docs/26#ghost-screens: a ghost is never primary while a real monitor is connected. On the mini-PC
// (2026-10-05) Xorg made the ghost primary beside three Dells, and the dashboard opened on it.
TEST(X11Layout, aGhostThatIsPrimaryHandsItToTheLeftmostRealMonitor) {
    Output ghost{"HDMI-A-0", true, true, 0, 0, 1920, 1080, 1920, 1080, /*ghost=*/true, /*primary=*/true};
    Output dell3{"DisplayPort-3", true, true, 0, 0, 1920, 1080, 1920, 1080};
    Output dell5{"DisplayPort-5", true, true, 1920, 0, 1920, 1080, 1920, 1080};
    ghost.x = 3840;
    const auto p = plan({ghost, dell5, dell3});
    EXPECT_EQ(p.primary, "DisplayPort-3");
    EXPECT_FALSE(p.changes) << "the layout is right already: making a monitor primary moves no CRTC";
}

TEST(X11Layout, aRealPrimaryStaysAndOnlyGhostsKeepTheirs) {
    Output dell5{"DisplayPort-5", true, true, 1920, 0, 1920, 1080, 1920, 1080, false, /*primary=*/true};
    Output dell3{"DisplayPort-3", true, true, 0, 0, 1920, 1080, 1920, 1080};
    EXPECT_EQ(plan({dell3, dell5}).primary, "") << "the operator's choice of a real monitor stands";
    Output g1{"HDMI-A-0", true, true, 0, 0, 1920, 1080, 1920, 1080, true, true};
    EXPECT_EQ(plan({g1}).primary, "") << "only ghosts: one of them is primary, as it must be";
}

TEST(X11Layout, noPrimaryOrAnUnpluggedOneGetsTheLeftmostRealMonitor) {
    Output dell3{"DisplayPort-3", true, true, 0, 0, 1920, 1080, 1920, 1080};
    Output gone{"DisplayPort-7", false, true, 1920, 0, 1920, 1080, 1920, 1080, false, true};
    EXPECT_EQ(plan({dell3}).primary, "DisplayPort-3");
    EXPECT_EQ(plan({dell3, gone}).primary, "DisplayPort-3");
}
