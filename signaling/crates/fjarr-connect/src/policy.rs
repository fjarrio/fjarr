//! The two packet-policy rules, from the operator's end. Pure functions over an IPv4 header: no
//! device, no channel, no session, so robot-to-robot isolation is testable on its own.
//!
//! This is a deliberate second implementation of `agent/src/capabilities/net_addressing.cpp`, not a
//! shortcut around it. The rules must hold at *both* ends of a link — an operator that trusted
//! whatever arrived on the channel would forward one robot's packets into another's route — and the
//! operator end is Rust. `fjarr-opsim` carries the same rules in C++ for the same reason. The tests
//! below mirror the C++ ones case for case, so a change to one that is not made to the other shows
//! up as a failing test rather than as a hole.
//!
//! spec: docs/27-network-tunnel.md#isolation · ADR-0026
use std::net::Ipv4Addr;

/// Only what the rules need from an IP packet.
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
pub struct PacketView {
    pub ipv4: bool,
    pub src: u32,
    pub dst: u32,
    pub protocol: u8,
    /// TCP/UDP destination port, when the header is present and not fragmented away.
    pub dst_port: Option<u16>,
}

/// Why a packet was let through or dropped. Everything but `Allow` counts as a policy drop; the
/// distinction is for the log line a support engineer reads after the counter sent them looking.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Verdict {
    Allow,
    NotIpv4,
    WrongDestination,
    WrongSource,
}

impl Verdict {
    pub fn name(self) -> &'static str {
        match self {
            Verdict::Allow => "allow",
            Verdict::NotIpv4 => "not-ipv4",
            Verdict::WrongDestination => "wrong-destination",
            Verdict::WrongSource => "wrong-source",
        }
    }
}

/// 224.0.0.0/4 — every IPv4 multicast address.
pub fn is_multicast(addr: u32) -> bool {
    addr & 0xf000_0000 == 0xe000_0000
}

pub fn inspect(packet: &[u8]) -> PacketView {
    let mut v = PacketView::default();
    // IPv6 inside the tunnel is open question #22.
    if packet.len() < 20 || packet[0] >> 4 != 4 {
        return v;
    }
    let ihl = (packet[0] & 0x0f) as usize * 4;
    if ihl < 20 || ihl > packet.len() {
        return v;
    }
    v.ipv4 = true;
    v.protocol = packet[9];
    v.src = u32::from_be_bytes([packet[12], packet[13], packet[14], packet[15]]);
    v.dst = u32::from_be_bytes([packet[16], packet[17], packet[18], packet[19]]);
    let first_fragment = (u16::from_be_bytes([packet[6], packet[7]]) & 0x1fff) == 0;
    if (v.protocol == 6 || v.protocol == 17) && first_fragment && packet.len() >= ihl + 4 {
        v.dst_port = Some(u16::from_be_bytes([packet[ihl + 2], packet[ihl + 3]]));
    }
    v
}

/// The two rules of docs/27#isolation, in whichever direction: inbound from the channel expects
/// `dst` to be this end and `src` to be the robot, outbound from the interface expects the mirror.
pub fn check(p: &PacketView, expect_dst: u32, expect_src: u32) -> Verdict {
    if !p.ipv4 {
        return Verdict::NotIpv4;
    }
    // Multicast from the peer passes on the strength of its source alone (ADR-0026): the
    // destination rule cannot be satisfied by a multicast address, and DDS discovery is multicast,
    // so the rule as first written made the tunnel's headline use case impossible. Nothing is
    // forwarded, and on a point-to-point link a multicast datagram can only have come from the one
    // peer at the other end.
    if is_multicast(p.dst) {
        return if p.src == expect_src {
            Verdict::Allow
        } else {
            Verdict::WrongSource
        };
    }
    if p.dst != expect_dst {
        return Verdict::WrongDestination;
    }
    if p.src != expect_src {
        return Verdict::WrongSource;
    }
    Verdict::Allow
}

