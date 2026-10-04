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
