//! The machine's facts and the few commands `setup` runs: os-release, accounts, systemctl, and the
//! routing table over netlink (for the range check and the LAN interface Cyclone keeps).
use std::net::Ipv4Addr;
use std::path::{Path, PathBuf};
use std::process::Command;

use anyhow::{bail, Context, Result};

/// System changes are applied only on Ubuntu with apt (docs/26#the-setup-tool); elsewhere the tool
/// prints what to set.
pub fn ubuntu_with_apt() -> bool {
    let Ok(text) = std::fs::read_to_string("/etc/os-release") else {
        return false;
    };
    let mut ids = String::new();
    for line in text.lines() {
        if let Some(v) = line
            .strip_prefix("ID=")
            .or_else(|| line.strip_prefix("ID_LIKE="))
        {
            ids.push(' ');
            ids.push_str(v.trim_matches('"'));
        }
    }
    (ids.contains("ubuntu") || ids.contains("debian")) && Path::new("/usr/bin/apt-get").exists()
}

pub fn uid_of(user: &str) -> Result<libc::uid_t> {
    let c = std::ffi::CString::new(user)?;
    // SAFETY: getpwnam takes a valid C string; the result is read before any other call touches it.
    let pw = unsafe { libc::getpwnam(c.as_ptr()) };
    if pw.is_null() {
        bail!("no account {user:?}: the fjarr-agent package's sysusers entry creates it (sudo systemd-sysusers)");
    }
    Ok(unsafe { (*pw).pw_uid })
}

/// `/var/lib/fjarr`: the agent's state directory (0700, owned by the agent's account per the
/// profile). Created here when the service has not started yet, with the same ownership, so the
/// profile check does not fail on setup's own doing.
pub fn ensure_state_dir(path: &Path, owner: &str) -> Result<()> {
    if path.exists() {
        return Ok(());
    }
    std::fs::create_dir_all(path).with_context(|| format!("creating {}", path.display()))?;
    let uid = uid_of(owner)?;
    let c = std::ffi::CString::new(path.to_string_lossy().as_bytes())?;
    // SAFETY: a valid path; gid -1 leaves the group as created.
    if unsafe { libc::chown(c.as_ptr(), uid, u32::MAX) } != 0 {
        return Err(std::io::Error::last_os_error())
            .with_context(|| format!("chown {} to {owner}", path.display()));
    }
    std::fs::set_permissions(path, std::os::unix::fs::PermissionsExt::from_mode(0o700))?;
    Ok(())
}

pub fn systemctl(args: &[&str]) -> Result<()> {
    let status = Command::new("systemctl")
        .args(args)
        .status()
        .context("running systemctl")?;
    if !status.success() {
        bail!("systemctl {} failed ({status})", args.join(" "));
    }
    Ok(())
}

/// Restart the agent if it is running, so it picks up the change now. Its own failure to start
/// (no credential yet, a server it cannot reach) is the agent's to report, not this command's:
/// the closing `--check` still runs, and the journal has the reason.
pub fn restart_agent_if_running() -> Result<()> {
    let status = Command::new("systemctl")
        .args(["try-restart", "fjarr-agent.service"])
        .status()
        .context("running systemctl")?;
    if !status.success() {
        cliclack::log::warning("fjarr-agent did not come back up after the restart: journalctl -u fjarr-agent.service has why")?;
    }
    Ok(())
}

pub fn systemctl_output(args: &[&str]) -> Result<String> {
    let out = Command::new("systemctl")
        .args(args)
        .output()
        .context("running systemctl")?;
    Ok(String::from_utf8_lossy(&out.stdout).into_owned())
}

/// Is `unit` known to systemd (installed anywhere it looks)?
pub fn unit_exists(unit: &str) -> bool {
    systemctl_output(&["show", "-p", "LoadState", "--value", unit])
        .map(|s| s.trim() == "loaded")
        .unwrap_or(false)
}

/// The enabled services, for the ROS units picker: the customer's own, never Fjarr's.
pub fn enabled_services() -> Vec<String> {
    systemctl_output(&[
        "list-unit-files",
        "--type=service",
        "--state=enabled",
        "--no-legend",
        "--plain",
    ])
    .unwrap_or_default()
    .lines()
    .filter_map(|l| l.split_whitespace().next())
    .filter(|u| !u.starts_with("fjarr-") && !u.starts_with("systemd-"))
    .map(str::to_string)
    .collect()
}