pub fn to_u32(a: Ipv4Addr) -> u32 {
    u32::from_be_bytes(a.octets())
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Build an IPv4 packet with the fields the rules read.
    fn packet(src: &str, dst: &str, protocol: u8, dst_port: u16) -> Vec<u8> {
        let mut p = vec![0u8; 28];
        p[0] = 0x45;
        p[9] = protocol;
        p[12..16].copy_from_slice(&src.parse::<Ipv4Addr>().unwrap().octets());
        p[16..20].copy_from_slice(&dst.parse::<Ipv4Addr>().unwrap().octets());
        p[22..24].copy_from_slice(&dst_port.to_be_bytes());
        p
    }

    fn addr(a: &str) -> u32 {
        to_u32(a.parse().unwrap())
    }

    #[test]
    fn a_packet_between_this_end_and_its_robot_is_allowed() {
        let p = inspect(&packet("100.70.118.224", "100.64.0.1", 6, 22));
        assert!(p.ipv4);
        assert_eq!(p.dst_port, Some(22));
        assert_eq!(
            check(&p, addr("100.64.0.1"), addr("100.70.118.224")),
            Verdict::Allow
        );
    }

    /// The isolation rule that matters: a packet from a *different* robot's address, arriving on
    /// this robot's channel, is refused. Two robots attached at once cannot reach each other
    /// because neither end will carry the other's source (docs/15 safety class).
    #[test]
    fn another_robots_source_is_refused() {
        let p = inspect(&packet("100.70.0.9", "100.64.0.1", 6, 22));
        assert_eq!(
            check(&p, addr("100.64.0.1"), addr("100.70.118.224")),
            Verdict::WrongSource
        );
    }

    #[test]
    fn a_packet_for_someone_else_is_refused() {
        let p = inspect(&packet("100.70.118.224", "100.64.0.2", 6, 22));
        assert_eq!(
            check(&p, addr("100.64.0.1"), addr("100.70.118.224")),
            Verdict::WrongDestination
        );
    }

    #[test]
    fn multicast_passes_on_its_source_alone() {
        let dds = inspect(&packet("100.70.118.224", "239.255.0.1", 17, 7400));
        assert_eq!(
            check(&dds, addr("100.64.0.1"), addr("100.70.118.224")),
            Verdict::Allow
        );
        let forged = inspect(&packet("100.70.0.9", "239.255.0.1", 17, 7400));
        assert_eq!(
            check(&forged, addr("100.64.0.1"), addr("100.70.118.224")),
            Verdict::WrongSource
        );
    }

    #[test]
    fn anything_that_is_not_ipv4_is_refused() {
        assert_eq!(
            check(&inspect(&[]), addr("100.64.0.1"), addr("100.70.118.224")),
            Verdict::NotIpv4
        );
        let mut v6 = vec![0u8; 40];
        v6[0] = 0x60;
        assert_eq!(
            check(&inspect(&v6), addr("100.64.0.1"), addr("100.70.118.224")),
            Verdict::NotIpv4
        );
        // A truncated header is not a packet either, however plausible its first byte.
        let short = vec![0x45u8; 19];
        assert!(!inspect(&short).ipv4);
    }

    /// A later fragment has no port to read. The operator end passes no port list, so this must
    /// still be allowed — the rule that reads ports lives on the robot (docs/27#isolation).
    #[test]
    fn a_later_fragment_has_no_port_and_is_still_carried() {
        let mut p = packet("100.70.118.224", "100.64.0.1", 6, 22);
        p[6..8].copy_from_slice(&0x0020u16.to_be_bytes()); // fragment offset, not the first
        let v = inspect(&p);
        assert_eq!(v.dst_port, None);
        assert_eq!(
            check(&v, addr("100.64.0.1"), addr("100.70.118.224")),
            Verdict::Allow
        );
    }
}
