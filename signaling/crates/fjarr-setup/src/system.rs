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

/// Is systemd the init here? In a container it is not, and the agent has to be started by hand.
pub fn systemd_running() -> bool {
    Path::new("/run/systemd/system").is_dir()
}

pub fn gid_of(group: &str) -> Result<libc::gid_t> {
    let c = std::ffi::CString::new(group)?;
    // SAFETY: getgrnam takes a valid C string; the result is read before any other call touches it.
    let gr = unsafe { libc::getgrnam(c.as_ptr()) };
    if gr.is_null() {
        bail!("no group {group:?}: the fjarr-agent package's sysusers entry creates it (sudo systemd-sysusers)");
    }
    Ok(unsafe { (*gr).gr_gid })
}

/// `chown user:group` and `chmod mode`: the configuration is root's, readable by the agent's group
/// and nobody else, because it carries the device token (docs/26#fjarr-agent-setup).
pub fn set_owner_mode(path: &Path, user: &str, group: &str, mode: u32) -> Result<()> {
    let uid = uid_of(user)?;
    let gid = gid_of(group)?;
    let c = std::ffi::CString::new(path.to_string_lossy().as_bytes())?;
    // SAFETY: a valid path.
    if unsafe { libc::chown(c.as_ptr(), uid, gid) } != 0 {
        return Err(std::io::Error::last_os_error())
            .with_context(|| format!("chown {} to {user}:{group}", path.display()));
    }
    std::fs::set_permissions(path, std::os::unix::fs::PermissionsExt::from_mode(mode))
        .with_context(|| format!("chmod {mode:o} {}", path.display()))
}

/// The accounts a person could hold a terminal as: ordinary users with a login shell.
pub fn human_accounts() -> Vec<String> {
    std::fs::read_to_string("/etc/passwd")
        .unwrap_or_default()
        .lines()
        .filter_map(|l| {
            let f: Vec<&str> = l.split(':').collect();
            let uid: u32 = f.get(2)?.parse().ok()?;
            let shell = f.get(6)?;
            ((1000..60000).contains(&uid)
                && !shell.ends_with("nologin")
                && !shell.ends_with("/false"))
            .then(|| f[0].to_string())
        })
        .collect()
}

/// Is the package installed (dpkg's view; the only package manager the tool acts through)?
pub fn dpkg_installed(package: &str) -> bool {
    Command::new("dpkg-query")
        .args(["-W", "-f", "${db:Status-Status}", package])
        .output()
        .map(|o| o.status.success() && String::from_utf8_lossy(&o.stdout).trim() == "installed")
        .unwrap_or(false)
}

pub fn unit_enabled(unit: &str) -> bool {
    systemctl_output(&["is-enabled", unit])
        .map(|s| s.trim() == "enabled")
        .unwrap_or(false)
}

pub fn unit_active(unit: &str) -> bool {
    systemctl_output(&["is-active", unit])
        .map(|s| s.trim() == "active")
        .unwrap_or(false)
}

/// The agent's `STATUS=` line and state, as `systemctl status` shows them (ADR-0019 addendum).
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct AgentState {
    pub active: String,
    pub sub: String,
    pub status: String,
    pub restarts: u64,
}

pub fn agent_state() -> AgentState {
    let out = systemctl_output(&[
        "show",
        "-p",
        "ActiveState",
        "-p",
        "SubState",
        "-p",
        "StatusText",
        "-p",
        "NRestarts",
        "fjarr-agent.service",
    ])
    .unwrap_or_default();
    let mut st = AgentState::default();
    for l in out.lines() {
        match l.split_once('=') {
            Some(("ActiveState", v)) => st.active = v.to_string(),
            Some(("SubState", v)) => st.sub = v.to_string(),
            Some(("StatusText", v)) => st.status = v.to_string(),
            Some(("NRestarts", v)) => st.restarts = v.parse().unwrap_or(0),
            _ => {}
        }
    }
    st
}

