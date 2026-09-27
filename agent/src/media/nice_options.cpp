#include "nice_options.hpp"

#include "core/glib/raii.hpp"

namespace fjarr::media {

namespace {

bool has_property(GObject* obj, const char* name) {
    return obj && g_object_class_find_property(G_OBJECT_GET_CLASS(obj), name);
}

/// The libnice agent under webrtcbin's ICE agent, or null. g_object_get hands back references.
glib::GObjectPtr<GObject> nice_agent_of(GstElement* webrtcbin) {
    if (!has_property(G_OBJECT(webrtcbin), "ice-agent")) return {};
    GObject* ice = nullptr;
    g_object_get(webrtcbin, "ice-agent", &ice, nullptr);
    glib::GObjectPtr<GObject> ice_ref(ice);
    if (!has_property(ice, "agent")) return {};
    GObject* agent = nullptr;
    g_object_get(ice, "agent", &agent, nullptr);
    glib::GObjectPtr<GObject> agent_ref(agent);
    if (!has_property(agent, "upnp")) return {};
    return agent_ref;
}

} // namespace

bool disable_upnp(GstElement* webrtcbin) {
    auto agent = nice_agent_of(webrtcbin);
    if (!agent) return false;
    g_object_set(agent.get(), "upnp", FALSE, nullptr);
    return true;
}

std::optional<bool> upnp_enabled(GstElement* webrtcbin) {
    auto agent = nice_agent_of(webrtcbin);
    if (!agent) return std::nullopt;
    gboolean on = FALSE;
    g_object_get(agent.get(), "upnp", &on, nullptr);
    return on != FALSE;
}

} // namespace fjarr::media
