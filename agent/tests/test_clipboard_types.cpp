// The clipboard's type names (docs/23#desktop-helper-protocol, Clipboard; spike 2026-10-03).
#include <gtest/gtest.h>

#include <algorithm>
#include <string>

#include "desktop/clipboard_types.hpp"

using namespace fjarr::desktop::clipboard;

TEST(ClipboardTypes, anyTextNameTheRobotOffersReadsAsTextPlain) {
    // What wl-copy offered in the spike: X11 atoms and both MIME spellings.
    EXPECT_EQ(fjarr_types({"UTF8_STRING", "STRING", "TEXT", "text/plain;charset=utf-8", "text/plain"}), std::vector<std::string>{"text/plain"});
    EXPECT_EQ(fjarr_types({"STRING"}), std::vector<std::string>{"text/plain"}); // an X11 app via Xwayland
    EXPECT_TRUE(fjarr_types({"image/png"}).empty());                          // images come later
    EXPECT_TRUE(fjarr_types({}).empty());                                     // cleared
}

TEST(ClipboardTypes, readingPrefersUtf8ByName) {
    EXPECT_EQ(compositor_type_for("text/plain", {"STRING", "text/plain", "text/plain;charset=utf-8"}), "text/plain;charset=utf-8");
    EXPECT_EQ(compositor_type_for("text/plain", {"STRING", "UTF8_STRING"}), "UTF8_STRING");
    EXPECT_EQ(compositor_type_for("text/plain", {"image/png"}), "");
    EXPECT_EQ(compositor_type_for("image/png", {"image/png"}), ""); // not a type Fjarr reads yet
}

TEST(ClipboardTypes, textIsOfferedUnderEveryNameAnAppMayAskFor) {
    const auto& a = text_aliases();
    for (const char* name : {"text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "STRING", "TEXT"})
        EXPECT_NE(std::find(a.begin(), a.end(), std::string(name)), a.end()) << name; // offering fewer, wl-paste found nothing
}
