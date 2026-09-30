// docs/08#track-manifest: a monitor is who its EDID says, not where it is plugged in.
#include <gtest/gtest.h>

#include "desktop/monitor_identity.hpp"

using namespace fjarr::desktop;

TEST(MonitorIdentity, theEdidSlugIsTheSpecsExample) {
    EXPECT_EQ(identity_slug("DEL", "DELL U2720Q", "8XK2N13"), "del-dell-u2720q-8xk2n13");
    EXPECT_EQ(identity_slug("GSM", "LG  ULTRAFINE__", "  "), "gsm-lg-ultrafine");
}

TEST(MonitorIdentity, noSerialOrACollisionFallsBackToTheConnectorAndVirtualIsNumbered) {
    const std::vector<MonitorKey> set = {
        {"DEL", "DELL U2422H", "GK19RP3", "DP-4", false},
        {"GSM", "LG TV", "", "HDMI-A-1", false},           // no serial
        {"FJR", "Fjarr Ghost 1", "FJ0001", "DP-2", false}, // a ghost screen: its own identity
        {"ACR", "TWIN", "SAME", "DP-5", false},             // two monitors claiming one serial
        {"ACR", "TWIN", "SAME", "DP-6", false},
        {"MetaVendor", "MetaVirtualMonitor", "0x00", "Meta-0", true},
    };
    EXPECT_EQ(wire_ids(set), (std::vector<std::string>{"del-dell-u2422h-gk19rp3", "hdmi-a-1", "fjr-fjarr-ghost-1-fj0001", "dp-5", "dp-6", "virtual-1"}));
}
