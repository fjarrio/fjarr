//! The local addresses this end's ICE binds: never a Fjarr tunnel interface (docs/23 ICE,
//! docs/27#the-operator-client, #34). webrtc-rs expands the wildcard `0.0.0.0` into every up IPv4
//! interface at bind time, so with a link already up — a second robot, an ICE restart — it would
//! offer that link's tunnel address: a path the robot reaches only through its own tunnel, the
//! link riding on itself. The same enumeration, minus the tunnels, is passed explicitly instead.
use std::net::{IpAddr, Ipv4Addr};

/// One local interface address.
#[derive(Debug, Clone)]
pub struct IfAddr {
    pub name: String,
    pub up: bool,
    pub loopback: bool,
    pub ip: IpAddr,
}

/// A Fjarr tunnel: any name starting `fjarr`, or one of `excluded` (a configured `[net] interface`).
/// By name, not by 100.64.0.0/10 — that is also carrier-NAT space a 4G operator really has.
pub fn is_tunnel_interface(name: &str, excluded: &[String]) -> bool {
    name.starts_with("fjarr") || excluded.iter().any(|e| !e.is_empty() && e == name)
}

/// What the wildcard would have bound — up, not loopback, IPv4, not link-local — minus tunnels.
pub fn gatherable(all: &[IfAddr], excluded: &[String]) -> Vec<Ipv4Addr> {
    all.iter()
        .filter(|a| a.up && !a.loopback && !is_tunnel_interface(&a.name, excluded))
        .filter_map(|a| match a.ip {
            IpAddr::V4(v4) if !v4.is_loopback() && !v4.is_link_local() && !v4.is_unspecified() => {
                Some(v4)
            }
            _ => None,
        })
        .collect()
}

/// The UDP addresses to bind: one per gatherable address, or the wildcard when none is left, which
/// is webrtc-rs's own fallback (a relay still works).
pub fn udp_bind_addrs(all: &[IfAddr], excluded: &[String]) -> Vec<String> {
    let keep = gatherable(all, excluded);
    if keep.is_empty() {
        return vec!["0.0.0.0:0".to_string()];
    }
    keep.iter().map(|ip| format!("{ip}:0")).collect()
}

/// Every local interface address.
#[cfg(unix)]
pub fn local_interface_addresses() -> Vec<IfAddr> {
    let mut out = Vec::new();
    let mut list: *mut libc::ifaddrs = std::ptr::null_mut();
    // SAFETY: getifaddrs fills `list` with a linked list we walk read-only and free once.
    if unsafe { libc::getifaddrs(&mut list) } != 0 {
        return out;
    }
    let mut cur = list;
    while !cur.is_null() {
        // SAFETY: `cur` is a node of the list getifaddrs returned, valid until freeifaddrs.
        let ifa = unsafe { &*cur };
        cur = ifa.ifa_next;
        if ifa.ifa_addr.is_null() || ifa.ifa_name.is_null() {
            continue;
        }
        // SAFETY: a non-null ifa_addr points at a sockaddr whose family says which one it is.
        let family = i32::from(unsafe { (*ifa.ifa_addr).sa_family });
        if family != libc::AF_INET {
            continue;
        }
        // SAFETY: AF_INET means the sockaddr is a sockaddr_in.
        let sin = unsafe { &*(ifa.ifa_addr as *const libc::sockaddr_in) };
        let ip = Ipv4Addr::from(u32::from_be(sin.sin_addr.s_addr));
        // SAFETY: ifa_name is a NUL-terminated interface name.
        let name = unsafe { std::ffi::CStr::from_ptr(ifa.ifa_name) }
            .to_string_lossy()
            .into_owned();
        let flags = ifa.ifa_flags as i32;
        out.push(IfAddr {
            name,
            up: flags & libc::IFF_UP != 0,
            loopback: flags & libc::IFF_LOOPBACK != 0,
            ip: IpAddr::V4(ip),
        });
    }
    // SAFETY: the list getifaddrs allocated, freed once.
    unsafe { libc::freeifaddrs(list) };
    out
}

/// No tunnel exists on this platform: the wildcard, as before.
#[cfg(not(unix))]
pub fn local_interface_addresses() -> Vec<IfAddr> {
    Vec::new()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn a(name: &str, ip: &str, up: bool, loopback: bool) -> IfAddr {
        IfAddr {
            name: name.into(),
            up,
            loopback,
            ip: ip.parse().unwrap(),
        }
    }

    #[test]
    fn tunnels_never_bind_and_carrier_nat_still_does() {
        let all = vec![
            a("lo", "127.0.0.1", true, true),
            a("wlp0s20f3", "192.168.10.188", true, false),
            a("fjarr0", "100.64.0.1", true, false), // the first robot's link, up
            a("tun9", "100.64.0.1", true, false),   // `[net] interface = "tun9"`
            a("wwan0", "100.72.3.4", true, false),  // 4G: the same range, a real path
            a("docker0", "172.17.0.1", false, false),
            a("eth9", "169.254.3.3", true, false),
            a("eth0", "fe80::1", true, false),
        ];
        assert_eq!(
            udp_bind_addrs(&all, &["tun9".into()]),
            vec!["192.168.10.188:0".to_string(), "100.72.3.4:0".to_string()]
        );
    }

    #[test]
    fn nothing_left_means_the_wildcard() {
        assert_eq!(
            udp_bind_addrs(&[a("fjarr0", "100.64.0.1", true, false)], &[]),
            vec!["0.0.0.0:0".to_string()]
        );
    }
}
