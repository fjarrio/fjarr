#include "nice_options.hpp"

#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>

#include <cstring>

#include <nice/agent.h>

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

bool is_tunnel_interface(const std::string& name, const std::vector<std::string>& excluded) {
    if (name.rfind("fjarr", 0) == 0) return true;
    for (const auto& e : excluded)
        if (!e.empty() && name == e) return true;
    return false;
}

std::vector<InterfaceAddress> gatherable_addresses(const std::vector<InterfaceAddress>& all, const std::vector<std::string>& excluded) {
    std::vector<InterfaceAddress> out;
    for (const auto& a : all) {
        const auto family = a.address.ss_family;
        if (!a.up || a.loopback || (family != AF_INET && family != AF_INET6)) continue;
        if (is_tunnel_interface(a.interface, excluded)) continue;
        out.push_back(a);
    }
    return out;
}

std::vector<InterfaceAddress> local_interface_addresses() {
    std::vector<InterfaceAddress> out;
    ifaddrs* list = nullptr;
    if (::getifaddrs(&list) != 0) return out;
    for (const ifaddrs* i = list; i; i = i->ifa_next) {
        if (!i->ifa_addr || !i->ifa_name) continue;
        const auto family = i->ifa_addr->sa_family;
        if (family != AF_INET && family != AF_INET6) continue;
        InterfaceAddress a;
        a.interface = i->ifa_name;
        a.up = (i->ifa_flags & IFF_UP) != 0;
        a.loopback = (i->ifa_flags & IFF_LOOPBACK) != 0;
        std::memcpy(&a.address, i->ifa_addr, family == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6));
        out.push_back(a);
    }
    ::freeifaddrs(list);
    return out;
}

std::size_t restrict_gathering(GstElement* webrtcbin, const std::vector<InterfaceAddress>& local,
                               const std::vector<std::string>& excluded) {
    // spec: docs/23-agent-core-architecture.md ICE — a Fjarr tunnel interface never carries ICE (#34).
    auto agent = nice_agent_of(webrtcbin);
    if (!agent) return 0;
    const auto keep = gatherable_addresses(local, excluded);
    // Nothing left: add nothing, and libnice gathers as it would have (a robot whose only
    // interface is the tunnel has no direct path either way; TURN still is one).
    if (keep.empty()) return 0;
    std::size_t added = 0;
    for (const auto& a : keep) {
        NiceAddress addr;
        nice_address_init(&addr);
        nice_address_set_from_sockaddr(&addr, reinterpret_cast<const sockaddr*>(&a.address));
        if (nice_agent_add_local_address(NICE_AGENT(agent.get()), &addr)) added++;
    }
    return added;
}

} // namespace fjarr::media
