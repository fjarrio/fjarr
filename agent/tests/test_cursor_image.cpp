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

// ------------------------------------------------- the hidden pointer (ADR-0041)
// spec: docs/22-remote-desktop-client.md#cursor-strategy · docs/23-agent-core-architecture.md#desktop-capture-memory

namespace {
CursorImage arrow(std::uint8_t shade = 200) {
    CursorImage c;
    c.width = 2;
    c.height = 1;
    c.rgba = {shade, shade, shade, 255, 0, 0, 0, 255};
    c.shape_id = shape_id(c);
    return c;
}
CursorImage empty_shape() {
    CursorImage c;
    c.visible = false;
    return c;
}
} // namespace

TEST(ReaderCursor, anInvalidIdHidesThePointerAndAValidIdWithoutABitmapBringsBackTheShapeBefore) {
    ReaderCursor r;
    auto a = r.bitmap(arrow());
    ASSERT_TRUE(a);
    EXPECT_TRUE(a->visible);
    EXPECT_FALSE(r.bitmap(arrow())) << "the same shape again is not news";
    auto hidden = r.invalid_id();
    ASSERT_TRUE(hidden);
    EXPECT_FALSE(hidden->visible);
    EXPECT_TRUE(hidden->invalid_id);
    EXPECT_EQ(hidden->shape_id, shape_id(empty_shape()));
    EXPECT_FALSE(r.invalid_id()) << "hidden already";
    // mutter sends a bitmap only when the shape changes: back without one, the arrow is in force.
    auto back = r.no_bitmap();
    ASSERT_TRUE(back);
    EXPECT_TRUE(back->visible);
    EXPECT_EQ(back->shape_id, a->shape_id);
    EXPECT_FALSE(r.no_bitmap()) << "shown already";
}

TEST(ReaderCursor, muttersEmptyShapeStaysInForceAcrossAnInvalidId) {
    ReaderCursor r;
    ASSERT_TRUE(r.bitmap(arrow()));
    auto empty = r.bitmap(empty_shape());
    ASSERT_TRUE(empty);
    EXPECT_FALSE(empty->visible);
    EXPECT_FALSE(empty->invalid_id) << "mutter's own empty shape, not an invalid id";
    EXPECT_FALSE(r.invalid_id()) << "hidden already: nothing to send";
    EXPECT_FALSE(r.no_bitmap()) << "the empty shape is still mutter's: the arrow must not come back";
}

TEST(ReaderCursor, withNoShapeSeenYetAValidIdWithoutABitmapSendsNothing) {
    ReaderCursor r;
    auto hidden = r.invalid_id(); // a capture that started with the pointer elsewhere
    ASSERT_TRUE(hidden);
    EXPECT_TRUE(hidden->invalid_id);
    EXPECT_FALSE(r.no_bitmap()) << "no shape to bring back";
    auto a = r.bitmap(arrow());
    ASSERT_TRUE(a);
    EXPECT_TRUE(a->visible);
}

TEST(CursorArbiter, anInvalidIdFromAMonitorThePointerIsNotOnIsIgnored) {
    CursorArbiter arb;
    ReaderCursor left, right;
    ASSERT_TRUE(arb.on_shape(1, *left.bitmap(arrow())));
    EXPECT_FALSE(arb.on_position(1)) << "the pointer on 1: the shape shown is already 1's";
    EXPECT_FALSE(arb.on_shape(2, *right.invalid_id())) << "2 sees an invalid id because the pointer is on 1";
    ASSERT_TRUE(arb.current());
    EXPECT_TRUE(arb.current()->visible);
}

TEST(CursorArbiter, anInvalidIdFromThePointersMonitorHidesIt) {
    CursorArbiter arb;
    ReaderCursor r;
    arb.on_shape(1, *r.bitmap(arrow()));
    arb.on_position(1);
    auto shown = arb.on_shape(1, *r.invalid_id()); // the application hid the pointer
    ASSERT_TRUE(shown);
    EXPECT_FALSE(shown->visible);
    EXPECT_FALSE(arb.current()->visible);
}

TEST(CursorArbiter, thePointerArrivingOnAMonitorBringsBackThatMonitorsShape) {
    CursorArbiter arb;
    ReaderCursor one, two;
    arb.on_shape(1, *one.bitmap(arrow(200)));
    arb.on_position(1);
    arb.on_shape(2, *two.bitmap(arrow(90))); // 2's reader saw a different shape earlier
    arb.on_shape(1, *one.invalid_id());      // the pointer leaves 1 ...
    auto there = arb.on_position(2);         // ... and arrives on 2
    ASSERT_TRUE(there);
    EXPECT_EQ(there->shape_id, arrow(90).shape_id);
    EXPECT_FALSE(arb.on_position(2)) << "moving within a monitor brings nothing";
    // Back on 1, whose reader restores its shape (a valid id without a bitmap).
    auto restored = one.no_bitmap();
    ASSERT_TRUE(restored);
    auto back = arb.on_shape(1, *restored);
    ASSERT_TRUE(back);
    EXPECT_EQ(back->shape_id, arrow(200).shape_id);
}

TEST(CursorArbiter, arrivingOnAMonitorWhoseLastShapeCameFromAnInvalidIdKeepsTheVisibleShape) {
    // A capture started while the pointer was on another monitor: its reader's only shape is the
    // hidden one from an invalid id. The pointer then enters with a valid id and no bitmap, and the
    // reader has nothing to restore: the operator must not lose a pointer that is visible.
    CursorArbiter arb;
    ReaderCursor one, two;
    arb.on_shape(1, *one.bitmap(arrow()));
    arb.on_position(1);
    arb.on_shape(2, *two.invalid_id());
    EXPECT_FALSE(two.no_bitmap());
    EXPECT_FALSE(arb.on_position(2)) << "the stale hidden shape was restored";
    ASSERT_TRUE(arb.current());
    EXPECT_TRUE(arb.current()->visible);
}

TEST(CursorArbiter, aStoppedCaptureIsForgottenAndSoIsThePointerBeingThere) {
    CursorArbiter arb;
    ReaderCursor one, two;
    arb.on_shape(1, *one.bitmap(arrow()));
    arb.on_position(1);
    arb.forget(1);
    // With nowhere known for the pointer, a hidden shape counts from any monitor again.
    auto hidden = arb.on_shape(2, *two.invalid_id());
    ASSERT_TRUE(hidden);
    EXPECT_FALSE(hidden->visible);
    EXPECT_FALSE(arb.on_position(1)) << "1's shape was forgotten with its capture";
}
