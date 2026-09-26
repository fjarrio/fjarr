// fjarr.net: addressing, the two policy rules that make robot-to-robot isolation structural, the
// MTU bound and the tail-drop — the unit half of docs/27#testing. The packet pump runs against a
// socketpair, so no TUN device and no privileges are needed.
// spec: docs/27-network-tunnel.md · docs/08-protocol.md#net-packets
#include <array>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <fjarr/errors.hpp>
#include <fjarr/net_capability.hpp>

#include "capabilities/net_addressing.hpp"
#include "core/loop.hpp"
#include "media/sources.hpp"
#include "recording_context.hpp"

using namespace fjarr;
using fjarr::testing::RecordingContext;

namespace {

const net::Range CGNAT = *net::parse_range("100.64.0.0/10");

/// The tunnel ignores the source factory entirely; one still has to exist to call configure().
struct Sources {
    CoreLoop loop;
    media::SourceRegistry reg;
    Sources() : reg(loop.context()) {}
};

/// A minimal well-formed IPv4 packet: 20-byte header, TCP/UDP ports, padded to `size`.
std::string packet(const std::string &src, const std::string &dst, std::uint8_t proto = 17, std::uint16_t dst_port = 22,
                   std::size_t size = 40) {
    std::string p(std::max<std::size_t>(size, 28), '\0');
    auto *b = reinterpret_cast<std::uint8_t *>(p.data());
    b[0] = 0x45; // IPv4, 5 words of header
    b[2] = static_cast<std::uint8_t>(p.size() >> 8);
    b[3] = static_cast<std::uint8_t>(p.size() & 0xff);
    b[9] = proto;
    const std::uint32_t s = *net::parse_address(src), d = *net::parse_address(dst);
    for (int i = 0; i < 4; i++) {
        b[12 + i] = static_cast<std::uint8_t>((s >> (24 - 8 * i)) & 0xff);
        b[16 + i] = static_cast<std::uint8_t>((d >> (24 - 8 * i)) & 0xff);
    }
    b[22] = static_cast<std::uint8_t>(dst_port >> 8);
    b[23] = static_cast<std::uint8_t>(dst_port & 0xff);
    return p;
}

std::span<const std::byte> bytes_of(const std::string &s) { return {reinterpret_cast<const std::byte *>(s.data()), s.size()}; }

/// A capability attached to one end of a socketpair, with the link open on `ctx`. The test writes
/// to `peer_fd` to play the robot's kernel and reads from it to see what the kernel was given.
struct Rig {
    NetCapability cap{"demo-robot-01"};
    RecordingContext ctx;
    int peer_fd = -1;
    std::string self_addr, peer_addr;

    Sources sources;

    explicit Rig(nlohmann::json config = nlohmann::json::object()) {
        std::array<int, 2> sv{};
        EXPECT_EQ(::socketpair(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0, sv.data()), 0);
        peer_fd = sv[1];
        config["enabled"] = true;
        cap.configure(config, sources.reg);
        cap.test_attach_fd(sv[0]);
        self_addr = cap.address();
        peer_addr = net::to_dotted(net::operator_address(CGNAT));
        cap.session_attached(ctx, nlohmann::json::object());
    }
    ~Rig() {
        cap.shutdown();
        if (peer_fd >= 0) ::close(peer_fd);
    }
    void open() {
        cap.on_message(ctx, Envelope{"fjarr.net", "open", "e1", "request", nlohmann::json::object()});
        const auto *r = ctx.last_result();
        ASSERT_NE(r, nullptr);
        ASSERT_TRUE(r->payload.value("ok", false)) << r->payload.dump();
    }
    /// Hand the capability a packet as if the kernel had routed it to the interface.
    void from_kernel(const std::string &p) {
        ASSERT_EQ(::write(peer_fd, p.data(), p.size()), static_cast<ssize_t>(p.size()));
        ctx.fire_readable();
    }
    /// What the capability wrote towards the kernel, or empty.
    std::string to_kernel() {
        std::string buf(2048, '\0');
        const ssize_t n = ::read(peer_fd, buf.data(), buf.size());
        if (n <= 0) return {};
        buf.resize(static_cast<std::size_t>(n));
        return buf;
    }
    nlohmann::json stats() {
        ctx.fire_timer();
        const auto all = ctx.events("link-stats");
        return all.empty() ? nlohmann::json::object() : all.back();
    }
};

} // namespace