pub fn drop_in_path(unit: &str) -> PathBuf {
    PathBuf::from(format!("/etc/systemd/system/{unit}.d/50-fjarr-net.conf"))
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Route {
    pub dst: Ipv4Addr,
    pub prefix: u8,
    pub oif: Option<String>,
}

/// The main table's IPv4 routes, each with its output interface's name.
pub async fn routes_v4() -> Result<Vec<Route>> {
    use futures_util::TryStreamExt;
    use rtnetlink::packet_route::route::{RouteAddress, RouteAttribute};

    let handle = fjarr_netdev::netlink().await?;
    let all = rtnetlink::RouteMessageBuilder::<Ipv4Addr>::new().build();
    let mut stream = handle.route().get(all).execute();
    let mut routes = Vec::new();
    while let Some(msg) = stream
        .try_next()
        .await
        .context("reading the routing table")?
    {
        // 254 = main; the local table's own-address entries are not "routes that already exist".
        if msg.header.table != 254 {
            continue;
        }
        let mut dst = Ipv4Addr::UNSPECIFIED;
        let mut oif = None;
        for attr in &msg.attributes {
            match attr {
                RouteAttribute::Destination(RouteAddress::Inet(a)) => dst = *a,
                RouteAttribute::Oif(i) => oif = Some(*i),
                _ => {}
            }
        }
        let name = match oif {
            Some(i) => link_name(&handle, i).await?,
            None => None,
        };
        routes.push(Route {
            dst,
            prefix: msg.header.destination_prefix_length,
            oif: name,
        });
    }
    Ok(routes)
}

async fn link_name(handle: &rtnetlink::Handle, index: u32) -> Result<Option<String>> {
    use futures_util::TryStreamExt;
    use rtnetlink::packet_route::link::LinkAttribute;
    let mut links = handle.link().get().match_index(index).execute();
    match links.try_next().await {
        Ok(Some(link)) => Ok(link.attributes.iter().find_map(|a| match a {
            LinkAttribute::IfName(n) => Some(n.clone()),
            _ => None,
        })),
        Ok(None) => Ok(None),
        Err(rtnetlink::Error::NetlinkError(e)) if e.raw_code() == -libc::ENODEV => Ok(None),
        Err(e) => Err(e).with_context(|| format!("looking up interface {index}")),
    }
}

/// The routes that already cover part of `range`, ignoring the tunnel's own: what
/// `fjarr-agent --check` calls an overlap (docs/27#addressing).
pub fn overlapping<'a>(
    routes: &'a [Route],
    range: &crate::addressing::Range,
    tunnel: &str,
) -> Vec<&'a Route> {
    routes
        .iter()
        .filter(|r| {
            r.prefix > 0 && r.oif.as_deref() != Some(tunnel) && range.overlaps(r.dst, r.prefix)
        })
        .collect()
}

/// The default route's interface: the LAN Cyclone keeps using beside the tunnel.
pub fn default_route_interface(routes: &[Route]) -> Option<String> {
    routes
        .iter()
        .find(|r| r.prefix == 0)
        .and_then(|r| r.oif.clone())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::addressing::Range;

    fn route(dst: &str, prefix: u8, oif: &str) -> Route {
        Route {
            dst: dst.parse().unwrap(),
            prefix,
            oif: Some(oif.into()),
        }
    }

    #[test]
    fn an_overlap_is_a_foreign_route_into_the_range_never_the_tunnels_own_or_the_default() {
        let range = Range::parse("100.64.0.0/10").unwrap();
        let routes = vec![
            route("0.0.0.0", 0, "wlp3s0"),
            route("192.168.10.0", 24, "wlp3s0"),
            route("100.64.0.1", 32, "fjarr0"),
            route("100.100.0.0", 16, "wg0"),
        ];
        let hits = overlapping(&routes, &range, "fjarr0");
        assert_eq!(hits, vec![&routes[3]]);
        assert_eq!(default_route_interface(&routes).as_deref(), Some("wlp3s0"));
    }
}
