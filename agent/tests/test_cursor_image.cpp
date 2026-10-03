// The cursor's pixels and names (docs/08 `cursor`; docs/23#desktop-helper-protocol, The cursor).
#include <gtest/gtest.h>

#include "desktop/cursor_image.hpp"

using namespace fjarr::desktop::mutter;

TEST(CursorImage, premultipliedPixelsComeOutStraightAndBgraComesOutRgba) {
    // Half-transparent pure red, premultiplied (128 red at 128 alpha) and opaque blue, in BGRA.
    const std::uint8_t bgra[] = {0, 0, 128, 128, 255, 0, 0, 255};
    const auto out = to_straight_rgba(bgra, 2, 1, 8, true);
    EXPECT_EQ(out, (std::vector<std::uint8_t>{255, 0, 0, 128, 0, 0, 255, 255}));
    // A colour above its alpha proves the pixels are straight already: left as they are.
    const std::uint8_t straight[] = {200, 10, 10, 100};
    EXPECT_EQ(to_straight_rgba(straight, 1, 1, 4, false), (std::vector<std::uint8_t>{200, 10, 10, 100}));
}

TEST(CursorImage, aShapeIsNamedByItsContentSoEqualShapesShareAnIdAndAHiddenOneIsHidden) {
    CursorImage a;
    a.width = 1;
    a.height = 1;
    a.rgba = {1, 2, 3, 4};
    CursorImage b = a;
    EXPECT_EQ(shape_id(a), shape_id(b));
    b.hot_x = 1; // the same pixels at another hotspot are another cursor
    EXPECT_NE(shape_id(a), shape_id(b));
    b = a;
    b.rgba[0] = 9;
    EXPECT_NE(shape_id(a), shape_id(b));
    CursorImage none;
    none.visible = false;
    EXPECT_EQ(shape_id(none), "hidden");
}
