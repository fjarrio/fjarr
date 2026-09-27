#pragma once
// libnice options webrtcbin does not expose itself, reached through its ICE agent's `agent`
// property (GStreamer 1.28's GstWebRTCNice).
// spec: docs/23-agent-core-architecture.md — ICE: the agent never asks the robot's router to open ports.
#include <optional>

#include <gst/gst.h>

namespace fjarr::media {

/// Turns libnice's UPnP-IGD off for this webrtcbin, before it gathers. Left on, libnice asks the
/// robot's gateway to map ports for every host candidate — on a customer's network, a robot opening
/// its router is not Fjarr's to decide, and TURN is the path for what is not reachable directly.
/// False when this webrtcbin's ICE agent is not libnice or has no `upnp` property (logged by the caller).
bool disable_upnp(GstElement* webrtcbin);

/// Whether UPnP-IGD is on for this webrtcbin's libnice agent; nullopt when it is not libnice.
std::optional<bool> upnp_enabled(GstElement* webrtcbin);

} // namespace fjarr::media
