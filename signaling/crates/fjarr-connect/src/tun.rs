//! The operator's tunnel interface: **one** device carrying a /32 route per attached robot, so a
//! long-running ROS 2 node keeps working as robots come and go (docs/27#the-shape).
//!
//! Attaching never creates. The device is created and addressed only when it is not already there,
//! which keeps the common case — a device that exists, as on a robot — free of privilege: the same
//! property the agent is measured against (docs/27#lifecycle). Creating one needs `CAP_NET_ADMIN`,
//! from `setcap cap_net_admin+ep` at install or from `sudo`, and when it is missing this says which
//! two commands fix it rather than failing with `EPERM`.
//!
//! spec: docs/27-network-tunnel.md#the-shape · docs/27-network-tunnel.md#the-packet-path
use std::net::Ipv4Addr;
use std::os::fd::{AsRawFd, FromRawFd, OwnedFd, RawFd};
use std::process::Command;

use anyhow::{anyhow, bail, Context, Result};
use tokio::io::unix::AsyncFd;

/// `IFNAMSIZ` — the kernel's limit on an interface name, including its terminator.
const IFNAMSIZ: usize = 16;

pub struct Tun {
    fd: AsyncFd<OwnedFd>,
    name: String,
    mtu: usize,
}

/// How the device came to be, for the line the CLI prints: an operator who did not create it should
/// not be told that it was.
#[derive(Debug, PartialEq, Eq)]
pub enum Provenance {
    /// It already existed, addressed, and this process needed no privilege.
    Existing,
    /// This process created and addressed it.
    Created,
}

impl Tun {
    /// Make sure `name` exists with `address` and `mtu`, then attach to it.
    pub fn open(name: &str, address: Ipv4Addr, mtu: usize) -> Result<(Tun, Provenance)> {
        if name.len() >= IFNAMSIZ {
            bail!("the interface name {name:?} is longer than the kernel's {IFNAMSIZ} bytes");
        }
        let provenance = ensure_device(name, address, mtu)?;
        let tun = attach(name, mtu)?;
        Ok((tun, provenance))
    }

    pub fn name(&self) -> &str {
        &self.name
    }

    pub fn mtu(&self) -> usize {
        self.mtu
    }

    /// Route `robot` down this interface. One /32 per link, and nothing that would route between
    /// two of them (docs/27#isolation).
    pub fn add_route(&self, robot: Ipv4Addr) -> Result<()> {
        add_route(&self.name, robot)
    }

    /// One packet from the kernel. `None` means the read would block; the caller is expected to be
    /// inside `readable()`.
    pub fn try_read(&self, buf: &mut [u8]) -> Result<Option<usize>> {
        // SAFETY: `buf` is valid for `buf.len()` bytes, and the fd is owned by this struct.
        let n = unsafe { libc::read(self.fd.as_raw_fd(), buf.as_mut_ptr().cast(), buf.len()) };
        if n >= 0 {
            return Ok(Some(n as usize));
        }
        let err = std::io::Error::last_os_error();
        match err.kind() {
            std::io::ErrorKind::WouldBlock | std::io::ErrorKind::Interrupted => Ok(None),
            _ => Err(err).with_context(|| format!("reading from {}", self.name)),
        }
    }

    /// Inject a packet into the kernel's receive path. A write can fail under load (`ENOBUFS` on a
    /// full device queue), which the caller counts: a silent failure here looks exactly like a
    /// stalled transfer from the outside and nothing else records it.
    pub fn try_write(&self, packet: &[u8]) -> Result<bool> {
        // SAFETY: `packet` is valid for `packet.len()` bytes, and the fd is owned by this struct.
        let n = unsafe { libc::write(self.fd.as_raw_fd(), packet.as_ptr().cast(), packet.len()) };
        if n > 0 {
            return Ok(true);
        }
        let err = std::io::Error::last_os_error();
        match err.kind() {
            std::io::ErrorKind::WouldBlock
            | std::io::ErrorKind::Interrupted
            | std::io::ErrorKind::OutOfMemory => Ok(false),
            _ if err.raw_os_error() == Some(libc::ENOBUFS) => Ok(false),
            _ => Err(err).with_context(|| format!("writing to {}", self.name)),
        }
    }

    /// Wait until the device has something to read.
    pub async fn readable(&self) -> Result<()> {
        let mut guard = self.fd.readable().await?;
        guard.clear_ready();
        Ok(())
    }
}

