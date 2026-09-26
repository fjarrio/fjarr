// spec: docs/27-network-tunnel.md#addressing · docs/27-network-tunnel.md#isolation
#include "capabilities/net_addressing.hpp"

#include <charconv>
#include <cstdio>

#include <glib.h>

namespace fjarr::net {

std::optional<std::uint32_t> parse_address(std::string_view dotted) {
    std::uint32_t out = 0;
    int octets = 0;
    std::size_t i = 0;
    while (i <= dotted.size()) {
        std::size_t j = i;
        while (j < dotted.size() && dotted[j] != '.') j++;
        if (j == i) return std::nullopt;
        unsigned v = 0;
        const auto [ptr, ec] = std::from_chars(dotted.data() + i, dotted.data() + j, v);
        if (ec != std::errc{} || ptr != dotted.data() + j || v > 255) return std::nullopt;
        out = (out << 8) | v;
        octets++;
        if (j == dotted.size()) break;
        i = j + 1;
    }
    return octets == 4 ? std::optional<std::uint32_t>{out} : std::nullopt;
}

std::string to_dotted(std::uint32_t a) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%u.%u.%u.%u", (a >> 24) & 0xff, (a >> 16) & 0xff, (a >> 8) & 0xff, a & 0xff);
    return buf;
}

std::optional<Range> parse_range(std::string_view cidr) {
    const auto slash = cidr.find('/');
    if (slash == std::string_view::npos) return std::nullopt;
    const auto base = parse_address(cidr.substr(0, slash));
    if (!base) return std::nullopt;
    int prefix = 0;
    const auto tail = cidr.substr(slash + 1);
    const auto [ptr, ec] = std::from_chars(tail.data(), tail.data() + tail.size(), prefix);
    if (ec != std::errc{} || ptr != tail.data() + tail.size() || prefix < 8 || prefix > 30) return std::nullopt;
    const Range r{*base, prefix};
    if ((r.base & (r.size() - 1)) != 0) return std::nullopt; // not on a block boundary
    return r;
}

std::uint32_t operator_address(const Range& range) { return range.base + 1; }

std::uint32_t derive_address(std::string_view robot_id, const Range& range) {
    // The first /24 is reserved for fixed roles (the operator lives there) and the top address is
    // the block's broadcast, so derivation picks from what is left.
    constexpr std::uint32_t RESERVED = 256;
    const std::uint32_t usable = range.size() - RESERVED - 1;
    gsize len = 32;
    guchar digest[32];
    GChecksum* sum = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(sum, reinterpret_cast<const guchar*>(robot_id.data()), static_cast<gssize>(robot_id.size()));
    g_checksum_get_digest(sum, digest, &len);
    g_checksum_free(sum);
    const std::uint32_t h = (static_cast<std::uint32_t>(digest[0]) << 24) | (static_cast<std::uint32_t>(digest[1]) << 16) |
                            (static_cast<std::uint32_t>(digest[2]) << 8) | static_cast<std::uint32_t>(digest[3]);
    return range.base + RESERVED + (h % usable);
}

PacketView inspect(std::span<const std::byte> packet) {
    PacketView v;
    const auto* p = reinterpret_cast<const std::uint8_t*>(packet.data());
    if (packet.size() < 20 || (p[0] >> 4) != 4) return v; // IPv6 inside the tunnel is question #22
    const std::size_t ihl = static_cast<std::size_t>(p[0] & 0x0f) * 4;
    if (ihl < 20 || ihl > packet.size()) return v;
    v.ipv4 = true;
    v.protocol = p[9];
    v.src = (static_cast<std::uint32_t>(p[12]) << 24) | (static_cast<std::uint32_t>(p[13]) << 16) |
            (static_cast<std::uint32_t>(p[14]) << 8) | static_cast<std::uint32_t>(p[15]);
    v.dst = (static_cast<std::uint32_t>(p[16]) << 24) | (static_cast<std::uint32_t>(p[17]) << 16) |
            (static_cast<std::uint32_t>(p[18]) << 8) | static_cast<std::uint32_t>(p[19]);
    const bool first_fragment = (((static_cast<unsigned>(p[6]) << 8) | p[7]) & 0x1fff) == 0;
    if ((v.protocol == 6 || v.protocol == 17) && first_fragment && packet.size() >= ihl + 4)
        v.dst_port = static_cast<std::uint16_t>((static_cast<unsigned>(p[ihl + 2]) << 8) | p[ihl + 3]);
    return v;
}

const char* verdict_name(Verdict v) {
    switch (v) {
    case Verdict::Allow: return "allow";
    case Verdict::NotIPv4: return "not-ipv4";
    case Verdict::WrongDestination: return "wrong-destination";
    case Verdict::WrongSource: return "wrong-source";
    case Verdict::PortNotAllowed: return "port-not-allowed";
    }
    return "unknown";
}

/// 224.0.0.0/4 — every IPv4 multicast address.
bool is_multicast(std::uint32_t a) { return (a & 0xf0000000u) == 0xe0000000u; }

Verdict check(const PacketView& p, std::uint32_t expect_dst, std::uint32_t expect_src, const std::vector<std::uint16_t>& allow_ports) {
    if (!p.ipv4) return Verdict::NotIPv4;
    // Multicast from the peer is allowed through (ADR-0026): the destination rule below cannot be
    // satisfied by a multicast address, and DDS discovery is multicast, so the rule as first written
    // made the tunnel's headline use case impossible — measured, not theorised (docs/27#ros2). The
    // source rule still applies, nothing is forwarded, and a multicast datagram on a
    // point-to-point link can only have come from the one peer at the other end.
    if (is_multicast(p.dst)) return p.src == expect_src ? Verdict::Allow : Verdict::WrongSource;
    if (p.dst != expect_dst) return Verdict::WrongDestination;
    if (p.src != expect_src) return Verdict::WrongSource;
    if (allow_ports.empty()) return Verdict::Allow;
    // A port list narrows what the peer may reach. A packet with no port to read (ICMP, a later
    // fragment) cannot be checked against the list, so it does not pass one.
    if (!p.dst_port) return Verdict::PortNotAllowed;
    for (const std::uint16_t allowed : allow_ports)
        if (allowed == *p.dst_port) return Verdict::Allow;
    return Verdict::PortNotAllowed;
}

} // namespace fjarr::net
