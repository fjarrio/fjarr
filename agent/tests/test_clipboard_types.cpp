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
    EXPECT_EQ(fjarr_types({"image/png"}), std::vector<std::string>{"image/png"}); // a copied image (M3 3.5)
    EXPECT_EQ(fjarr_types({"image/png", "text/plain"}), (std::vector<std::string>{"text/plain", "image/png"}));
    EXPECT_TRUE(fjarr_types({"image/jpeg", "text/uri-list"}).empty()); // other images, and files (fjarr.files, M4)
    EXPECT_TRUE(fjarr_types({}).empty());                                     // cleared
}

TEST(ClipboardTypes, readingPrefersUtf8ByName) {
    EXPECT_EQ(compositor_type_for("text/plain", {"STRING", "text/plain", "text/plain;charset=utf-8"}), "text/plain;charset=utf-8");
    EXPECT_EQ(compositor_type_for("text/plain", {"STRING", "UTF8_STRING"}), "UTF8_STRING");
    EXPECT_EQ(compositor_type_for("text/plain", {"image/png"}), "");
    EXPECT_EQ(compositor_type_for("image/png", {"image/png"}), "image/png");
    EXPECT_EQ(compositor_type_for("image/png", {"text/plain"}), "");
}

TEST(ClipboardTypes, textIsOfferedUnderEveryNameAnAppMayAskFor) {
    const auto& a = text_aliases();
    for (const char* name : {"text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "STRING", "TEXT"})
        EXPECT_NE(std::find(a.begin(), a.end(), std::string(name)), a.end()) << name; // offering fewer, wl-paste found nothing
}

TEST(ClipboardTypes, eachTypeHasItsNamesAndItsLimit) {
    // docs/08: 1 MiB of text, 8 MiB of PNG; a PNG is offered as image/png only.
    EXPECT_EQ(compositor_names(TEXT), text_aliases());
    EXPECT_EQ(compositor_names(PNG), std::vector<std::string>{"image/png"});
    EXPECT_TRUE(compositor_names("image/jpeg").empty());
    EXPECT_EQ(max_bytes(TEXT), 1024u * 1024u);
    EXPECT_EQ(max_bytes(PNG), 8u * 1024u * 1024u);
    EXPECT_EQ(max_bytes("image/jpeg"), 0u);
}
