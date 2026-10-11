// docs/23#desktop-helper-protocol (Sound): the helper reads the default output from the session
// manager's `default` metadata (agent/desktop-session/sound.cpp).
#include <gtest/gtest.h>

#include "desktop/default_sink.hpp"

using fjarr::desktop::default_sink_name;

TEST(DefaultSink, theValueNamesTheSinksNode) {
    EXPECT_EQ(default_sink_name(R"({ "name": "alsa_output.pci-0000_00_1f.3.hdmi-stereo" })"), "alsa_output.pci-0000_00_1f.3.hdmi-stereo");
}

TEST(DefaultSink, aClearedOrMalformedValueNamesNone) {
    EXPECT_EQ(default_sink_name(nullptr), ""); // the key was removed
    EXPECT_EQ(default_sink_name(""), "");
    EXPECT_EQ(default_sink_name("alsa_output.pci"), "");         // not JSON
    EXPECT_EQ(default_sink_name(R"({"name": 3})"), "");          // not a name
    EXPECT_EQ(default_sink_name(R"(["alsa_output.pci"])"), "");  // not an object
    EXPECT_EQ(default_sink_name(R"({"other": "x"})"), "");
}
