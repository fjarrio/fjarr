//! The persistent tun device, created and addressed **in this process** over netlink (docs/27).
//!
//! Shared by `fjarr-connect` (the operator's device, owned by whoever runs it, with a /32 route per
//! attached device) and `fjarr-setup` (the device side's `fjarr0`, created at every boot by
//! `fjarr-net.service`, owned by the agent's account, addressed point-to-point with the operator as
//! its peer — the shape the agent attaches to without privilege). In-process, not `ip`: a file
//! capability is not inherited by a child process (docs/27#the-operator-client).
//!
//! spec: docs/27-network-tunnel.md#lifecycle · docs/26-robot-install-and-drivers.md#the-setup-tool
#![cfg(target_os = "linux")]

use std::net::Ipv4Addr;
use std::os::fd::{AsRawFd, FromRawFd, OwnedFd};

use anyhow::{anyhow, Context, Result};

/// `IFNAMSIZ` — the kernel's limit on an interface name, including its terminator.
pub const IFNAMSIZ: usize = 16;

/// How the device came to be: a caller that did not create it should not say it did.
#[derive(Debug, PartialEq, Eq)]
pub enum Provenance {
    /// It already existed with the address, and nothing was changed.
    Existing,
    /// This process created and addressed it.
    Created,
}

/// `TUNSETIFF` against `/dev/net/tun`: attaches to `name`, or creates it when it does not exist —
/// the one operation here that is the same call either way.
pub fn open_tun(name: &str) -> Result<OwnedFd> {
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

/// Make a freshly created device outlive this process, owned by `owner`. The interface and its
/// address never changing is what lets a long-running ROS 2 node keep working as links come and go
/// (docs/27#the-shape), and the owner is who can attach to it later with no privilege at all.
pub fn make_persistent(fd: &OwnedFd, owner: libc::uid_t) -> Result<()> {
    // TUNSETPERSIST: _IOW('T', 203, int); TUNSETOWNER: _IOW('T', 204, int).
    const TUNSETPERSIST: libc::c_ulong = 0x4004_54cb;
    const TUNSETOWNER: libc::c_ulong = 0x4004_54cc;
    // SAFETY: both take an int by value on an fd this process owns.
    unsafe {
        if libc::ioctl(fd.as_raw_fd(), TUNSETPERSIST, 1) < 0 {
            return Err(std::io::Error::last_os_error()).context("making the device persistent");
        }
        if libc::ioctl(fd.as_raw_fd(), TUNSETOWNER, owner as libc::c_int) < 0 {
            return Err(std::io::Error::last_os_error()).context("setting the device's owner");
        }
    }
    Ok(())
}

/// A netlink connection, for as long as the caller needs it.
pub async fn netlink() -> Result<rtnetlink::Handle> {
    let (connection, handle, _) = rtnetlink::new_connection().context("opening netlink")?;
    tokio::spawn(connection);
    Ok(handle)
}

pub async fn index_of(handle: &rtnetlink::Handle, name: &str) -> Result<Option<u32>> {
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

/// Does interface `index` carry `address`? Checks both attributes, deliberately: on a point-to-point
/// address — the shape the device side uses, `<self> peer <operator>` — `IFA_ADDRESS` is the *peer*
/// and the local end is `IFA_LOCAL`; on an ordinary address they are the same. Checking only one
/// once turned the documented no-privilege attach into an EPERM.
pub async fn has_address(
    handle: &rtnetlink::Handle,
    index: u32,
    address: Ipv4Addr,
) -> Result<bool> {
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

/// Make sure `name` exists as a persistent tun owned by `owner`, carrying `local` (point-to-point
/// with `peer` when given), at `mtu` and up. Never re-addresses a device that already has `local`:
/// flushing an address is exactly what breaks participants already bound to it (docs/27#lifecycle).
pub async fn ensure_tun(
    name: &str,
    owner: libc::uid_t,
    local: Ipv4Addr,
    peer: Option<Ipv4Addr>,
    mtu: usize,
) -> Result<Provenance> {
    if name.len() >= IFNAMSIZ {
        anyhow::bail!("the interface name {name:?} is longer than the kernel's {IFNAMSIZ} bytes");
    }
    let handle = netlink().await?;
    let existing = index_of(&handle, name).await?;
    if let Some(index) = existing {
        if has_address(&handle, index, local).await? {
            return Ok(Provenance::Existing);
        }
    }

    // Creating it is the same ioctl as attaching to it; what makes it outlive this process is
    // TUNSETPERSIST. The fd is dropped straight after: whoever uses the device attaches its own.
    let created = existing.is_none();
    if created {
        let fd = open_tun(name)?;
        make_persistent(&fd, owner)?;
    }
    let index = index_of(&handle, name)
        .await?
        .ok_or_else(|| anyhow!("{name} does not exist even after creating it"))?;

    let mut add = handle.address().add(index, std::net::IpAddr::V4(local), 32);
    if let Some(peer) = peer {
        use rtnetlink::packet_route::address::AddressAttribute;
        // Point-to-point: IFA_LOCAL is this end, IFA_ADDRESS the peer — `ip addr add <local> peer
        // <peer>`, which also installs the /32 route to the peer.
        for attr in add.message_mut().attributes.iter_mut() {
            if let AddressAttribute::Address(a) = attr {
                *a = std::net::IpAddr::V4(peer);
            }
        }
    }
    add.execute()
        .await
        .map_err(|e| anyhow!(e))
        .with_context(|| match peer {
            Some(p) => format!("adding {local} peer {p} to {name}"),
            None => format!("adding {local}/32 to {name}"),
        })?;

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
        .map_err(|e| anyhow!(e))
        .with_context(|| format!("bringing {name} up with mtu {mtu}"))?;

    Ok(if created {
        Provenance::Created
    } else {
        Provenance::Existing
    })
}

/// Remove `name` entirely (`setup --undo net`). False when it was not there.
pub async fn delete_link(name: &str) -> Result<bool> {
    let handle = netlink().await?;
    let Some(index) = index_of(&handle, name).await? else {
        return Ok(false);
    };
    handle
        .link()
        .del(index)
        .execute()
        .await
        .map_err(|e| anyhow!(e))
        .with_context(|| format!("deleting {name}"))?;
    Ok(true)
}