// ------------------------------------------------------------------ addressing

TEST(NetAddressing, derivesAStableAddressInsideTheRangeAboveTheReservedBlock) {
    const auto a = net::derive_address("demo-robot-01", CGNAT);
    EXPECT_EQ(a, net::derive_address("demo-robot-01", CGNAT)); // no allocator, no state: same id, same address
    EXPECT_TRUE(CGNAT.contains(a));
    EXPECT_GE(a, CGNAT.base + 256) << "the first /24 is reserved for fixed roles";
    EXPECT_LT(a, CGNAT.base + CGNAT.size() - 1);
    EXPECT_NE(a, net::operator_address(CGNAT));
    EXPECT_NE(a, net::derive_address("demo-robot-02", CGNAT));
}

TEST(NetAddressing, theOperatorAddressIsTheSameOnEveryLink) {
    // docs/27: fixed and well known, so a robot's DDS config can name one peer literally forever.
    EXPECT_EQ(net::to_dotted(net::operator_address(CGNAT)), "100.64.0.1");
}

TEST(NetAddressing, rangesAreParsedAndBadOnesRefused) {
    EXPECT_TRUE(net::parse_range("100.64.0.0/10").has_value());
    EXPECT_FALSE(net::parse_range("100.64.0.1/10").has_value()) << "not on a block boundary";
    EXPECT_FALSE(net::parse_range("100.64.0.0").has_value());
    EXPECT_FALSE(net::parse_range("100.64.0.0/33").has_value());
    EXPECT_FALSE(net::parse_address("100.64.0").has_value());
    EXPECT_FALSE(net::parse_address("100.64.0.256").has_value());
}

// ---------------------------------------------------------------- policy rules

TEST(NetPolicy, aPacketMustBeForThisEndAndFromThePeer) {
    const std::uint32_t self = *net::parse_address("100.66.1.2"), peer = *net::parse_address("100.64.0.1");
    const auto ok = net::inspect(bytes_of(packet("100.64.0.1", "100.66.1.2")));
    EXPECT_EQ(net::check(ok, self, peer, {}), net::Verdict::Allow);

    // The rule that makes lateral movement into the robot's LAN impossible.
    const auto lan = net::inspect(bytes_of(packet("100.64.0.1", "192.168.1.5")));
    EXPECT_EQ(net::check(lan, self, peer, {}), net::Verdict::WrongDestination);

    // The rule that makes robot A unreachable from robot B: not from the expected peer.
    const auto other_robot = net::inspect(bytes_of(packet("100.66.9.9", "100.66.1.2")));
    EXPECT_EQ(net::check(other_robot, self, peer, {}), net::Verdict::WrongSource);

    const auto v6 = net::inspect(bytes_of(std::string(40, '\x60')));
    EXPECT_EQ(net::check(v6, self, peer, {}), net::Verdict::NotIPv4);
}

TEST(NetPolicy, multicastFromThePeerIsAllowedAndFromAnyoneElseIsNot) {
    // ADR-0026: DDS discovery is multicast, and a multicast destination can never be this end's own
    // tunnel address, so the destination rule alone made ROS 2 impossible over the link. The source
    // rule still decides, which is what keeps another robot's traffic out.
    const std::uint32_t self = *net::parse_address("100.66.1.2"), peer = *net::parse_address("100.64.0.1");
    EXPECT_TRUE(net::is_multicast(*net::parse_address("239.255.0.1")));
    EXPECT_TRUE(net::is_multicast(*net::parse_address("224.0.0.22")));
    EXPECT_FALSE(net::is_multicast(self));

    // The real DDS discovery group, and the IGMP membership reports that go with it.
    EXPECT_EQ(net::check(net::inspect(bytes_of(packet("100.64.0.1", "239.255.0.1", 17, 7400))), self, peer, {}), net::Verdict::Allow);
    EXPECT_EQ(net::check(net::inspect(bytes_of(packet("100.64.0.1", "224.0.0.22", 2))), self, peer, {}), net::Verdict::Allow);
    // From anyone but the peer it is refused, so two robots on one operator still cannot see each
    // other's announcements.
    EXPECT_EQ(net::check(net::inspect(bytes_of(packet("100.66.9.9", "239.255.0.1", 17, 7400))), self, peer, {}), net::Verdict::WrongSource);
}