/// Wait for `STATUS=online` (ADR-0019, second addendum): the agent's own word that the server
/// took its token. A restart or a failed unit in the meantime is an answer too: the journal's
/// reason, with the fix.
pub fn wait_for_agent_online(timeout: std::time::Duration) -> Result<()> {
    let start = std::time::Instant::now();
    let restarts_before = agent_state().restarts;
    let mut last = String::new();
    loop {
        let st = agent_state();
        if st.status == "online" {
            return Ok(());
        }
        if st.active == "failed" || st.restarts > restarts_before {
            bail!(
                "fjarr-agent {}{}: {}",
                if st.active == "failed" {
                    "failed"
                } else {
                    "restarted"
                },
                if st.status.is_empty() {
                    String::new()
                } else {
                    format!(" ({})", st.status)
                },
                journal_reason()
            );
        }
        if st.status != last && !st.status.is_empty() {
            cliclack::log::step(format!("fjarr-agent: {}", st.status))?;
            last = st.status.clone();
        }
        if start.elapsed() >= timeout {
            bail!(
                "fjarr-agent is not online after {} s (last status: {}); the configuration is written and the \
                 agent keeps trying. Check the server URL and the device token: journalctl -u fjarr-agent.service",
                timeout.as_secs(),
                if st.status.is_empty() { st.active.as_str() } else { st.status.as_str() }
            );
        }
        std::thread::sleep(std::time::Duration::from_millis(500));
    }
}

/// The agent's last error line, for a start that did not stay up.
fn journal_reason() -> String {
    let out = Command::new("journalctl")
        .args([
            "-u",
            "fjarr-agent.service",
            "-n",
            "40",
            "-o",
            "cat",
            "--no-pager",
        ])
        .output()
        .map(|o| String::from_utf8_lossy(&o.stdout).into_owned())
        .unwrap_or_default();
    out.lines()
        .rev()
        .find(|l| l.contains("ERROR") || l.contains("fjarr-agent:") || l.contains("auth failed"))
        .map(|l| l.trim().to_string())
        .unwrap_or_else(|| "see journalctl -u fjarr-agent.service".to_string())
}

/// The host and port a `ws://` or `wss://` URL names, for the reachability check.
pub fn ws_host_port(url: &str) -> Result<(String, u16)> {
    let (scheme, rest) = url
        .split_once("://")
        .ok_or_else(|| anyhow::anyhow!("{url:?} is not a ws:// or wss:// URL"))?;
    let default_port = match scheme {
        "ws" => 80,
        "wss" => 443,
        _ => bail!("{url:?} is not a ws:// or wss:// URL"),
    };
    let authority = rest.split(['/', '?', '#']).next().unwrap_or("");
    let authority = authority.rsplit('@').next().unwrap_or(authority);
    if authority.is_empty() {
        bail!("{url:?} names no host");
    }
    // [v6]:port, host:port, or host.
    if let Some(v6) = authority.strip_prefix('[') {
        let (host, port) = v6
            .split_once(']')
            .ok_or_else(|| anyhow::anyhow!("{url:?}: unclosed [ in the host"))?;
        let port = port
            .strip_prefix(':')
            .map(|p| p.parse::<u16>())
            .transpose()?
            .unwrap_or(default_port);
        return Ok((host.to_string(), port));
    }
    match authority.rsplit_once(':') {
        Some((host, port)) => Ok((
            host.to_string(),
            port.parse()
                .with_context(|| format!("{url:?}: port {port:?}"))?,
        )),
        None => Ok((authority.to_string(), default_port)),
    }
}

