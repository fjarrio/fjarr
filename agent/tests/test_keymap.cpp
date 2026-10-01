// docs/08 `text` and `key`: text is typed through the robot's own keymap, keys are physical.
// spec: docs/23-agent-core-architecture.md#desktop-helper-protocol ("Typing text")
#include <gtest/gtest.h>

#include <linux/input-event-codes.h>

#include "desktop/keycodes.hpp"
#include "desktop/keymap.hpp"

using namespace fjarr::desktop;

namespace {
std::unique_ptr<KeymapIndex> layout(const char* name) {
    std::string err;
    auto k = KeymapIndex::from_names(name, "", &err);
    EXPECT_TRUE(k) << err;
    return k;
}
} // namespace

TEST(Keymap, aSwedishKeymapTypesAaOAsTheirOwnKeys) {
    auto se = layout("se");
    ASSERT_TRUE(se);
    // å ä ö sit right of P and L: [ ; ' on a US keyboard.
    EXPECT_EQ(se->lookup(U'å')->key, KEY_LEFTBRACE);
    EXPECT_EQ(se->lookup(U'ä')->key, KEY_APOSTROPHE);
    EXPECT_EQ(se->lookup(U'ö')->key, KEY_SEMICOLON);
    EXPECT_TRUE(se->lookup(U'å')->modifiers.empty());
    EXPECT_EQ(se->lookup(U'Å')->modifiers, std::vector<std::uint16_t>{KEY_LEFTSHIFT});
}

TEST(Keymap, levelsNeedTheirModifiersShiftOnUsAltGrOnSwedish) {
    auto us = layout("us");
    auto se = layout("se");
    ASSERT_TRUE(us && se);
    EXPECT_EQ(us->lookup(U'a')->key, KEY_A);
    EXPECT_TRUE(us->lookup(U'a')->modifiers.empty());
    EXPECT_EQ(us->lookup(U'A')->modifiers, std::vector<std::uint16_t>{KEY_LEFTSHIFT});
    EXPECT_EQ(us->lookup(U'@')->key, KEY_2);
    EXPECT_EQ(us->lookup(U'@')->modifiers, std::vector<std::uint16_t>{KEY_LEFTSHIFT});
    // AltGr+2 on a Swedish keyboard, and AltGr is the right Alt key.
    EXPECT_EQ(se->lookup(U'@')->key, KEY_2);
    EXPECT_EQ(se->lookup(U'@')->modifiers, std::vector<std::uint16_t>{KEY_RIGHTALT});
    EXPECT_EQ(us->lookup(U'\n')->key, KEY_ENTER);
    EXPECT_EQ(us->lookup(U' ')->key, KEY_SPACE);
}

TEST(Keymap, aCharacterTheLayoutLacksIsNotTypable) {
    auto us = layout("us");
    ASSERT_TRUE(us);
    EXPECT_FALSE(us->lookup(U'å'));
    EXPECT_FALSE(us->lookup(U'Δ'));
    EXPECT_FALSE(KeymapIndex::from_string("not a keymap"));
}

TEST(Keymap, utf8RoundTripsAndInvalidBytesBecomeReplacement) {
    EXPECT_EQ(decode_utf8("aåä€😀"), (std::vector<char32_t>{U'a', U'å', U'ä', U'€', U'😀'}));
    EXPECT_EQ(decode_utf8("a\xff" "b"), (std::vector<char32_t>{U'a', 0xFFFD, U'b'}));
    EXPECT_EQ(decode_utf8("\xc3"), (std::vector<char32_t>{0xFFFD}));
    for (const char32_t cp : {U'a', U'å', U'€', U'😀'}) EXPECT_EQ(decode_utf8(encode_utf8(cp)), std::vector<char32_t>{cp});
}

TEST(Keycodes, physicalKeysMapToEvdevAndUnknownCodesDoNot) {
    EXPECT_EQ(evdev_for_code("KeyA"), KEY_A);
    EXPECT_EQ(evdev_for_code("Semicolon"), KEY_SEMICOLON); // ö on a Swedish robot: the robot's layout decides
    EXPECT_EQ(evdev_for_code("IntlBackslash"), KEY_102ND);
    EXPECT_EQ(evdev_for_code("AltRight"), KEY_RIGHTALT);
    EXPECT_EQ(evdev_for_code("MetaLeft"), KEY_LEFTMETA);
    EXPECT_FALSE(evdev_for_code("NotAKey"));
    EXPECT_FALSE(evdev_for_code(""));
}
