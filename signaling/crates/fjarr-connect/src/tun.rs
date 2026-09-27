//! The operator's tunnel interface: **one** device carrying a /32 route per attached robot, so a
//! long-running ROS 2 node keeps working as robots come and go (docs/27#the-shape).
//!
//! Attaching never creates. The device is created and addressed only when it is not already there,
//! which keeps the common case — a device that exists, as on a robot — free of privilege: the same
//! property the agent is measured against (docs/27#lifecycle). Creating one needs `CAP_NET_ADMIN`,
//! from `setcap cap_net_admin+ep` at install or from `sudo`, and when it is missing this says which
//! commands fix it rather than failing with `EPERM`.
//!
//! Every change to the host's network configuration is made **in this process**, over netlink. That
//! is not a preference: a file capability is not inherited by a child process, so a client that
//! shelled out to `ip` would work under `sudo` and fail under `setcap` with `Operation not
//! permitted` — which is exactly what the first version did, and docs/27 promises setcap. Doing it
//! here also means the binary needs no `iproute2` on the host, which is the point of shipping one
//! static file (ADR-0024).
//!
//! spec: docs/27-network-tunnel.md#the-shape · docs/27-network-tunnel.md#the-packet-path
use std::net::Ipv4Addr;
use std::os::fd::{AsRawFd, FromRawFd, OwnedFd, RawFd};

use anyhow::{anyhow, bail, Context, Result};
use tokio::io::unix::AsyncFd;

/// `IFNAMSIZ` — the kernel's limit on an interface name, including its terminator.
const IFNAMSIZ: usize = 16;

pub struct Tun {
    fd: AsyncFd<OwnedFd>,
    name: String,
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
    pub async fn open(name: &str, address: Ipv4Addr, mtu: usize) -> Result<(Tun, Provenance)> {
        if name.len() >= IFNAMSIZ {
            bail!("the interface name {name:?} is longer than the kernel's {IFNAMSIZ} bytes");
        }
        let provenance = ensure_device(name, address, mtu).await?;
        let tun = attach(name)?;
        Ok((tun, provenance))
    }

    pub fn name(&self) -> &str {
        &self.name
    }

    /// Route `robot` down this interface. One /32 per link, and nothing that would route between
    /// two of them (docs/27#isolation).
    pub async fn add_route(&self, robot: Ipv4Addr) -> Result<()> {
        add_route(&self.name, robot).await
    }