#[cfg(target_os = "linux")]
fn attach(name: &str, mtu: usize) -> Result<Tun> {
    use std::ffi::CString;

    let path = CString::new("/dev/net/tun").expect("no interior nul");
    // SAFETY: a constant, valid, nul-terminated path.
    let raw = unsafe { libc::open(path.as_ptr(), libc::O_RDWR | libc::O_CLOEXEC) };
    if raw < 0 {
        return Err(std::io::Error::last_os_error())
            .context("opening /dev/net/tun (in a container it has to be passed in as a device)");
    }
    // SAFETY: `raw` is a fresh, owned descriptor.
    let fd = unsafe { OwnedFd::from_raw_fd(raw) };

    // `struct ifreq` is larger than the two fields used here, and the kernel reads all of it, so
    // the whole thing is zeroed first.
    #[repr(C)]
    struct IfReq {
        name: [libc::c_char; IFNAMSIZ],
        flags: libc::c_short,
        _pad: [u8; 22],
    }
    let mut req = IfReq {
        name: [0; IFNAMSIZ],
        // No packet-information header: one read is one bare IP packet, which is what rides the
        // channel (docs/27#the-packet-path).
        flags: (libc::IFF_TUN | libc::IFF_NO_PI) as libc::c_short,
        _pad: [0; 22],
    };
    for (slot, byte) in req.name.iter_mut().zip(name.as_bytes()) {
        *slot = *byte as libc::c_char;
    }
    // TUNSETIFF: _IOW('T', 202, int).
    const TUNSETIFF: libc::c_ulong = 0x4004_54ca;
    // SAFETY: `req` is a correctly shaped, zero-initialised `ifreq` for this ioctl.
    if unsafe { libc::ioctl(fd.as_raw_fd(), TUNSETIFF, &mut req) } < 0 {
        return Err(std::io::Error::last_os_error())
            .with_context(|| format!("attaching to {name}"));
    }
    set_nonblocking(fd.as_raw_fd())?;
    Ok(Tun {
        fd: AsyncFd::new(fd)?,
        name: name.to_string(),
        mtu,
    })
}

fn set_nonblocking(fd: RawFd) -> Result<()> {
    // SAFETY: `fd` is open and owned by the caller for the duration of these two calls.
    let flags = unsafe { libc::fcntl(fd, libc::F_GETFL) };
    if flags < 0 || unsafe { libc::fcntl(fd, libc::F_SETFL, flags | libc::O_NONBLOCK) } < 0 {
        return Err(std::io::Error::last_os_error()).context("setting O_NONBLOCK on the device");
    }
    Ok(())
}

/// Run `ip`, returning its output. Shelling out rather than speaking netlink: this is three
/// commands on the setup path, run once per link, and `ip` is on every host that has a TUN device.
#[cfg(target_os = "linux")]
fn ip(args: &[&str]) -> Result<String> {
    let out = Command::new("ip")
        .args(args)
        .output()
        .with_context(|| format!("running `ip {}`", args.join(" ")))?;
    if !out.status.success() {
        let stderr = String::from_utf8_lossy(&out.stderr).trim().to_string();
        bail!("`ip {}` failed: {stderr}", args.join(" "));
    }
    Ok(String::from_utf8_lossy(&out.stdout).to_string())
}

#[cfg(target_os = "linux")]
fn exists(name: &str) -> bool {
    std::path::Path::new(&format!("/sys/class/net/{name}")).exists()
}

#[cfg(target_os = "linux")]
fn has_address(name: &str, address: Ipv4Addr) -> bool {
    ip(&["-o", "-4", "addr", "show", "dev", name])
        .map(|out| out.contains(&format!("inet {address}")))
        .unwrap_or(false)
}

