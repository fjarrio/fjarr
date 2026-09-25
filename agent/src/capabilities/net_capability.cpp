// spec: docs/27-network-tunnel.md · docs/08-protocol.md#net-packets · docs/10-security.md#network-tunnel
#include <fjarr/net_capability.hpp>

#include <cerrno>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <fcntl.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <fjarr/errors.hpp>
#include <fjarr/session_context.hpp>

#include "capabilities/net_addressing.hpp"
#include "core/log.hpp"

namespace fjarr {
namespace {

constexpr std::size_t MAX_PACKET = 65536; // the read buffer, not the MTU: a short read is the norm
constexpr auto STATS_PERIOD = std::chrono::milliseconds(1000);

std::string short_id(const SessionId& id) { return id.size() > 8 ? id.substr(id.size() - 8) : id; }

/// The device's real MTU, which governs regardless of what the config believes (docs/27). 0 when
/// it cannot be read.
int device_mtu(const std::string& ifname) {
    const int s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return 0;
    struct ifreq req {};
    std::snprintf(req.ifr_name, IFNAMSIZ, "%s", ifname.c_str());
    const int rc = ::ioctl(s, SIOCGIFMTU, &req);
    ::close(s);
    return rc == 0 ? req.ifr_mtu : 0;
}

/// What `link-stats` reports once per second (docs/08#net-packets). The four drop counters are
/// what a support engineer reads first, which is why they are separate rather than one total.
struct Counters {
    unsigned long tx_packets = 0, rx_packets = 0;
    unsigned long long tx_bytes = 0, rx_bytes = 0;
    unsigned long dropped_no_peer = 0, dropped_policy = 0, dropped_queue = 0, dropped_mtu = 0;

    nlohmann::json to_json() const {
        return nlohmann::json{{"interval_ms", STATS_PERIOD.count()}, {"tx_packets", tx_packets},   {"rx_packets", rx_packets},
                              {"tx_bytes", tx_bytes},                {"rx_bytes", rx_bytes},       {"dropped_no_peer", dropped_no_peer},
                              {"dropped_policy", dropped_policy},    {"dropped_queue", dropped_queue}, {"dropped_mtu", dropped_mtu}};
    }
};

} // namespace

struct NetCapability::Impl {
    std::string robot_id;
    bool enabled = false;
    std::string ifname = "fjarr0";
    net::Range range{};
    std::uint32_t self_addr = 0; // this robot
    std::uint32_t peer_addr = 0; // the operator, the same on every link
    int mtu = 1280;
    std::vector<std::uint16_t> allow_ports;
    std::string attach_error; // why the device could not be attached to

    /// Open for the agent's whole life, not per link: attaching is what raises the interface's
    /// carrier, and a participant created while the carrier is down ignores the interface
    /// permanently (docs/27#lifecycle, measured). Closing this between sessions would silently
    /// break every ROS 2 node that started before the operator connected.
    int tun_fd = -1;

    std::map<SessionId, SessionContext*> sessions;
    /// One link at a time: the robot's device is point-to-point with exactly one peer.
    SessionId link_session;
    std::unique_ptr<FdWatch> watch;
    std::unique_ptr<Timer> stats_timer;
    Counters counters;

    bool link_open() const { return !link_session.empty(); }

    /// Why a link cannot be opened here, or empty when it can. Availability is state, not an
    /// exception: a robot without the tunnel set up is a deployment choice, not a fault.
    std::string unavailable_reason() const {
        if (!enabled) return "the network tunnel is not enabled on this robot (capabilities.\"fjarr.net\".enabled)";
        if (tun_fd < 0) return attach_error;
        return {};
    }

