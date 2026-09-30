#pragma once
// libnice options webrtcbin does not expose itself, reached through its ICE agent's `agent`
// property (GStreamer 1.28's GstWebRTCNice).
// spec: docs/23-agent-core-architecture.md — ICE: the agent never asks the robot's router to open ports.
#include <optional>
#include <string>
#include <vector>

#include <sys/socket.h>

#include <gst/gst.h>

namespace fjarr::media {

/// Turns libnice's UPnP-IGD off for this webrtcbin, before it gathers. Left on, libnice asks the
/// robot's gateway to map ports for every host candidate — on a customer's network, a robot opening
/// its router is not Fjarr's to decide, and TURN is the path for what is not reachable directly.
/// False when this webrtcbin's ICE agent is not libnice or has no `upnp` property (logged by the caller).
bool disable_upnp(GstElement* webrtcbin);

/// Whether UPnP-IGD is on for this webrtcbin's libnice agent; nullopt when it is not libnice.
std::optional<bool> upnp_enabled(GstElement* webrtcbin);

/// One local interface address, as getifaddrs reports it.
struct InterfaceAddress {
    std::string interface;
    bool up = false;
    bool loopback = false;
    sockaddr_storage address{};
};

/// Whether ICE may never gather on this interface: a Fjarr tunnel — any name starting `fjarr`, or
/// one of `excluded` (fjarr.net's configured interface). spec: docs/23 ICE, #34.
bool is_tunnel_interface(const std::string& name, const std::vector<std::string>& excluded);

/// The addresses ICE gathers from: up, not loopback, IPv4 or IPv6, not on a tunnel interface.
std::vector<InterfaceAddress> gatherable_addresses(const std::vector<InterfaceAddress>& all, const std::vector<std::string>& excluded);

/// Every local interface address (getifaddrs).
std::vector<InterfaceAddress> local_interface_addresses();

/// Gives this webrtcbin's libnice agent an explicit list of local addresses — what it would find
/// itself, minus the tunnel interfaces — so a Fjarr tunnel never carries the link it is carried by
/// (docs/23 ICE, #34). Must run before gathering. Returns how many addresses were given; 0 means
/// libnice was left to gather as it would have (nothing left after the exclusion, or not libnice).
std::size_t restrict_gathering(GstElement* webrtcbin, const std::vector<InterfaceAddress>& local,
                               const std::vector<std::string>& excluded);

} // namespace fjarr::media