/// Can this device open a TCP connection to the server right now? Not the WebSocket handshake:
/// the agent does that, and reports it as STATUS=. This catches the typo and the closed port
/// before anything is written.
pub fn tcp_reachable(host: &str, port: u16, timeout: std::time::Duration) -> Result<()> {
    use std::net::ToSocketAddrs;
    let addrs: Vec<_> = (host, port)
        .to_socket_addrs()
        .with_context(|| format!("resolving {host}"))?
        .collect();
    let mut last = None;
    for a in addrs {
        match std::net::TcpStream::connect_timeout(&a, timeout) {
            Ok(_) => return Ok(()),
            Err(e) => last = Some(e),
        }
    }
    match last {
        Some(e) => Err(e).with_context(|| format!("connecting to {host}:{port}")),
        None => bail!("{host} resolves to no address"),
    }
}

/// `user`'s login shell from /etc/passwd, if the account is there.
pub fn login_shell(user: &str) -> Option<String> {
    std::fs::read_to_string("/etc/passwd")
        .ok()?
        .lines()
        .find_map(|l| {
            let f: Vec<&str> = l.split(':').collect();
            (f.first() == Some(&user) && f.len() >= 7).then(|| f[6].to_string())
        })
}

/// A login shell that refuses logins (`nologin`, `false`): no terminal (docs/06#fjarr.terminal).
pub fn refuses_logins(shell: &str) -> bool {
    matches!(shell.rsplit('/').next(), Some("nologin") | Some("false"))
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
    // A start-rate limit left by an earlier failure (a wrong token restarts the agent until systemd
    // gives up) would make this restart fail for a reason that is already gone.
    let _ = Command::new("systemctl")
        .args(["reset-failed", "fjarr-agent.service"])
        .status();
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

/// Run a system command (useradd, gpasswd, loginctl, dconf …); its stdout, or an error carrying its
/// stderr.
pub fn run(program: &str, args: &[&str]) -> Result<String> {
    let out = Command::new(program)
        .args(args)
        .output()
        .with_context(|| format!("running {program}"))?;
    if !out.status.success() {
        bail!(
            "{program} {} failed ({}): {}",
            args.join(" "),
            out.status,
            String::from_utf8_lossy(&out.stderr).trim()
        );
    }
    Ok(String::from_utf8_lossy(&out.stdout).into_owned())
}

/// An account's home directory and ids, from the passwd database.
pub fn passwd_entry(user: &str) -> Option<(PathBuf, libc::uid_t, libc::gid_t)> {
    std::fs::read_to_string("/etc/passwd")
        .ok()?
        .lines()
        .find_map(|l| {
            let f: Vec<&str> = l.split(':').collect();
            (f.first() == Some(&user) && f.len() >= 7).then(|| {
                (
                    PathBuf::from(f[5]),
                    f[2].parse().unwrap_or(0),
                    f[3].parse().unwrap_or(0),
                )
            })
        })
}

/// Is `user` in `group` (the group database, not the running session's view)?
pub fn in_group(user: &str, group: &str) -> bool {
    run("id", &["-nG", user])
        .map(|s| s.split_whitespace().any(|g| g == group))
        .unwrap_or(false)
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
    fn a_ws_url_yields_its_host_and_port_with_the_schemes_default() {
        assert_eq!(
            ws_host_port("ws://192.168.10.188:8080/ws").unwrap(),
            ("192.168.10.188".into(), 8080)
        );
        assert_eq!(
            ws_host_port("wss://fleet.acme.com/ws").unwrap(),
            ("fleet.acme.com".into(), 443)
        );
        assert_eq!(
            ws_host_port("ws://fjarr-server/ws?x=1").unwrap(),
            ("fjarr-server".into(), 80)
        );
        assert_eq!(
            ws_host_port("wss://[::1]:9443/ws").unwrap(),
            ("::1".into(), 9443)
        );
        assert!(ws_host_port("https://fleet.acme.com").is_err());
        assert!(ws_host_port("ws://").is_err());
    }

    #[test]
    fn an_unreachable_port_is_reported_not_hung() {
        // Port 9 (discard) is closed on every lab machine; the check fails fast and names it.
        let e = tcp_reachable("127.0.0.1", 9, std::time::Duration::from_secs(2))
            .unwrap_err()
            .to_string();
        assert!(e.contains("127.0.0.1:9"), "{e}");
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