TEST(NetPolicy, anAllowListNarrowsToNamedPortsAndRefusesWhatHasNoPort) {
    const std::uint32_t self = *net::parse_address("100.66.1.2"), peer = *net::parse_address("100.64.0.1");
    const std::vector<std::uint16_t> ssh_only{22};
    EXPECT_EQ(net::check(net::inspect(bytes_of(packet("100.64.0.1", "100.66.1.2", 6, 22))), self, peer, ssh_only), net::Verdict::Allow);
    EXPECT_EQ(net::check(net::inspect(bytes_of(packet("100.64.0.1", "100.66.1.2", 6, 80))), self, peer, ssh_only),
              net::Verdict::PortNotAllowed);
    // ICMP carries no port, so it cannot satisfy a port list — it does not get a free pass.
    EXPECT_EQ(net::check(net::inspect(bytes_of(packet("100.64.0.1", "100.66.1.2", 1))), self, peer, ssh_only), net::Verdict::PortNotAllowed);
}

// ----------------------------------------------------------------- the capability

TEST(NetCapability, declaresARawStreamChannelAndTakesTheInputLease) {
    NetCapability cap{"demo-robot-01"};
    const auto m = cap.manifest();
    EXPECT_EQ(m.name, "fjarr.net");
    ASSERT_EQ(m.channels.size(), 2u);
    EXPECT_EQ(m.channels[1].channel, ChannelClass::Stream);
    EXPECT_EQ(m.channels[1].framing, BulkFraming::Raw);
    EXPECT_TRUE(m.input_bearing) << "granting net is granting network access to the robot (docs/10)";
}

TEST(NetCapability, withoutTheInterfaceItIsUnavailableAndNamesTheFix) {
    NetCapability cap{"demo-robot-01"};
    Sources sources;
    cap.configure(nlohmann::json{{"enabled", true}, {"interface", "fjarr-does-not-exist"}}, sources.reg);
    RecordingContext ctx;
    cap.session_attached(ctx, nlohmann::json::object());
    cap.on_message(ctx, Envelope{"fjarr.net", "open", "e1", "request", nlohmann::json::object()});
    const auto *r = ctx.last_result();
    ASSERT_NE(r, nullptr);
    EXPECT_FALSE(r->payload.value("ok", true));
    // Availability is state, not a fault: a robot without the tunnel set up is a deployment
    // choice. Which of the two ways it is missing depends on the machine the test runs on, and
    // both must name the thing to do about it (docs/26 missing-device behaviour).
    EXPECT_EQ(r->payload["error"]["code"], "unavailable");
    const auto why = r->payload["error"]["message"].get<std::string>();
    const bool no_device_node = why.find("/dev/net/tun") != std::string::npos && why.find("docs/26") != std::string::npos;
    const bool no_interface = why.find("ip tuntap add fjarr-does-not-exist") != std::string::npos;
    EXPECT_TRUE(no_device_node || no_interface) << why;
}

TEST(NetCapability, openReportsTheAddressesAndThatNothingIsForwarded) {
    Rig rig;
    rig.open();
    const auto &p = rig.ctx.last_result()->payload;
    EXPECT_EQ(p["address"], rig.self_addr);
    EXPECT_EQ(p["peer_address"], "100.64.0.1");
    EXPECT_EQ(p["mtu"], 1280);
    EXPECT_FALSE(p["policy"]["forwarding"].get<bool>());
}

TEST(NetCapability, aSecondSessionCannotTakeTheLinkFromTheFirst) {
    Rig rig;
    rig.open();
    RecordingContext other;
    other.sid = "01a0-other-session";
    rig.cap.session_attached(other, nlohmann::json::object());
    rig.cap.on_message(other, Envelope{"fjarr.net", "open", "e2", "request", nlohmann::json::object()});
    const auto *r = other.last_result();
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->payload["error"]["code"], "busy");
}

