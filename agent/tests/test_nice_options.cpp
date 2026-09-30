// The robot never asks its router to open ports: libnice's UPnP-IGD is off on every webrtcbin
// the agent builds (docs/23 ICE). A regression, because libnice's default is on and nothing in a
// working session would reveal it — only a gateway's port-mapping table would.
#include <gtest/gtest.h>
#include <gst/gst.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include "core/glib/raii.hpp"
#include "media/nice_options.hpp"

using namespace fjarr;

TEST(NiceOptions, libniceDefaultsToUpnpOnWhichIsWhyTheAgentMustTurnItOff) {
    auto webrtc = glib::make_element("webrtcbin", "upnp-default");
    ASSERT_TRUE(webrtc) << "webrtcbin missing (gstreamer1.0-nice?)";
    auto on = media::upnp_enabled(webrtc.get());
    ASSERT_TRUE(on.has_value()) << "webrtcbin's ICE agent is no longer libnice: revisit docs/23 ICE";
    EXPECT_TRUE(*on) << "libnice's default changed; the explicit off is still right, this test is not";
}

TEST(NiceOptions, disableUpnpTurnsItOff) {
    auto webrtc = glib::make_element("webrtcbin", "upnp-off");
    ASSERT_TRUE(webrtc);
    ASSERT_TRUE(media::disable_upnp(webrtc.get()));
    EXPECT_EQ(media::upnp_enabled(webrtc.get()), std::optional<bool>(false));
}

// #34 (docs/23 ICE): a Fjarr tunnel interface never carries ICE — the robot offered its tunnel
// address as a host candidate, a pair that works exactly while the link it would carry is up.
namespace {
media::InterfaceAddress v4(const char* iface, const char* ip, bool up = true, bool loopback = false) {
    media::InterfaceAddress a;
    a.interface = iface;
    a.up = up;
    a.loopback = loopback;
    auto* sin = reinterpret_cast<sockaddr_in*>(&a.address);
    sin->sin_family = AF_INET;
    inet_pton(AF_INET, ip, &sin->sin_addr);
    return a;
}
std::vector<std::string> names(const std::vector<media::InterfaceAddress>& v) {
    std::vector<std::string> out;
    for (const auto& a : v) out.push_back(a.interface);
    return out;
}
} // namespace

TEST(NiceOptions, tunnelInterfacesNeverGatherAndCarrierNatAddressesStillDo) {
    const std::vector<media::InterfaceAddress> all = {
        v4("lo", "127.0.0.1", true, true),        // loopback: never
        v4("enp3s0", "192.168.10.108"),           // the LAN
        v4("fjarr0", "100.77.176.22"),            // the tunnel (the mini-PC, 2026-09-30)
        v4("fjarr1", "100.64.0.1"),               // another link's tunnel on an operator host
        v4("robotnet", "100.70.1.1"),             // fjarr.net configured with its own name
        v4("wwan0", "100.72.3.4"),                // 4G carrier NAT: same range, a real path
        v4("wlan0", "10.0.0.2", false),           // down: never
    };
    EXPECT_EQ(names(media::gatherable_addresses(all, {"robotnet"})), (std::vector<std::string>{"enp3s0", "wwan0"}));
    EXPECT_TRUE(media::is_tunnel_interface("fjarr0", {}));
    EXPECT_FALSE(media::is_tunnel_interface("wwan0", {"fjarr0"}));
}

TEST(NiceOptions, restrictGatheringHandsLibniceTheAddressesAndLeavesItAloneWhenNoneRemain) {
    auto webrtc = glib::make_element("webrtcbin", "gather-restricted");
    ASSERT_TRUE(webrtc);
    EXPECT_EQ(media::restrict_gathering(webrtc.get(), {v4("eth0", "192.168.1.5"), v4("fjarr0", "100.77.1.1")}, {}), 1u);
    auto other = glib::make_element("webrtcbin", "gather-only-tunnel");
    ASSERT_TRUE(other);
    EXPECT_EQ(media::restrict_gathering(other.get(), {v4("fjarr0", "100.77.1.1")}, {}), 0u) << "nothing left: libnice gathers as it would have";
}