    /// Attach to the interface the installer created. Never creates it: creating one needs
    /// CAP_NET_ADMIN, and the whole point of the persistent device is that the agent has none
    /// (docs/27#lifecycle).
    void attach_device() {
        const int fd = ::open("/dev/net/tun", O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            attach_error = std::string("/dev/net/tun cannot be opened: ") + std::strerror(errno) +
                           " — the tunnel needs the tun module and read access to the device node (docs/26)";
            return;
        }
        struct ifreq req {};
        req.ifr_flags = IFF_TUN | IFF_NO_PI; // raw IP packets, no 4-byte protocol prefix
        std::snprintf(req.ifr_name, IFNAMSIZ, "%s", ifname.c_str());
        if (::ioctl(fd, TUNSETIFF, &req) < 0) {
            const int err = errno;
            ::close(fd);
            attach_error = "cannot attach to '" + ifname + "': " + std::strerror(err) +
                           " — the interface must exist and be owned by this user before the agent starts (`ip tuntap add " + ifname +
                           " mode tun user <agent>`, docs/26)";
            return;
        }
        tun_fd = fd;
        attach_error.clear();
        const int real = device_mtu(ifname);
        if (real > 0 && real != mtu) {
            // The device's MTU is the one that governs: the kernel will not hand us a larger
            // packet, and the peer must agree or it fragments. Follow it and say so.
            log::warn("net", "interface MTU differs from the configured one, following the interface",
                      {{"interface", ifname}, {"configured", std::to_string(mtu)}, {"interface_mtu", std::to_string(real)}});
            mtu = real;
        }
        log::info("net", "attached to the tunnel interface",
                  {{"interface", ifname},
                   {"address", net::to_dotted(self_addr)},
                   {"peer", net::to_dotted(peer_addr)},
                   {"mtu", std::to_string(mtu)}});
    }

    /// Everything the kernel queued while nobody was forwarding, discarded before the first
    /// forwarded packet. A link that opened onto a backlog would deliver a burst of stale packets,
    /// which is the thing the whole no-queue rule exists to prevent (docs/27).
    void drain_stale() {
        if (tun_fd < 0) return;
        std::string buf(MAX_PACKET, '\0');
        unsigned long n = 0;
        while (::read(tun_fd, buf.data(), buf.size()) > 0) {
            n++;
            counters.dropped_no_peer++;
        }
        if (n) log::debug("net", "discarded packets queued before the link opened", {{"packets", std::to_string(n)}});
    }