TEST(NetCapability, outboundPacketsReachTheChannelAndInboundReachTheKernel) {
    Rig rig;
    rig.open();
    rig.from_kernel(packet(rig.self_addr, rig.peer_addr));
    ASSERT_EQ(rig.ctx.stream_sender.frames.size(), 1u) << "a packet the kernel routed here must reach the operator";

    const std::string inbound = packet(rig.peer_addr, rig.self_addr);
    rig.cap.on_binary(rig.ctx, bytes_of(inbound));
    EXPECT_EQ(rig.to_kernel(), inbound);

    const auto s = rig.stats();
    EXPECT_EQ(s["tx_packets"], 1);
    EXPECT_EQ(s["rx_packets"], 1);
    EXPECT_EQ(s["interval_ms"], 1000) << "link-stats is per second while the link is open (docs/08)";
}

TEST(NetCapability, aPacketNotAddressedToThisRobotNeverReachesTheKernel) {
    Rig rig;
    rig.open();
    rig.cap.on_binary(rig.ctx, bytes_of(packet(rig.peer_addr, "192.168.1.5"))); // into the robot's LAN
    rig.cap.on_binary(rig.ctx, bytes_of(packet("100.66.9.9", rig.self_addr)));  // from another robot
    EXPECT_EQ(rig.to_kernel(), "");
    EXPECT_EQ(rig.stats()["dropped_policy"], 2);
}

TEST(NetCapability, anOutboundPacketForAnyoneButTheOperatorIsDropped) {
    Rig rig;
    rig.open();
    rig.from_kernel(packet(rig.self_addr, "100.66.9.9")); // another robot: links never join
    EXPECT_TRUE(rig.ctx.stream_sender.frames.empty());
    EXPECT_EQ(rig.stats()["dropped_policy"], 1);
}

TEST(NetCapability, aPacketOverTheMtuIsDroppedRatherThanFragmented) {
    Rig rig;
    rig.open();
    rig.from_kernel(packet(rig.self_addr, rig.peer_addr, 17, 22, 1400));
    EXPECT_TRUE(rig.ctx.stream_sender.frames.empty());
    rig.cap.on_binary(rig.ctx, bytes_of(packet(rig.peer_addr, rig.self_addr, 17, 22, 1400)));
    EXPECT_EQ(rig.to_kernel(), "");
    EXPECT_EQ(rig.stats()["dropped_mtu"], 2);
}

TEST(NetCapability, aFullChannelTailDropsInsteadOfGrowingAQueue) {
    Rig rig;
    rig.open();
    rig.ctx.stream_sender.refuse = true;
    for (int i = 0; i < 5; i++) rig.from_kernel(packet(rig.self_addr, rig.peer_addr));
    EXPECT_TRUE(rig.ctx.stream_sender.frames.empty());
    EXPECT_EQ(rig.stats()["dropped_queue"], 5);

    // What was refused is gone, not queued: the next accepted packet is the new one, and no burst
    // of stale packets follows the congestion (docs/08#net-packets).
    rig.ctx.stream_sender.refuse = false;
    rig.from_kernel(packet(rig.self_addr, rig.peer_addr, 17, 4242));
    ASSERT_EQ(rig.ctx.stream_sender.frames.size(), 1u);
    EXPECT_EQ(net::inspect(bytes_of(rig.ctx.stream_sender.frames[0])).dst_port, 4242);
}

TEST(NetCapability, theLinkDiesWithTheSessionButTheInterfaceStays) {
    Rig rig;
    rig.open();
    rig.cap.release_all_input(rig.ctx.id()); // safety: called first on every detach path (docs/15)
    rig.from_kernel(packet(rig.self_addr, rig.peer_addr));
    EXPECT_TRUE(rig.ctx.stream_sender.frames.empty()) << "no link, no packets";

    // The device stays attached, so a participant bound to it keeps working and the same session
    // can open the link again (docs/27#lifecycle).
    rig.open();
    rig.from_kernel(packet(rig.self_addr, rig.peer_addr));
    EXPECT_EQ(rig.ctx.stream_sender.frames.size(), 1u);
}

TEST(NetCapability, aPinnedAddressOutsideTheRangeIsAConfigError) {
    NetCapability cap{"demo-robot-01"};
    Sources sources;
    EXPECT_THROW(cap.configure(nlohmann::json{{"enabled", false}, {"address", "10.0.0.5"}}, sources.reg), FjarrError);
    EXPECT_THROW(cap.configure(nlohmann::json{{"enabled", false}, {"address", "100.64.0.1"}}, sources.reg), FjarrError)
        << "the operator's own address is not a robot address";
}