    /// One packet from the kernel. `None` means the read would block; the caller is expected to be
    /// inside `readable()`.
    pub fn try_read(&self, buf: &mut [u8]) -> Result<Option<usize>> {
        // SAFETY: `buf` is valid for `buf.len()` bytes, and the fd is owned by this struct.
        let n = unsafe { libc::read(self.fd.as_raw_fd(), buf.as_mut_ptr().cast(), buf.len()) };
        if n >= 0 {
            let n = n as usize;
            #[cfg(target_os = "macos")]
            {
                // utun hands over a 4-byte address family first; the channel carries bare packets.
                if n < AF_HEADER {
                    return Ok(Some(0));
                }
                buf.copy_within(AF_HEADER..n, 0);
                return Ok(Some(n - AF_HEADER));
            }
            #[cfg(not(target_os = "macos"))]
            return Ok(Some(n));
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
        // utun expects the address family in front of every packet; Linux's IFF_NO_PI does not.
        #[cfg(target_os = "macos")]
        let framed = {
            let mut framed = Vec::with_capacity(AF_HEADER + packet.len());
            framed.extend_from_slice(&(libc::AF_INET as u32).to_be_bytes());
            framed.extend_from_slice(packet);
            framed
        };
        #[cfg(target_os = "macos")]
        let packet = framed.as_slice();
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
fn attach(name: &str) -> Result<Tun> {
    let fd = open_tun(name)?;
    set_nonblocking(fd.as_raw_fd())?;
    Ok(Tun {
        fd: AsyncFd::new(fd)?,
        name: name.to_string(),
    })
}

/// `TUNSETIFF` against `/dev/net/tun`: attaches to `name`, or creates it when it does not exist —
/// which is the one operation here that is the same call either way.
#[cfg(target_os = "linux")]
fn open_tun(name: &str) -> Result<OwnedFd> {
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

    // `struct ifreq` is larger than the two fields used here, and the kernel reads all of it, so the
    // whole thing is zeroed first.
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
    Ok(fd)
}

/// Make a freshly created device outlive this process, owned by whoever is running. The interface and
/// its address never changing is what lets a long-running ROS 2 node keep working as robots come and
/// go (docs/27#the-shape), and it is also what makes the next run need no privilege at all.
#[cfg(target_os = "linux")]
fn make_persistent(fd: &OwnedFd) -> Result<()> {
    // TUNSETPERSIST: _IOW('T', 203, int); TUNSETOWNER: _IOW('T', 204, int).
    const TUNSETPERSIST: libc::c_ulong = 0x4004_54cb;
    const TUNSETOWNER: libc::c_ulong = 0x4004_54cc;
    // SAFETY: both take an int by value on an fd this process owns.
    unsafe {
        if libc::ioctl(fd.as_raw_fd(), TUNSETPERSIST, 1) < 0 {
            return Err(std::io::Error::last_os_error()).context("making the device persistent");
        }
        if libc::ioctl(fd.as_raw_fd(), TUNSETOWNER, libc::getuid() as libc::c_int) < 0 {
            return Err(std::io::Error::last_os_error()).context("setting the device's owner");
        }
    }
    Ok(())
}

fn set_nonblocking(fd: RawFd) -> Result<()> {
    // SAFETY: `fd` is open and owned by the caller for the duration of these two calls.
    let flags = unsafe { libc::fcntl(fd, libc::F_GETFL) };
    if flags < 0 || unsafe { libc::fcntl(fd, libc::F_SETFL, flags | libc::O_NONBLOCK) } < 0 {
        return Err(std::io::Error::last_os_error()).context("setting O_NONBLOCK on the device");
    }
    Ok(())
}

/// A netlink connection, for as long as the caller needs it. Each call opens its own rather than
/// holding one: this happens a handful of times per link, and a socket kept open for the life of the
/// process would be one more thing to reason about in the pump.
#[cfg(target_os = "linux")]
async fn netlink() -> Result<rtnetlink::Handle> {
    let (connection, handle, _) = rtnetlink::new_connection().context("opening netlink")?;
    tokio::spawn(connection);
    Ok(handle)
}

#[cfg(target_os = "linux")]
async fn index_of(handle: &rtnetlink::Handle, name: &str) -> Result<Option<u32>> {
    use futures_util::TryStreamExt;
    let mut links = handle.link().get().match_name(name.to_string()).execute();
    match links.try_next().await {
        Ok(Some(link)) => Ok(Some(link.header.index)),
        Ok(None) => Ok(None),
        // "no such device" is an answer, not a failure.
        Err(rtnetlink::Error::NetlinkError(e)) if e.raw_code() == -libc::ENODEV => Ok(None),
        Err(e) => Err(e).with_context(|| format!("looking up {name}")),
    }
}

#[cfg(target_os = "linux")]
async fn has_address(handle: &rtnetlink::Handle, index: u32, address: Ipv4Addr) -> Result<bool> {
    use futures_util::TryStreamExt;
    use rtnetlink::packet_route::address::AddressAttribute;

    let mut addrs = handle
        .address()
        .get()
        .set_link_index_filter(index)
        .execute();
    while let Some(msg) = addrs
        .try_next()
        .await
        .context("reading the interface's addresses")?
    {
        for attr in &msg.attributes {
            // Both, deliberately. On a point-to-point address — the shape the installer creates,
            // `ip addr add <self> peer <robot>` — `IFA_ADDRESS` is the *peer* and the local end is
            // `IFA_LOCAL`; on an ordinary address they are the same. Checking only `Address` missed
            // the lab's device, tried to add an address it already had, and turned the documented
            // no-privilege attach into an EPERM. The setcap'd lab build had been hiding it.
            match attr {
                AddressAttribute::Address(std::net::IpAddr::V4(a))
                | AddressAttribute::Local(std::net::IpAddr::V4(a))
                    if *a == address =>
                {
                    return Ok(true)
                }
                _ => {}
            }
        }
    }
    Ok(false)
}

/// Create and address the device when it is not already there. Never re-addresses an existing one:
/// flushing an address is exactly what breaks participants already bound to it
/// (docs/27#lifecycle), and on the operator side it would break the other robots' links too.
#[cfg(target_os = "linux")]
async fn ensure_device(name: &str, address: Ipv4Addr, mtu: usize) -> Result<Provenance> {
    let handle = netlink().await?;
    let existing = index_of(&handle, name).await?;
    if let Some(index) = existing {
        if has_address(&handle, index, address).await? {
            return Ok(Provenance::Existing);
        }
    }

    // Creating it is the same ioctl as attaching to it; what makes it outlive this process is
    // TUNSETPERSIST. The fd is dropped straight after, because the pump attaches its own.
    let created = existing.is_none();
    if created {
        let fd = open_tun(name).map_err(|e| e.context(privilege_hint(name, address, mtu)))?;
        make_persistent(&fd).map_err(|e| e.context(privilege_hint(name, address, mtu)))?;
    }
    let index = index_of(&handle, name)
        .await?
        .ok_or_else(|| anyhow!("{name} does not exist even after creating it"))?;

    handle
        .address()
        .add(index, std::net::IpAddr::V4(address), 32)
        .execute()
        .await
        .map_err(|e| anyhow!(e).context(privilege_hint(name, address, mtu)))
        .with_context(|| format!("adding {address}/32 to {name}"))?;

    let up = rtnetlink::LinkMessageBuilder::<rtnetlink::LinkUnspec>::new()
        .index(index)
        .mtu(mtu as u32)
        .up()
        .build();
    handle
        .link()
        .set(up)
        .execute()
        .await
        .map_err(|e| anyhow!(e).context(privilege_hint(name, address, mtu)))
        .with_context(|| format!("bringing {name} up with mtu {mtu}"))?;

    Ok(if created {
        Provenance::Created
    } else {
        Provenance::Existing
    })
}

/// What to do about a change the kernel refused. Printed as the error's context, because `Operation
/// not permitted` on its own sends people to the wrong place.
#[cfg(target_os = "linux")]
fn privilege_hint(name: &str, address: Ipv4Addr, mtu: usize) -> String {
    format!(
        "setting up {name} needs CAP_NET_ADMIN. Either grant it once to the binary:\n  \
         sudo setcap cap_net_admin+ep $(command -v fjarr-connect)\n\
         or create the interface once, as root, and run unprivileged from then on:\n  \
         sudo ip tuntap add dev {name} mode tun user {}\n  \
         sudo ip addr add {address}/32 dev {name}\n  \
         sudo ip link set {name} mtu {mtu} up",
        current_user()
    )
}

#[cfg(target_os = "linux")]
fn route_to(index: u32, robot: Ipv4Addr) -> rtnetlink::packet_route::route::RouteMessage {
    rtnetlink::RouteMessageBuilder::<Ipv4Addr>::new()
        .destination_prefix(robot, 32)
        .output_interface(index)
        .build()
}

/// Is `robot/32` already routed down interface `index`? Asked before any write, because for an
/// unprivileged process the kernel answers RTM_NEWROUTE with EPERM before it would ever say
/// EEXIST — and on a point-to-point device whose address names the peer, the kernel installed this
/// exact route with the address. That is the installer's shape, and the documented no-privilege
/// attach depends on recognising it by looking rather than by trying (docs/27#the-operator-client).
#[cfg(target_os = "linux")]
async fn route_exists(handle: &rtnetlink::Handle, index: u32, robot: Ipv4Addr) -> Result<bool> {
    use futures_util::TryStreamExt;
    use rtnetlink::packet_route::route::{RouteAddress, RouteAttribute};

    let all = rtnetlink::RouteMessageBuilder::<Ipv4Addr>::new().build();
    let mut routes = handle.route().get(all).execute();
    while let Some(msg) = routes.try_next().await.context("listing routes")? {
        if msg.header.destination_prefix_length != 32 {
            continue;
        }
        let mut dst_matches = false;
        let mut oif_matches = false;
        for attr in &msg.attributes {
            match attr {
                RouteAttribute::Destination(RouteAddress::Inet(a)) if *a == robot => {
                    dst_matches = true
                }
                RouteAttribute::Oif(i) if *i == index => oif_matches = true,
                _ => {}
            }
        }
        if dst_matches && oif_matches {
            return Ok(true);
        }
    }
    Ok(false)
}

#[cfg(target_os = "linux")]
async fn add_route(name: &str, robot: Ipv4Addr) -> Result<()> {
    let handle = netlink().await?;
    let index = index_of(&handle, name)
        .await?
        .ok_or_else(|| anyhow!("{name} is gone"))?;
    if route_exists(&handle, index, robot).await? {
        return Ok(());
    }
    match handle.route().add(route_to(index, robot)).execute().await {
        Ok(()) => Ok(()),
        // An existing route is the normal case on a point-to-point device whose address names the
        // peer: the kernel installed it with the address. Two robots cannot collide here, because a
        // colliding pair is refused before anything is routed (docs/27#addressing).
        Err(rtnetlink::Error::NetlinkError(e)) if e.raw_code() == -libc::EEXIST => Ok(()),
        Err(e) => Err(anyhow!(e))
            .context(privilege_hint(name, robot, 1280))
            .with_context(|| format!("routing {robot} down {name}")),
    }
}

#[cfg(target_os = "linux")]
async fn del_route(name: &str, robot: Ipv4Addr) -> Result<()> {
    let handle = netlink().await?;
    let index = index_of(&handle, name)
        .await?
        .ok_or_else(|| anyhow!("{name} is gone"))?;
    handle
        .route()
        .del(route_to(index, robot))
        .execute()
        .await
        .map_err(|e| anyhow!(e))
        .with_context(|| format!("removing the route to {robot}"))
}

/// Best-effort route removal for the teardown path, where a failure must not mask the reason the
/// link ended.
#[cfg(target_os = "linux")]
pub async fn drop_route(name: &str, robot: Ipv4Addr) {
    if let Err(e) = del_route(name, robot).await {
        tracing::debug!(error = %e, %robot, "the route outlived the link");
    }
}

// ------------------------------------------------------------------- macOS --
//
// **Written against Apple's documented interfaces and cross-compiled, never run on macOS hardware**
// (docs/04, docs/17 4.5e). Three things differ from Linux and each one is a place this could be
// wrong on a real machine:
//
// 1. There is no `/dev/net/tun`. A utun interface is a `PF_SYSTEM` control socket, and its name is
//    chosen by the kernel from the unit number — `utun3` is unit 4 — so `--dev utunN` selects a unit
//    rather than naming a device, and unit 0 asks the kernel for the first free one.
// 2. **Every read and write carries a 4-byte address-family header**, which Linux's `IFF_NO_PI`
//    removes. The channel carries bare IP packets (docs/27#the-packet-path), so that header is
//    stripped on the way out and prepended on the way in.
// 3. There are no file capabilities, so there is no `setcap` equivalent: configuring an interface
//    needs root, and under `sudo` a child process inherits it. `ifconfig` and `route` are therefore
//    the honest mechanism here, unlike on Linux where using them would have broken the documented
//    install.
#[cfg(target_os = "macos")]
const AF_HEADER: usize = 4;

#[cfg(target_os = "macos")]
fn attach(name: &str) -> Result<Tun> {
    let unit = utun_unit(name)?;
    let fd = open_utun(unit)?;
    set_nonblocking(fd.as_raw_fd())?;
    Ok(Tun {
        fd: AsyncFd::new(fd)?,
        name: format!("utun{}", unit - 1),
    })
}

/// `utun3` is unit 4; `utun` on its own means "whichever is free".
#[cfg(target_os = "macos")]
fn utun_unit(name: &str) -> Result<u32> {
    let digits = name.trim_start_matches("utun");
    if !name.starts_with("utun") {
        bail!("on macOS the interface has to be a utun, not {name:?} — pass --dev utun9 or --dev utun");
    }
    if digits.is_empty() {
        return Ok(0);
    }
    let n: u32 = digits
        .parse()
        .map_err(|_| anyhow!("{name:?} is not a utun number"))?;
    Ok(n + 1)
}

#[cfg(target_os = "macos")]
fn open_utun(unit: u32) -> Result<OwnedFd> {
    const UTUN_CONTROL_NAME: &[u8] = b"com.apple.net.utun_control\0";
    const CTLIOCGINFO: libc::c_ulong = 0xc064_4e03;

    #[repr(C)]
    struct CtlInfo {
        id: u32,
        name: [u8; 96],
    }

    // SAFETY: a plain socket call; the constants are Apple's documented values.
    let raw = unsafe { libc::socket(libc::PF_SYSTEM, libc::SOCK_DGRAM, libc::SYSPROTO_CONTROL) };
    if raw < 0 {
        return Err(std::io::Error::last_os_error()).context("opening a PF_SYSTEM control socket");
    }
    // SAFETY: `raw` is a fresh, owned descriptor.
    let fd = unsafe { OwnedFd::from_raw_fd(raw) };

    let mut info = CtlInfo {
        id: 0,
        name: [0; 96],
    };
    info.name[..UTUN_CONTROL_NAME.len()].copy_from_slice(UTUN_CONTROL_NAME);
    // SAFETY: `info` is the shape CTLIOCGINFO expects, with a nul-terminated name.
    if unsafe { libc::ioctl(fd.as_raw_fd(), CTLIOCGINFO, &mut info) } < 0 {
        return Err(std::io::Error::last_os_error()).context("resolving the utun control id");
    }

    let addr = libc::sockaddr_ctl {
        sc_len: std::mem::size_of::<libc::sockaddr_ctl>() as u8,
        sc_family: libc::AF_SYSTEM as u8,
        ss_sysaddr: libc::AF_SYS_CONTROL as u16,
        sc_id: info.id,
        sc_unit: unit,
        sc_reserved: [0; 5],
    };
    // SAFETY: a correctly sized sockaddr_ctl for a PF_SYSTEM socket.
    if unsafe {
        libc::connect(
            fd.as_raw_fd(),
            &addr as *const libc::sockaddr_ctl as *const libc::sockaddr,
            std::mem::size_of::<libc::sockaddr_ctl>() as libc::socklen_t,
        )
    } < 0
    {
        let err = std::io::Error::last_os_error();
        return Err(err).context(
            "attaching to the utun unit (creating one needs root: run under sudo — macOS has no \
             setcap equivalent)",
        );
    }
    Ok(fd)
}

/// Run a configuration command. Shelling out is correct on macOS and only on macOS: there are no file
/// capabilities to fail to inherit, so a process that can configure an interface at all is root, and
/// its children are too.
#[cfg(target_os = "macos")]
fn run(program: &str, args: &[&str]) -> Result<()> {
    let out = std::process::Command::new(program)
        .args(args)
        .output()
        .with_context(|| format!("running `{program} {}`", args.join(" ")))?;
    if !out.status.success() {
        let stderr = String::from_utf8_lossy(&out.stderr).trim().to_string();
        bail!("`{program} {}` failed: {stderr}", args.join(" "));
    }
    Ok(())
}

#[cfg(target_os = "macos")]
async fn ensure_device(name: &str, address: Ipv4Addr, mtu: usize) -> Result<Provenance> {
    // A utun exists only while something holds its socket, so there is no persistent device to find
    // and no unprivileged attach to a device someone else made: the process that attaches is the
    // process that creates. `Created` is the only honest answer here.
    let unit = utun_unit(name)?;
    let fd = open_utun(unit)?;
    let dev = format!("utun{}", unit.saturating_sub(1));
    // A point-to-point address whose peer is this machine: the same shape `tundev.sh` gives the lab's
    // Linux interface, and what the /32 routes below hang off.
    run(
        "ifconfig",
        &[
            &dev,
            "inet",
            &address.to_string(),
            &address.to_string(),
            "netmask",
            "255.255.255.255",
            "mtu",
            &mtu.to_string(),
            "up",
        ],
    )?;
    // Held open until the pump attaches its own: dropping it now would take the interface with it.
    std::mem::forget(fd);
    Ok(Provenance::Created)
}

#[cfg(target_os = "macos")]
async fn add_route(name: &str, robot: Ipv4Addr) -> Result<()> {
    run(
        "route",
        &["-n", "add", "-host", &robot.to_string(), "-interface", name],
    )
}

#[cfg(target_os = "macos")]
async fn del_route(name: &str, robot: Ipv4Addr) -> Result<()> {
    run(
        "route",
        &[
            "-n",
            "delete",
            "-host",
            &robot.to_string(),
            "-interface",
            name,
        ],
    )
}

#[cfg(target_os = "macos")]
pub async fn drop_route(name: &str, robot: Ipv4Addr) {
    if let Err(e) = del_route(name, robot).await {
        tracing::debug!(error = %e, %robot, "the route outlived the link");
    }
}

/// Only the Linux privilege hint needs this: macOS has no `setcap` equivalent, so its advice is
/// "run under sudo" and does not depend on who you are.
#[cfg(target_os = "linux")]
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
    #[tokio::test]
    async fn an_over_long_interface_name_is_refused() {
        let err = Tun::open("fjarr-far-too-long", Ipv4Addr::new(100, 64, 0, 1), 1280)
            .await
            .err()
            .expect("a 18-byte name cannot be an interface");
        assert!(format!("{err}").contains("longer than"), "{err}");
    }
}