    void close_link();
};

void NetCapability::Impl::close_link() {
    if (!link_open()) return;
    const SessionId id = link_session;
    link_session.clear();
    watch.reset();
    stats_timer.reset();
    log::info("net", "link closed",
              {{"session", short_id(id)},
               {"tx_packets", std::to_string(counters.tx_packets)},
               {"rx_packets", std::to_string(counters.rx_packets)},
               {"dropped_policy", std::to_string(counters.dropped_policy)},
               {"dropped_queue", std::to_string(counters.dropped_queue)}});
}

NetCapability::NetCapability(std::string robot_id) : impl_(std::make_unique<Impl>()) { impl_->robot_id = std::move(robot_id); }
NetCapability::~NetCapability() {
    if (impl_->tun_fd >= 0) ::close(impl_->tun_fd);
}

CapabilityManifest NetCapability::manifest() const {
    CapabilityManifest m;
    m.name = "fjarr.net";
    m.version = {0, 1, 0};
    // Stream, not bulk: the traffic inside carries its own recovery, and an outer retransmission
    // would fight TCP's and lose on a bad link (docs/08#net-packets).
    m.channels = {{ChannelClass::Control}, {ChannelClass::Stream, BulkFraming::Raw}};
    m.consumers.peer = true;
    // docs/10: granting `net` is granting network access to the robot from inside, so it takes the
    // ownership lease and is released first on every detach path, like the terminal.
    m.input_bearing = true;
    m.config_schema = nlohmann::json{
        {"type", "object"},
        {"additionalProperties", false},
        {"properties",
         {{"enabled", {{"type", "boolean"}}},
          {"interface", {{"type", "string"}, {"description", "the persistent device the installer created; attached to, never created"}}},
          {"range", {{"type", "string"}, {"description", "both ends must agree; default 100.64.0.0/10"}}},
          {"address", {{"type", "string"}, {"description", "\"auto\" derives from the robot id (docs/27#addressing)"}}},
          {"mtu", {{"type", "integer"}, {"minimum", 576}, {"maximum", 9000}}},
          {"allow_ports",
           {{"type", "array"},
            {"items", {{"type", "integer"}, {"minimum", 1}, {"maximum", 65535}}},
            {"description", "empty = every port on this robot's own tunnel address"}}}}}};
    return m;
}

namespace {
/// Range, this robot's address and the operator's, from config alone (docs/27#addressing).
struct Addressing {
    net::Range range;
    std::uint32_t self = 0, peer = 0;
};
Addressing resolve_addressing(const std::string& robot_id, const nlohmann::json& c) {
    const std::string range_s = c.value("range", std::string{"100.64.0.0/10"});
    const auto range = net::parse_range(range_s);
    if (!range) throw FjarrError("config", "fjarr.net: range is not a CIDR block between /8 and /30: " + range_s);
    Addressing a{*range, 0, net::operator_address(*range)};
    const std::string addr = c.value("address", std::string{"auto"});
    if (addr == "auto") {
        a.self = net::derive_address(robot_id, *range);
        return a;
    }
    const auto pinned = net::parse_address(addr);
    if (!pinned) throw FjarrError("config", "fjarr.net: address is neither \"auto\" nor an IPv4 address: " + addr);
    if (!range->contains(*pinned)) throw FjarrError("config", "fjarr.net: address " + addr + " is outside range " + range_s);
    if (*pinned == a.peer) throw FjarrError("config", "fjarr.net: address " + addr + " is the operator's own address (docs/27#addressing)");
    a.self = *pinned;
    return a;
}
} // namespace

std::string NetCapability::address_from_config(const std::string& robot_id, const nlohmann::json& config) {
    return net::to_dotted(resolve_addressing(robot_id, config).self);
}

void NetCapability::configure(const nlohmann::json& c, const SourceFactory&) {
    impl_->enabled = c.value("enabled", false);
    impl_->ifname = c.value("interface", std::string{"fjarr0"});
    impl_->mtu = c.value("mtu", 1280);
    const Addressing a = resolve_addressing(impl_->robot_id, c);
    impl_->range = a.range;
    impl_->self_addr = a.self;
    impl_->peer_addr = a.peer;
    if (c.contains("allow_ports"))
        for (const auto& p : c.at("allow_ports")) impl_->allow_ports.push_back(static_cast<std::uint16_t>(p.get<int>()));

    if (impl_->enabled) impl_->attach_device();
    if (impl_->enabled && impl_->tun_fd < 0) log::warn("net", "enabled but not attached", {{"reason", impl_->attach_error}});
}

void NetCapability::test_attach_fd(int fd) {
    if (impl_->tun_fd >= 0) ::close(impl_->tun_fd);
    impl_->tun_fd = fd;
    impl_->enabled = true;
    impl_->attach_error.clear();
}

std::string NetCapability::address() const { return impl_->self_addr ? net::to_dotted(impl_->self_addr) : std::string{}; }

void NetCapability::session_attached(SessionContext& ctx, const nlohmann::json&) { impl_->sessions[ctx.id()] = &ctx; }

void NetCapability::session_detached(const SessionId& id, DetachReason, std::string_view) {
    if (impl_->link_session == id) impl_->close_link();
    impl_->sessions.erase(id);
}

void NetCapability::release_all_input(const SessionId& id) {
    if (impl_->link_session == id) impl_->close_link();
}

void NetCapability::on_binary(SessionContext& ctx, std::span<const std::byte> bytes) {
    // Inbound, from the operator. Packets for a session with no link are dropped: `open` is what
    // takes the lease, so an operator who never opened one has no path in.
    if (impl_->link_session != ctx.id() || impl_->tun_fd < 0) {
        impl_->counters.dropped_no_peer++;
        return;
    }
    if (bytes.size() > static_cast<std::size_t>(impl_->mtu)) {
        impl_->counters.dropped_mtu++;
        return;
    }
    const auto view = net::inspect(bytes);
    const auto verdict = net::check(view, impl_->self_addr, impl_->peer_addr, impl_->allow_ports);
    if (verdict != net::Verdict::Allow) {
        // This is the rule that makes lateral movement into the robot's LAN impossible: a packet
        // addressed anywhere but this robot's own tunnel address never reaches the kernel.
        impl_->counters.dropped_policy++;
        log::debug("net", "inbound packet refused",
                   {{"session", short_id(ctx.id())},
                    {"reason", net::verdict_name(verdict)},
                    {"dst", net::to_dotted(view.dst)},
                    {"src", net::to_dotted(view.src)}});
        return;
    }
    if (::write(impl_->tun_fd, bytes.data(), bytes.size()) < 0) {
        impl_->counters.dropped_queue++;
        return;
    }
    impl_->counters.rx_packets++;
    impl_->counters.rx_bytes += bytes.size();
}

void NetCapability::on_message(SessionContext& ctx, const Envelope& msg) {
    if (msg.kind != "request") return;

    if (msg.type == "open") {
        if (impl_->link_open()) {
            ctx.fail(msg, error_codes::busy,
                     impl_->link_session == ctx.id() ? "this session already has the link open"
                                                     : "another session holds the link; the robot's interface is point-to-point");
            return;
        }
        const std::string why = impl_->unavailable_reason();
        if (!why.empty()) {
            ctx.fail(msg, error_codes::unavailable, why);
            return;
        }
        impl_->drain_stale();
        impl_->counters = Counters{};
        impl_->link_session = ctx.id();
        const SessionId sid = ctx.id();
        impl_->watch = ctx.watch_readable(impl_->tun_fd, [this, sid, ctxp = &ctx]() -> bool {
            if (impl_->link_session != sid) return false;
            // Drain what is ready: the loop wakes once per readable state, not once per packet.
            std::string buf(MAX_PACKET, '\0');
            for (;;) {
                const ssize_t n = ::read(impl_->tun_fd, buf.data(), buf.size());
                if (n <= 0) {
                    if (n < 0 && errno == EINTR) continue;
                    return true; // EAGAIN: nothing more for now
                }
                const auto size = static_cast<std::size_t>(n);
                const std::span<const std::byte> packet(reinterpret_cast<const std::byte*>(buf.data()), size);
                if (size > static_cast<std::size_t>(impl_->mtu)) {
                    impl_->counters.dropped_mtu++;
                    continue;
                }
                // Outbound: the mirror of the inbound rule — for the operator, from this robot.
                if (net::check(net::inspect(packet), impl_->peer_addr, impl_->self_addr, {}) != net::Verdict::Allow) {
                    impl_->counters.dropped_policy++;
                    continue;
                }
                if (!ctxp->stream().send_binary(packet)) {
                    // The channel is above its watermark. Drop rather than hold: a queue delivers
                    // a burst of stale packets after congestion, which damages the round-trip
                    // estimate of every connection inside the tunnel more than the loss does
                    // (docs/08#net-packets).
                    impl_->counters.dropped_queue++;
                    continue;
                }
                impl_->counters.tx_packets++;
                impl_->counters.tx_bytes += size;
            }
        });
        impl_->stats_timer = ctx.every(STATS_PERIOD, [this, sid, ctxp = &ctx]() -> bool {
            if (impl_->link_session != sid) return false;
            ctxp->event("link-stats", impl_->counters.to_json());
            return true;
        });
        log::info("net", "link opened",
                  {{"session", short_id(ctx.id())},
                   {"operator", ctx.operator_info().id},
                   {"address", net::to_dotted(impl_->self_addr)},
                   {"peer", net::to_dotted(impl_->peer_addr)}});
        ctx.result(msg, nlohmann::json{{"ok", true},
                                       {"address", net::to_dotted(impl_->self_addr)},
                                       {"peer_address", net::to_dotted(impl_->peer_addr)},
                                       {"mtu", impl_->mtu},
                                       {"policy", {{"forwarding", false}, {"allow_ports", impl_->allow_ports}}}});
        return;
    }

    if (msg.type == "close") {
        if (impl_->link_session != ctx.id()) {
            ctx.fail(msg, error_codes::unavailable, "this session has no open link");
            return;
        }
        impl_->close_link();
        // The interface itself stays: it is persistent by design, and taking it down would
        // silently break every participant bound to it (docs/27#lifecycle).
        ctx.result(msg, nlohmann::json{{"ok", true}});
        return;
    }

    ctx.fail(msg, error_codes::payload_invalid, "unknown fjarr.net request: " + msg.type);
}

void NetCapability::shutdown() {
    impl_->close_link();
    impl_->sessions.clear();
    if (impl_->tun_fd >= 0) {
        ::close(impl_->tun_fd);
        impl_->tun_fd = -1;
    }
}

} // namespace fjarr
