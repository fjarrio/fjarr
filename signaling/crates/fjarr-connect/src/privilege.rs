//! `CAP_NET_ADMIN` as a **permitted, not effective** file capability (docs/27#the-operator-client).
//!
//! The installer grants `cap_net_admin+p`, and this raises the capability into the effective set only
//! for the commands that bring a link up. With `+ep` instead, the kernel refuses to execute the
//! binary at all wherever the capability is outside the bounding set (a container, CI, a sandbox),
//! so even `fjarr-connect shell` or `--help`, which need no privilege, failed with exit 126. That was
//! found installing the M2.5 packages into a clean container.
//!
//! Capabilities are per thread, so this runs in `main` **before** the async runtime starts its
//! workers, which inherit it.

/// Raise `CAP_NET_ADMIN` into the effective set when it is permitted. True when it is effective
/// afterwards (root, or the file capability). False otherwise: the link commands then say what to
/// grant (tun.rs) rather than failing with a bare `EPERM`.
#[cfg(target_os = "linux")]
pub fn raise_net_admin() -> bool {
    const LINUX_CAPABILITY_VERSION_3: u32 = 0x2008_0522;
    const CAP_NET_ADMIN: u32 = 12;
    #[repr(C)]
    struct Header {
        version: u32,
        pid: i32,
    }
    #[repr(C)]
    #[derive(Clone, Copy, Default)]
    struct Data {
        effective: u32,
        permitted: u32,
        inheritable: u32,
    }
    let mut header = Header {
        version: LINUX_CAPABILITY_VERSION_3,
        pid: 0,
    };
    let mut data = [Data::default(); 2];
    let bit = 1u32 << CAP_NET_ADMIN; // capabilities 0..31 live in data[0]
                                     // SAFETY: capget and capset with a version-3 header read and write exactly two `Data` structs,
                                     // which `data` is; `pid: 0` means this thread.
    unsafe {
        if libc::syscall(
            libc::SYS_capget,
            &mut header as *mut Header,
            data.as_mut_ptr(),
        ) != 0
        {
            return false;
        }
        if data[0].permitted & bit == 0 {
            return false;
        }
        if data[0].effective & bit != 0 {
            return true;
        }
        data[0].effective |= bit;
        libc::syscall(libc::SYS_capset, &mut header as *mut Header, data.as_ptr()) == 0
    }
}

/// macOS configures its interface through `sudo` (docs/27); there is no file capability to raise.
#[cfg(not(target_os = "linux"))]
pub fn raise_net_admin() -> bool {
    false
}
