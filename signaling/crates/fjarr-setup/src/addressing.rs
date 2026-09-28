//! The tunnel range and the two fixed roles in it (docs/27#addressing): the operator is the range's
//! first host, and the device's own address comes from `fjarr-agent --net-address`, never derived
//! a second time here.
use std::net::Ipv4Addr;

use anyhow::{bail, Result};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Range {
    pub base: u32,
    pub prefix: u8,
}

impl Range {
    /// `100.64.0.0/10`: a CIDR block between /8 and /30, as the agent validates it.
    pub fn parse(s: &str) -> Result<Range> {
        let Some((addr, plen)) = s.split_once('/') else {
            bail!("range {s:?} is not a CIDR block")
        };
        let addr: Ipv4Addr = addr
            .parse()
            .map_err(|_| anyhow::anyhow!("range {s:?}: not an IPv4 address"))?;
        let prefix: u8 = plen
            .parse()
            .map_err(|_| anyhow::anyhow!("range {s:?}: not a prefix length"))?;
        if !(8..=30).contains(&prefix) {
            bail!("range {s:?} is not a CIDR block between /8 and /30");
        }
        let mask = u32::MAX << (32 - prefix);
        Ok(Range {
            base: u32::from(addr) & mask,
            prefix,
        })
    }

    /// The operator host: the first address of the range, the same on every link.
    pub fn operator(&self) -> Ipv4Addr {
        Ipv4Addr::from(self.base + 1)
    }

    pub fn contains(&self, a: Ipv4Addr) -> bool {
        self.overlaps(a, 32)
    }

    /// Does the route `dst/plen` share any address with this range?
    pub fn overlaps(&self, dst: Ipv4Addr, plen: u8) -> bool {
        let shorter = self.prefix.min(plen);
        if shorter == 0 {
            return true;
        }
        (u32::from(dst) ^ self.base) >> (32 - shorter) == 0
    }
}

impl std::fmt::Display for Range {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}/{}", Ipv4Addr::from(self.base), self.prefix)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_default_range_and_its_operator() {
        let r = Range::parse("100.64.0.0/10").unwrap();
        assert_eq!(r.operator(), Ipv4Addr::new(100, 64, 0, 1));
        assert!(r.contains(Ipv4Addr::new(100, 127, 255, 254)));
        assert!(!r.contains(Ipv4Addr::new(100, 128, 0, 1)));
        assert_eq!(r.to_string(), "100.64.0.0/10");
    }

    #[test]
    fn a_range_is_normalised_to_its_network_address() {
        assert_eq!(
            Range::parse("10.20.30.40/8").unwrap().to_string(),
            "10.0.0.0/8"
        );
    }

    #[test]
    fn what_the_agent_rejects_is_rejected_here() {
        for bad in [
            "100.64.0.0",
            "100.64.0.0/7",
            "100.64.0.0/31",
            "300.1.1.1/10",
            "abc/10",
        ] {
            assert!(Range::parse(bad).is_err(), "{bad}");
        }
    }

    #[test]
    fn overlap_is_symmetric_across_prefix_lengths() {
        let r = Range::parse("100.64.0.0/10").unwrap();
        assert!(
            r.overlaps(Ipv4Addr::new(100, 64, 0, 0), 8),
            "a /8 containing the range"
        );
        assert!(
            r.overlaps(Ipv4Addr::new(100, 100, 0, 0), 16),
            "a /16 inside the range"
        );
        assert!(!r.overlaps(Ipv4Addr::new(192, 168, 10, 0), 24));
        assert!(
            !r.overlaps(Ipv4Addr::new(100, 128, 0, 0), 10),
            "the neighbouring /10"
        );
        assert!(
            r.overlaps(Ipv4Addr::new(0, 0, 0, 0), 0),
            "the default route covers everything"
        );
    }
}
