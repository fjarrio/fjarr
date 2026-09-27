// The robot never asks its router to open ports: libnice's UPnP-IGD is off on every webrtcbin
// the agent builds (docs/23 ICE). A regression, because libnice's default is on and nothing in a
// working session would reveal it — only a gateway's port-mapping table would.
#include <gtest/gtest.h>
#include <gst/gst.h>

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
