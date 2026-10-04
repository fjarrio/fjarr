// A monitor's identity from its EDID (docs/23#desktop-x11): backend A reads EDIDs from RandR and must
// give a monitor the wire id backend E (mutter) gives it. Real EDIDs from the lab's mini-PC.
#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "desktop/edid.hpp"
#include "desktop/monitor_identity.hpp"

namespace {
std::vector<std::uint8_t> hex(const std::string& h) {
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i + 1 < h.size(); i += 2) out.push_back(static_cast<std::uint8_t>(std::stoi(h.substr(i, 2), nullptr, 16)));
    return out;
}
// One of the three Dells on the mini-PC's DisplayPort chain (DP-4), 2026-10-04.
const std::string DELL = "00ffffffffffff0010acb7a14c34443005210104a5351e783e9025ac524f9e250f5054a54b00714f8180a9c0d1c00101010101010101023a801871382d40582c45000f282100001e000000ff0047324b4a5250330a2020202020000000fc0044454c4c205532343232480a20000000fd00384c1e5311010a20202020202001a8020315f14890040302011f12132309070783010000023a801871382d40582c45000f282100001e011d007251d01e20462855000f282100001e8c0ad08a20e02d10103e96000f2821000018000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000003f";
// The ghost screen's EDID that fjarr-agent display add-ghost generated (ADR-0032).
const std::string GHOST = "00ffffffffffff0019520147010000000024010480331d7806ee91a3544c99260f5054000000010101010101010101010101010101011a3680a070381f4030203500fe221100001a000000fd003b3d41430e010a202020202020000000fc00466a6172722047686f73742031000000ff00464a47484f5354310a202020200048";
} // namespace

TEST(Edid, aDellGetsTheWireIdMutterGaveIt) {
    const auto e = hex(DELL);
    const auto k = fjarr::desktop::edid_key(e.data(), e.size(), "DisplayPort-4");
    EXPECT_EQ(k.vendor, "DEL");
    EXPECT_EQ(k.model, "DELL U2422H");
    const std::string id = fjarr::desktop::wire_ids({k})[0];
    // GNOME named the chain's three Dells these (the agent's log on the mini-PC, 2026-10-04).
    const std::vector<std::string> gnome{"del-dell-u2422h-3529rp3", "del-dell-u2422h-g2kjrp3", "del-dell-u2422h-gk19rp3"};
    EXPECT_NE(std::find(gnome.begin(), gnome.end(), id), gnome.end()) << id;
}

TEST(Edid, theGhostScreenGetsItsGnomeIdToo) {
    const auto e = hex(GHOST);
    const auto k = fjarr::desktop::edid_key(e.data(), e.size(), "HDMI-1");
    EXPECT_EQ(fjarr::desktop::wire_ids({k})[0], "fjr-fjarr-ghost-1-fjghost1"); // the mini-PC's ghost under GNOME
}

TEST(Edid, notAnEdidLeavesOnlyTheConnector) {
    const std::vector<std::uint8_t> junk(128, 0x42);
    const auto k = fjarr::desktop::edid_key(junk.data(), junk.size(), "LEFT");
    EXPECT_TRUE(k.vendor.empty() && k.model.empty() && k.serial.empty());
    EXPECT_EQ(k.connector, "LEFT");
    EXPECT_EQ(fjarr::desktop::edid_key(nullptr, 0, "X").connector, "X");
}