/// Create and address the device when it is not already there. Never re-addresses an existing one:
/// flushing an address is exactly what breaks participants already bound to it
/// (docs/27#lifecycle), and on the operator side it would break the other robots' links.
#[cfg(target_os = "linux")]
fn ensure_device(name: &str, address: Ipv4Addr, mtu: usize) -> Result<Provenance> {
    if exists(name) && has_address(name, address) {
        return Ok(Provenance::Existing);
    }
    let user = current_user();
    let created = !exists(name);
    let steps: Vec<Vec<String>> = {
        let mut s: Vec<Vec<String>> = Vec::new();
        if created {
            s.push(
                ["tuntap", "add", "dev", name, "mode", "tun", "user", &user]
                    .iter()
                    .map(|a| a.to_string())
                    .collect(),
            );
        }
        s.push(
            ["addr", "add", &format!("{address}/32"), "dev", name]
                .iter()
                .map(|a| a.to_string())
                .collect(),
        );
        s.push(
            ["link", "set", name, "mtu", &mtu.to_string(), "up"]
                .iter()
                .map(|a| a.to_string())
                .collect(),
        );
        s
    };
    for step in &steps {
        let args: Vec<&str> = step.iter().map(String::as_str).collect();
        if let Err(e) = ip(&args) {
            return Err(e.context(privilege_hint(name, address, mtu, &user)));
        }
    }
    Ok(if created {
        Provenance::Created
    } else {
        Provenance::Existing
    })
}

/// What to do about an `ip` command that was refused. Printed as the error's context, because
/// `Operation not permitted` on its own sends people to the wrong place.
#[cfg(target_os = "linux")]
fn privilege_hint(name: &str, address: Ipv4Addr, mtu: usize, user: &str) -> String {
    format!(
        "setting up {name} needs CAP_NET_ADMIN. Either grant it once to the binary:\n  \
         sudo setcap cap_net_admin+ep $(command -v fjarr-connect)\n\
         or create the interface once, as root, and run unprivileged from then on:\n  \
         sudo ip tuntap add dev {name} mode tun user {user}\n  \
         sudo ip addr add {address}/32 dev {name}\n  \
         sudo ip link set {name} mtu {mtu} up"
    )
}

#[cfg(target_os = "linux")]
fn add_route(name: &str, robot: Ipv4Addr) -> Result<()> {
    // An existing route is the normal case on a point-to-point device whose address names the peer,
    // and re-adding it is an error rather than a no-op, so look first.
    let shown = ip(&["-o", "route", "show", &format!("{robot}/32")]).unwrap_or_default();
    if shown.contains(&format!("dev {name}")) {
        return Ok(());
    }
    if !shown.trim().is_empty() {
        bail!(
            "{robot} is already routed somewhere else ({}), so this link would not be used",
            shown.trim()
        );
    }
    ip(&["route", "add", &format!("{robot}/32"), "dev", name]).map(|_| ())
}

#[cfg(target_os = "linux")]
fn del_route(name: &str, robot: Ipv4Addr) -> Result<()> {
    ip(&["route", "del", &format!("{robot}/32"), "dev", name]).map(|_| ())
}

/// Best-effort route removal for the teardown path, where a failure must not mask the reason the
/// link ended.
pub fn drop_route(name: &str, robot: Ipv4Addr) {
    if let Err(e) = del_route(name, robot) {
        tracing::debug!(error = %e, %robot, "the route outlived the link");
    }
}

fn current_user() -> String {
    // SAFETY: getuid cannot fail.
    let uid = unsafe { libc::getuid() };
    std::env::var("USER").unwrap_or_else(|_| uid.to_string())
}

/// The address this end uses, as the agent derived it. Parsed rather than assumed: the operator
/// address is the range's base + 1 and the range is configurable, so the robot is the authority on
/// what it is (docs/27#addressing).
pub fn parse_address(field: &str) -> Result<Ipv4Addr> {
    field
        .split('/')
        .next()
        .unwrap_or(field)
        .parse()
        .map_err(|_| anyhow!("{field:?} is not an IPv4 address"))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn an_address_is_read_with_or_without_a_prefix() {
        assert_eq!(
            parse_address("100.64.0.1").unwrap(),
            Ipv4Addr::new(100, 64, 0, 1)
        );
        assert_eq!(
            parse_address("100.70.118.224/32").unwrap(),
            Ipv4Addr::new(100, 70, 118, 224)
        );
        assert!(parse_address("?").is_err());
        assert!(parse_address("").is_err());
    }

    /// A name the kernel would silently truncate is refused before anything is created, because a
    /// truncated name attaches to the wrong device.
    #[test]
    fn an_over_long_interface_name_is_refused() {
        let err = Tun::open("fjarr-far-too-long", Ipv4Addr::new(100, 64, 0, 1), 1280)
            .err()
            .expect("a 18-byte name cannot be an interface");
        assert!(format!("{err}").contains("longer than"), "{err}");
    }
}
