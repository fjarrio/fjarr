//! A robot's tunnel address from its id (docs/27#addressing), so `list --ssh-config` can print a
//! stanza the robot will actually answer at.
//!
//! This is the agent's derivation restated in Rust, not a second source of truth: the robot is the
//! authority on its own address and `connect` always takes what the robot reports. What keeps the
//! two from drifting is the pair of vectors below, produced by `fjarr-agent --net-address` for the
//! lab's robots — a change to either implementation that is not made to the other fails here.
//!
//! spec: docs/27-network-tunnel.md#addressing · agent/src/capabilities/net_addressing.cpp
use std::net::Ipv4Addr;

use anyhow::{anyhow, Result};
use sha2::{Digest, Sha256};

/// An IPv4 CIDR block, host order.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Range {
    pub base: u32,
    pub prefix: u8,
}

impl Range {
    pub fn parse(cidr: &str) -> Result<Self> {
        let (ip, prefix) = cidr
            .split_once('/')
            .ok_or_else(|| anyhow!("{cidr:?} is not a CIDR block"))?;
        let base = u32::from_be_bytes(
            ip.parse::<Ipv4Addr>()
                .map_err(|_| anyhow!("{ip:?} is not an IPv4 address"))?
                .octets(),
        );
        let prefix: u8 = prefix
            .parse()
            .map_err(|_| anyhow!("{prefix:?} is not a prefix length"))?;
        if !(8..=30).contains(&prefix) {
            return Err(anyhow!(
                "a tunnel range is between /8 and /30, not /{prefix}"
            ));
        }
        let r = Self { base, prefix };
        if base & (r.size() - 1) != 0 {
            return Err(anyhow!("{cidr} is not on a block boundary"));
        }
        Ok(r)
    }

    pub fn size(&self) -> u32 {
        1u32 << (32 - self.prefix)
    }
}

/// The default, `100.64.0.0/10` — the CGNAT block, which nothing on a robot's LAN uses (docs/27).
pub fn default_range() -> Range {
    Range {
        base: u32::from_be_bytes([100, 64, 0, 0]),
        prefix: 10,
    }
}

/// SHA-256 of the id, first four bytes big-endian, masked into the range past the reserved first
/// /24 and short of the top address. No allocator and no state anywhere.
pub fn derive_address(robot_id: &str, range: &Range) -> Ipv4Addr {
    const RESERVED: u32 = 256;
    let usable = range.size() - RESERVED - 1;
    let digest = Sha256::digest(robot_id.as_bytes());
    let h = u32::from_be_bytes([digest[0], digest[1], digest[2], digest[3]]);
    Ipv4Addr::from((range.base + RESERVED + (h % usable)).to_be_bytes())
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Produced by `fjarr-agent --net-address` on the lab's robots. If these move, so did the agent.
    #[test]
    fn the_lab_robots_derive_what_the_agent_derives() {
        let r = default_range();
        assert_eq!(
            derive_address("demo-robot-01", &r),
            Ipv4Addr::new(100, 70, 118, 224)
        );
        assert_eq!(
            derive_address("demo-robot-02", &r),
            Ipv4Addr::new(100, 103, 147, 23)
        );
    }

    #[test]
    fn the_address_stays_inside_the_range_above_the_reserved_block() {
        let r = Range::parse("10.200.0.0/16").unwrap();
        for id in ["a", "robot-1", "x".repeat(100).as_str()] {
            let a = u32::from_be_bytes(derive_address(id, &r).octets());
            assert!(a >= r.base + 256 && a < r.base + r.size() - 1, "{id}");
        }
    }

    #[test]
    fn ranges_are_parsed_and_bad_ones_refused() {
        assert_eq!(Range::parse("100.64.0.0/10").unwrap(), default_range());
        assert!(Range::parse("100.64.0.1/10").is_err(), "not on a boundary");
        assert!(Range::parse("100.64.0.0/31").is_err());
        assert!(Range::parse("100.64.0.0").is_err());
        assert!(Range::parse("nonsense/10").is_err());
    }
}
