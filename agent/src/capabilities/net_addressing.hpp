#pragma once
// Addressing and the two packet-policy rules of the network tunnel, as pure functions: no
// interface, no session, no I/O — so the rules that make robot-to-robot isolation structural are
// testable on their own (docs/27#testing).
// spec: docs/27-network-tunnel.md#addressing · docs/27-network-tunnel.md#isolation
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fjarr::net {

/// An IPv4 CIDR block, host-order.
struct Range {
    std::uint32_t base = 0;
    int prefix = 0;
    std::uint32_t size() const { return prefix >= 32 ? 1u : (1u << (32 - prefix)); }
    bool contains(std::uint32_t a) const { return (a & ~(size() - 1)) == base; }
};

std::optional<Range> parse_range(std::string_view cidr);
std::optional<std::uint32_t> parse_address(std::string_view dotted);
std::string to_dotted(std::uint32_t addr);

/// The operator host, fixed and the same on every link so a robot's DDS config can name it
/// literally and forever (docs/27#addressing). It is the range's base + 1.
std::uint32_t operator_address(const Range& range);

/// A robot's address, derived from its id: SHA-256 masked into the range, skipping the reserved
/// first /24 and the top address. No allocator and no state anywhere — the robot knows its own
/// address at boot without asking (docs/27#addressing).
std::uint32_t derive_address(std::string_view robot_id, const Range& range);

/// Only what the policy rules need from an IP packet.
struct PacketView {
    bool ipv4 = false;
    std::uint32_t src = 0, dst = 0;
    std::uint8_t protocol = 0;
    /// TCP/UDP destination port, when the header is present and not fragmented away.
    std::optional<std::uint16_t> dst_port;
};
PacketView inspect(std::span<const std::byte> packet);

/// Why a packet was let through or dropped. Everything but `Allow` counts as `dropped_policy` on
/// `link-stats` (docs/08#net-packets); the distinction is for the log line, which is what a
/// support engineer reads after the counter told them to look.
enum class Verdict : std::uint8_t { Allow, NotIPv4, WrongDestination, WrongSource, PortNotAllowed };
const char* verdict_name(Verdict v);

/// True for 224.0.0.0/4. Multicast is the one destination the rules below let through on the
/// strength of its source alone (ADR-0026).
bool is_multicast(std::uint32_t addr);

/// The two rules of docs/27#isolation. They are the same rules in both directions, which is why
/// they take the addresses rather than a side: inbound from the channel expects `dst` to be this
/// end and `src` to be the peer; outbound from the interface expects the mirror. `allow_ports`
/// empty means every port, and it is only ever passed for the inbound direction.
Verdict check(const PacketView& p, std::uint32_t expect_dst, std::uint32_t expect_src, const std::vector<std::uint16_t>& allow_ports);

} // namespace fjarr::net
