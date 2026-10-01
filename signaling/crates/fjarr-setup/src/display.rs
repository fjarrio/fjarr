//! `fjarr-agent display list|add-ghost|remove-ghost`: the robot's connectors, and ghost screens for a
//! headless robot (docs/26#ghost-screens, ADR-0032 and its 2026-10-01 addendum).
//!
//! A ghost is a forced DRM connector carrying a Fjarr-generated EDID: `video=<connector>:<mode>e`
//! and `drm.edid_firmware=<connector>:edid/fjarr-ghost-N.bin` on the kernel command line. The GRUB
//! snippet is the one record of which ghosts exist; every write is recorded for
//! `setup --undo desktop`. Ghosts go on free root connectors only: never one a monitor uses, and
//! never a DisplayPort chain's root, which reads as disconnected while its chain is plugged in.
use std::collections::BTreeMap;
use std::path::Path;

use anyhow::{bail, Context, Result};

use crate::changes::Record;
use crate::desktop::FEATURE;
use crate::system;

pub const GRUB_SNIPPET: &str = "/etc/default/grub.d/fjarr-ghosts.cfg";
pub const FIRMWARE_DIR: &str = "/usr/lib/firmware/edid";
pub const DEFAULT_MODE: Mode = Mode {
    width: 1920,
    height: 1080,
    refresh: 60,
};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Mode {
    pub width: u32,
    pub height: u32,
    pub refresh: u32,
}

impl Mode {
    /// `1920x1080@60` (the refresh defaults to 60).
    pub fn parse(s: &str) -> Result<Mode> {
        let (size, refresh) = s.split_once('@').unwrap_or((s, "60"));
        let (w, h) = size
            .split_once('x')
            .with_context(|| format!("mode {s:?}: WIDTHxHEIGHT[@HZ]"))?;
        let m = Mode {
            width: w.parse().with_context(|| format!("mode {s:?}: width"))?,
            height: h.parse().with_context(|| format!("mode {s:?}: height"))?,
            refresh: refresh
                .parse()
                .with_context(|| format!("mode {s:?}: refresh"))?,
        };
        if !(640..=4096).contains(&m.width)
            || !(480..=2304).contains(&m.height)
            || !(24..=120).contains(&m.refresh)
        {
            bail!("mode {s:?}: a ghost is 640x480 to 4096x2304, 24 to 120 Hz");
        }
        Ok(m)
    }
}

impl std::fmt::Display for Mode {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}x{}@{}", self.width, self.height, self.refresh)
    }
}

/// One configured ghost: the connector, its number (EDID name and file) and mode.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Ghost {
    pub connector: String,
    pub number: u32,
    pub mode: Mode,
}

impl Ghost {
    pub fn firmware_name(&self) -> String {
        format!("fjarr-ghost-{}.bin", self.number)
    }
}

// --------------------------------------------------------------------------------------- EDID

/// A detailed timing from CVT reduced blanking (VESA CVT 1.2, RB v1): what a monitor of this mode
/// would advertise.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Timing {
    pub pixel_khz: u32,
    pub h_active: u32,
    pub h_blank: u32,
    pub h_front: u32,
    pub h_sync: u32,
    pub v_active: u32,
    pub v_blank: u32,
    pub v_front: u32,
    pub v_sync: u32,
}

pub fn cvt_rb(m: Mode) -> Timing {
    const H_BLANK: u32 = 160;
    const H_FRONT: u32 = 48;
    const H_SYNC: u32 = 32;
    const V_FRONT: u32 = 3;
    const MIN_V_BACK: u32 = 6;
    const MIN_VBI_US: f64 = 460.0;
    // The vsync width says the aspect ratio (CVT 1.2 table 3).
    let v_sync = match (
        m.width * 3 == m.height * 4,
        m.width * 9 == m.height * 16,
        m.width * 10 == m.height * 16,
        m.width * 4 == m.height * 5,
        m.width * 9 == m.height * 15,
    ) {
        (true, ..) => 4,
        (_, true, ..) => 5,
        (_, _, true, ..) => 6,
        (.., true, _) | (.., true) => 7,
        _ => 10,
    };
    let h_period_us = (1_000_000.0 / m.refresh as f64 - MIN_VBI_US) / m.height as f64;
    let vbi = ((MIN_VBI_US / h_period_us).floor() as u32 + 1).max(V_FRONT + v_sync + MIN_V_BACK);
    let total_h = m.width + H_BLANK;
    let total_v = m.height + vbi;
    // The clock in 0.25 MHz steps, rounded down (CVT).
    let pixel_khz =
        ((m.refresh as f64 * total_h as f64 * total_v as f64 / 250_000.0).floor() as u32) * 250;
    Timing {
        pixel_khz,
        h_active: m.width,
        h_blank: H_BLANK,
        h_front: H_FRONT,
        h_sync: H_SYNC,
        v_active: m.height,
        v_blank: vbi,
        v_front: V_FRONT,
        v_sync,
    }
}

fn descriptor_text(tag: u8, text: &str) -> [u8; 18] {
    let mut d = [0u8; 18];
    d[3] = tag;
    let bytes = text.as_bytes();
    let n = bytes.len().min(13);
    d[5..5 + n].copy_from_slice(&bytes[..n]);
    if n < 13 {
        d[5 + n] = b'\n';
        for b in d.iter_mut().skip(6 + n) {
            *b = b' ';
        }
    }
    d
}

/// A ghost's EDID 1.4: manufacturer `FJR`, the name "Fjarr Ghost N" and serial `FJGHOST<N>`, so its
/// monitor id (`fjr-fjarr-ghost-n-fjghostn`) can never be a real monitor's, and the mode as its
/// one preferred timing.
pub fn edid(number: u32, mode: Mode) -> [u8; 128] {
    let mut e = [0u8; 128];
    e[0..8].copy_from_slice(&[0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00]);
    let letter = |c: u8| (c - b'A' + 1) as u16;
    let mfg = (letter(b'F') << 10) | (letter(b'J') << 5) | letter(b'R');
    e[8..10].copy_from_slice(&mfg.to_be_bytes());
    e[10..12].copy_from_slice(&(0x4700u16 + number as u16).to_le_bytes()); // product code
    e[12..16].copy_from_slice(&number.to_le_bytes()); // serial number
    e[16] = 0; // week: unspecified
    e[17] = (2026 - 1990) as u8; // year of manufacture
    e[18] = 1;
    e[19] = 4; // EDID 1.4
    e[20] = 0x80; // digital input, depth and interface undefined: valid on DP and HDMI alike
                  // Physical size at 96 dpi, in cm.
    e[21] = ((mode.width as f64 * 2.54 / 96.0).round() as u32).min(255) as u8;
    e[22] = ((mode.height as f64 * 2.54 / 96.0).round() as u32).min(255) as u8;
    e[23] = 120; // gamma 2.2
    e[24] = 0x06; // RGB 4:4:4, sRGB default, preferred timing is the native mode
    e[25..35].copy_from_slice(&[0xEE, 0x91, 0xA3, 0x54, 0x4C, 0x99, 0x26, 0x0F, 0x50, 0x54]); // sRGB primaries
                                                                                              // No established timings; standard timings unused (0x0101).
    for i in 0..8 {
        e[38 + 2 * i] = 0x01;
        e[39 + 2 * i] = 0x01;
    }
    // Descriptor 1: the preferred detailed timing.
    let t = cvt_rb(mode);
    let d = &mut e[54..72];
    d[0..2].copy_from_slice(&((t.pixel_khz / 10) as u16).to_le_bytes());
    d[2] = (t.h_active & 0xFF) as u8;
    d[3] = (t.h_blank & 0xFF) as u8;
    d[4] = (((t.h_active >> 8) & 0xF) << 4 | ((t.h_blank >> 8) & 0xF)) as u8;
    d[5] = (t.v_active & 0xFF) as u8;
    d[6] = (t.v_blank & 0xFF) as u8;
    d[7] = (((t.v_active >> 8) & 0xF) << 4 | ((t.v_blank >> 8) & 0xF)) as u8;
    d[8] = (t.h_front & 0xFF) as u8;
    d[9] = (t.h_sync & 0xFF) as u8;
    d[10] = (((t.v_front & 0xF) << 4) | (t.v_sync & 0xF)) as u8;
    d[11] = ((((t.h_front >> 8) & 0x3) << 6)
        | (((t.h_sync >> 8) & 0x3) << 4)
        | (((t.v_front >> 4) & 0x3) << 2)
        | ((t.v_sync >> 4) & 0x3)) as u8;
    let (w_mm, h_mm) = (e[21] as u32 * 10, e[22] as u32 * 10);
    let d = &mut e[54..72];
    d[12] = (w_mm & 0xFF) as u8;
    d[13] = (h_mm & 0xFF) as u8;
    d[14] = ((((w_mm >> 8) & 0xF) << 4) | ((h_mm >> 8) & 0xF)) as u8;
    d[17] = 0x1A; // digital separate sync, hsync +, vsync - (CVT reduced blanking)
                  // Descriptor 2: range limits around the mode; 3: the name; 4: the serial string.
    let mut range = [0u8; 18];
    range[3] = 0xFD;
    range[5] = (mode.refresh.saturating_sub(1)).max(1) as u8; // min V Hz
    range[6] = (mode.refresh + 1) as u8; // max V Hz
    let h_khz = t.pixel_khz / (t.h_active + t.h_blank);
    range[7] = (h_khz.saturating_sub(1)).max(1) as u8; // min H kHz
    range[8] = (h_khz + 1).min(255) as u8; // max H kHz
    range[9] = ((t.pixel_khz / 10_000) + 1).min(255) as u8; // max clock, 10 MHz units
    range[10] = 0x01; // range limits only
    range[11] = b'\n';
    for b in range.iter_mut().skip(12) {
        *b = b' ';
    }
    e[72..90].copy_from_slice(&range);
    e[90..108].copy_from_slice(&descriptor_text(0xFC, &format!("Fjarr Ghost {number}")));
    e[108..126].copy_from_slice(&descriptor_text(0xFF, &format!("FJGHOST{number}")));
    e[126] = 0; // no extensions
    e[127] = (0u8).wrapping_sub(e[..127].iter().fold(0u8, |a, b| a.wrapping_add(*b)));
    e
}

// -------------------------------------------------------------------------------- the kernel line

/// The GRUB snippet for these ghosts: the two kernel parameters, appended to the distribution's.
pub fn grub_snippet(ghosts: &[Ghost]) -> String {
    let video: Vec<String> = ghosts
        .iter()
        .map(|g| format!("video={}:{}e", g.connector, g.mode))
        .collect();
    let firmware: Vec<String> = ghosts
        .iter()
        .map(|g| format!("{}:edid/{}", g.connector, g.firmware_name()))
        .collect();
    format!(
        "# Written by `fjarr-agent display`: ghost screens, forced connectors with Fjarr EDIDs\n\
         # (docs/26#ghost-screens). Change with `fjarr-agent display add-ghost|remove-ghost`;\n\
         # undo with `fjarr-agent setup --undo desktop`.\n\
         # fjarr-ghosts: {}\n\
         GRUB_CMDLINE_LINUX_DEFAULT=\"$GRUB_CMDLINE_LINUX_DEFAULT {} drm.edid_firmware={}\"\n",
        ghosts
            .iter()
            .map(|g| format!("{}={}:{}", g.connector, g.number, g.mode))
            .collect::<Vec<_>>()
            .join(" "),
        video.join(" "),
        firmware.join(",")
    )
}

/// The ghosts a snippet configures: its `# fjarr-ghosts:` line.
pub fn parse_snippet(text: &str) -> Vec<Ghost> {
    let Some(line) = text.lines().find_map(|l| l.strip_prefix("# fjarr-ghosts:")) else {
        return Vec::new();
    };
    line.split_whitespace()
        .filter_map(|item| {
            let (connector, rest) = item.split_once('=')?;
            let (number, mode) = rest.split_once(':')?;
            Some(Ghost {
                connector: connector.into(),
                number: number.parse().ok()?,
                mode: Mode::parse(mode).ok()?,
            })
        })
        .collect()
}

/// The ghosts the running kernel was booted with: `video=<c>:…e` on /proc/cmdline.
pub fn booted_ghosts(cmdline: &str) -> Vec<String> {
    cmdline
        .split_whitespace()
        .filter_map(|p| p.strip_prefix("video="))
        .filter(|v| v.ends_with('e'))
        .filter_map(|v| v.split_once(':').map(|(c, _)| c.to_string()))
        .collect()
}

// ------------------------------------------------------------------------------------ connectors

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Connector {
    /// The kernel's name: `DP-2`, `HDMI-A-1`.
    pub name: String,
    pub connected: bool,
    /// For a connector on a DisplayPort chain: the root connector the chain hangs off.
    pub mst_root: Option<String>,
    /// The monitor's name from its EDID, when connected. On a ghost connector that is the ghost.
    pub monitor: Option<String>,
    /// The name a sink gives over the connector's DDC bus, read only where `scan` was asked to:
    /// on a ghost connector this is a real monitor, whatever EDID the kernel forces (#37).
    pub ddc_monitor: Option<String>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum State {
    /// A real monitor.
    Connected(String),
    /// The root of a DisplayPort chain, with the chain's monitors.
    Chain(Vec<String>),
    Free,
    /// Configured as a ghost; `active` when this boot has it.
    Ghost {
        ghost: Ghost,
        active: bool,
        real_monitor: Option<String>,
    },
}

/// What `display list` shows, one row per root connector (chain members under their root).
pub fn rows(connectors: &[Connector], ghosts: &[Ghost], booted: &[String]) -> Vec<(String, State)> {
    let mut chains: BTreeMap<String, Vec<String>> = BTreeMap::new();
    for c in connectors {
        if let Some(root) = &c.mst_root {
            if c.connected {
                chains
                    .entry(root.clone())
                    .or_default()
                    .push(c.monitor.clone().unwrap_or_else(|| c.name.clone()));
            }
        }
    }
    let mut out = Vec::new();
    for c in connectors.iter().filter(|c| c.mst_root.is_none()) {
        let state = if let Some(g) = ghosts.iter().find(|g| g.connector == c.name) {
            // A real monitor plugged into a ghost connector shows up as the ghost, because the kernel
            // forces the ghost's EDID on the port; its own EDID still answers over DDC (#37, measured
            // on the mini-PC). A ghost never answers there: it has no sink.
            let ghost_name = format!("Fjarr Ghost {}", g.number);
            let real = c.ddc_monitor.clone().filter(|m| *m != ghost_name);
            State::Ghost {
                ghost: g.clone(),
                active: booted.contains(&c.name),
                real_monitor: real,
            }
        } else if let Some(members) = chains.get(&c.name) {
            State::Chain(members.clone())
        } else if c.connected {
            State::Connected(
                c.monitor
                    .clone()
                    .unwrap_or_else(|| "unknown monitor".into()),
            )
        } else {
            State::Free
        };
        out.push((c.name.clone(), state));
    }
    out
}

/// The next free root connector for a ghost, or why there is none.
pub fn pick_free(rows: &[(String, State)], wanted: Option<&str>) -> Result<String> {
    if let Some(w) = wanted {
        return match rows.iter().find(|(n, _)| n == w) {
            None => bail!("no connector {w} on this machine (fjarr-agent display list shows them)"),
            Some((_, State::Free)) => Ok(w.to_string()),
            Some((_, State::Ghost { .. })) => bail!("{w} is already a ghost"),
            Some((_, State::Chain(_))) => bail!(
                "{w} has a DisplayPort chain plugged in: a ghost there would replace the chain"
            ),
            Some((_, _)) => {
                bail!("{w} has a monitor plugged in: a ghost there would replace its EDID")
            }
        };
    }
    // eDP is a laptop's own panel: never a ghost.
    match rows
        .iter()
        .find(|(n, s)| *s == State::Free && !n.starts_with("eDP"))
    {
        Some((n, _)) => Ok(n.clone()),
        None => {
            let ghosts = rows
                .iter()
                .filter(|(_, s)| matches!(s, State::Ghost { .. }))
                .count();
            bail!(
                "no free connector left: this machine can have {ghosts} ghost screen{} (one per free root connector; \
                 a chain's root and connectors with monitors are never used)",
                if ghosts == 1 { "" } else { "s" }
            )
        }
    }
}

/// The root connector a DisplayPort MST connector's `PATH` (`mst:<base>-<port>[-<port>…]`) hangs off.
/// Drivers fill `<base>` differently: Intel with the root connector's DRM object id, amdgpu with its
/// own connector index, which is the root's position among the root connectors (measured on the
/// mini-PC, 2026-10-01: `mst:1-8` for a chain on DP-1, after HDMI-A-1). `roots` is (object id, name)
/// of every connector without a PATH, in the driver's order.
pub fn resolve_mst_root(path: &str, roots: &[(u32, String)]) -> Option<String> {
    let base: u32 = path.strip_prefix("mst:")?.split('-').next()?.parse().ok()?;
    if let Some((_, name)) = roots.iter().find(|(id, _)| *id == base) {
        return Some(name.clone());
    }
    roots.get(base as usize).map(|(_, name)| name.clone())
}

/// The connectors, from the DRM devices (cardN): the kernel's names, connection state, the chain a
/// DisplayPort MST connector hangs off (its `PATH` property, `mst:<root id>-<port>`), and the
/// monitor's EDID name.
pub fn scan(probe_ddc: &[String]) -> Result<Vec<Connector>> {
    use drm::control::{connector, Device as ControlDevice};
    struct Card(std::fs::File);
    impl std::os::fd::AsFd for Card {
        fn as_fd(&self) -> std::os::fd::BorrowedFd<'_> {
            self.0.as_fd()
        }
    }
    impl drm::Device for Card {}
    impl ControlDevice for Card {}

    let mut out = Vec::new();
    let mut cards: Vec<_> = std::fs::read_dir("/dev/dri")
        .context("reading /dev/dri")?
        .filter_map(|e| e.ok())
        .map(|e| e.path())
        .filter(|p| {
            p.file_name()
                .is_some_and(|n| n.to_string_lossy().starts_with("card"))
        })
        .collect();
    cards.sort();
    for path in cards {
        let Ok(file) = std::fs::OpenOptions::new()
            .read(true)
            .write(true)
            .open(&path)
        else {
            continue;
        };
        let card = Card(file);
        let Ok(res) = card.resource_handles() else {
            continue;
        };
        let mut infos = Vec::new();
        for &h in res.connectors() {
            let Ok(info) = card.get_connector(h, false) else {
                continue;
            };
            let name = format!("{}-{}", info.interface().as_str(), info.interface_id());
            let mut path_prop = None;
            let mut edid_name = None;
            if let Ok(props) = card.get_properties(h) {
                let (ids, values) = props.as_props_and_values();
                for (id, value) in ids.iter().zip(values.iter()) {
                    let Ok(p) = card.get_property(*id) else {
                        continue;
                    };
                    let pname = p.name().to_string_lossy().into_owned();
                    if (pname == "PATH" || pname == "EDID") && *value != 0 {
                        if let Ok(blob) = card.get_property_blob(*value) {
                            if pname == "PATH" {
                                path_prop = Some(
                                    String::from_utf8_lossy(&blob)
                                        .trim_end_matches('\0')
                                        .to_string(),
                                );
                            } else {
                                edid_name = edid_monitor_name(&blob);
                            }
                        }
                    }
                }
            }
            infos.push((
                name,
                info.state() == connector::State::Connected,
                path_prop,
                edid_name,
                u32::from(h),
            ));
        }
        // The root connectors (no PATH), in the order the driver lists them: amdgpu names a chain's
        // root by this position, Intel by its object id (resolve_mst_root).
        let roots: Vec<(u32, String)> = infos
            .iter()
            .filter(|(_, _, p, _, _)| p.is_none())
            .map(|(n, _, _, _, id)| (*id, n.clone()))
            .collect();
        for (name, connected, path_prop, monitor, _) in infos {
            if name.starts_with("Writeback") || name.starts_with("Virtual") {
                continue;
            }
            let mst_root = path_prop
                .as_deref()
                .and_then(|p| resolve_mst_root(p, &roots));
            let ddc_monitor = if probe_ddc.contains(&name) {
                let card_name = path
                    .file_name()
                    .map(|n| n.to_string_lossy().into_owned())
                    .unwrap_or_default();
                ddc_edid(&card_name, &name).and_then(|e| edid_monitor_name(&e))
            } else {
                None
            };
            out.push(Connector {
                name,
                connected,
                mst_root,
                monitor,
                ddc_monitor,
            });
        }
    }
    Ok(out)
}

/// A sink's own EDID over the connector's DDC bus (`/sys/class/drm/<card>-<connector>/ddc`, read
/// through `i2c-dev` at the EDID address 0x50), bypassing any EDID the kernel forces. None when no
/// sink answers, the connector has no DDC bus, or `i2c-dev` is not loaded.
pub fn ddc_edid(card: &str, connector: &str) -> Option<Vec<u8>> {
    use std::io::{Read, Write};
    use std::os::fd::AsRawFd;
    const I2C_SLAVE: libc::c_ulong = 0x0703;
    let bus = std::fs::read_link(format!("/sys/class/drm/{card}-{connector}/ddc")).ok()?;
    let dev = Path::new("/dev").join(bus.file_name()?);
    let mut f = std::fs::OpenOptions::new()
        .read(true)
        .write(true)
        .open(dev)
        .ok()?;
    // SAFETY: I2C_SLAVE takes the 7-bit address by value; the descriptor is open.
    if unsafe { libc::ioctl(f.as_raw_fd(), I2C_SLAVE as _, 0x50 as libc::c_ulong) } < 0 {
        return None;
    }
    f.write_all(&[0]).ok()?;
    let mut edid = vec![0u8; 128];
    f.read_exact(&mut edid).ok()?;
    (edid[0..8] == [0, 255, 255, 255, 255, 255, 255, 0]).then_some(edid)
}

/// What to call the monitor an EDID describes: its name descriptor (0xFC); else the last free-text
/// descriptor (0xFE), which is where laptop panels put their model ("N140JCA-EEL"); else the
/// manufacturer code.
pub fn edid_monitor_name(edid: &[u8]) -> Option<String> {
    if edid.len() < 128 || edid[0..8] != [0, 255, 255, 255, 255, 255, 255, 0] {
        return None;
    }
    let text = |tag: u8| -> Vec<String> {
        (0..4)
            .map(|i| &edid[54 + 18 * i..72 + 18 * i])
            .filter(|d| d[0] == 0 && d[1] == 0 && d[3] == tag)
            .map(|d| {
                String::from_utf8_lossy(&d[5..18])
                    .split('\n')
                    .next()
                    .unwrap_or("")
                    .trim()
                    .to_string()
            })
            .filter(|t| !t.is_empty())
            .collect()
    };
    if let Some(name) = text(0xFC).into_iter().next() {
        return Some(name);
    }
    if let Some(model) = text(0xFE).into_iter().last() {
        return Some(model);
    }
    let m = u16::from_be_bytes([edid[8], edid[9]]);
    let l = |s: u16| (b'A' + ((m >> s) & 0x1F) as u8 - 1) as char;
    Some(format!("{}{}{}", l(10), l(5), l(0)))
}

// ------------------------------------------------------------------------------------- commands

fn load_ghosts() -> Vec<Ghost> {
    std::fs::read_to_string(GRUB_SNIPPET)
        .map(|t| parse_snippet(&t))
        .unwrap_or_default()
}

fn booted() -> Vec<String> {
    booted_ghosts(&std::fs::read_to_string("/proc/cmdline").unwrap_or_default())
}

pub fn list() -> Result<i32> {
    let ghosts = load_ghosts();
    let probe: Vec<String> = ghosts.iter().map(|g| g.connector.clone()).collect();
    let connectors = scan(&probe)?;
    println!("  {:<11}{:<13}{:<27}GHOST", "CONNECTOR", "STATE", "MONITOR");
    for (name, state) in rows(&connectors, &ghosts, &booted()) {
        let (st, monitor, ghost): (String, String, String) = match state {
            State::Connected(m) => ("connected".into(), format!("{m} (real)"), "—".to_string()),
            State::Chain(ms) => (
                "connected".into(),
                format!(
                    "MST chain: {} monitor{}",
                    ms.len(),
                    if ms.len() == 1 { "" } else { "s" }
                ),
                "— (not forceable: chain)".into(),
            ),
            State::Free => ("free".into(), "—".into(), "can be a ghost".into()),
            State::Ghost {
                ghost,
                active,
                real_monitor,
            } => (
                "ghost".into(),
                format!(
                    "Fjarr Ghost {}  {}×{}",
                    ghost.number, ghost.mode.width, ghost.mode.height
                ),
                match (active, real_monitor) {
                    (_, Some(m)) => {
                        format!("FAILED: {m} is plugged in here and shows as the ghost")
                    }
                    (true, None) => "active".into(),
                    (false, None) => "reboot pending".into(),
                },
            ),
        };
        println!("  {name:<11}{st:<13}{monitor:<27}{ghost}");
    }
    Ok(0)
}

/// Write the snippet and EDIDs for `ghosts`, recorded under `desktop`, then regenerate GRUB's file.
fn apply(record: &mut Record, state: &Path, ghosts: &[Ghost]) -> Result<()> {
    let backups = Path::new(crate::changes::BACKUPS);
    for g in ghosts {
        let path = Path::new(FIRMWARE_DIR).join(g.firmware_name());
        if !path.exists() {
            std::fs::create_dir_all(FIRMWARE_DIR)?;
            std::fs::write(&path, edid(g.number, g.mode))
                .with_context(|| format!("writing {}", path.display()))?;
            record.add(FEATURE, crate::changes::Change::File { path, backup: None });
        }
    }
    if ghosts.is_empty() {
        if Path::new(GRUB_SNIPPET).exists() {
            record.write_file(
                FEATURE,
                Path::new(GRUB_SNIPPET),
                "# fjarr-ghosts:\n",
                backups,
            )?;
        }
    } else {
        record.write_file(
            FEATURE,
            Path::new(GRUB_SNIPPET),
            &grub_snippet(ghosts),
            backups,
        )?;
    }
    record.save(state)?;
    if Path::new("/usr/sbin/update-grub").exists() {
        system::run("update-grub", &[])?;
    }
    Ok(())
}

/// Add `count` ghosts in `mode`, each on the next free root connector (or `connector`, for one),
/// recorded under `desktop`: what `display add-ghost` and `setup desktop --ghost-screens N` share.
pub fn add_ghosts(
    record: &mut Record,
    state: &Path,
    count: u32,
    connector: Option<&str>,
    mode: Mode,
) -> Result<Vec<Ghost>> {
    if !system::ubuntu_with_apt() {
        bail!("ghost screens are set on the kernel command line with GRUB; this is not Ubuntu with apt, so nothing is changed (docs/26#ghost-screens)");
    }
    let connectors = scan(&[])?;
    let mut ghosts = load_ghosts();
    let mut added = Vec::new();
    for _ in 0..count {
        let chosen = pick_free(&rows(&connectors, &ghosts, &booted()), connector)?;
        let number = (1..)
            .find(|n| !ghosts.iter().any(|g| g.number == *n))
            .expect("a free number");
        let g = Ghost {
            connector: chosen,
            number,
            mode,
        };
        ghosts.push(g.clone());
        added.push(g);
    }
    apply(record, state, &ghosts)?;
    Ok(added)
}

pub fn add_ghost(state: &Path, connector: Option<&str>, mode: Option<&str>) -> Result<i32> {
    crate::require_root("display add-ghost")?;
    let mode = mode.map(Mode::parse).transpose()?.unwrap_or(DEFAULT_MODE);
    let mut record = Record::load(state)?;
    for g in add_ghosts(&mut record, state, 1, connector, mode)? {
        println!(
            "  {} → Fjarr Ghost {} ({}) · takes effect after a reboot",
            g.connector, g.number, g.mode
        );
    }
    Ok(0)
}

/// Monitors plugged in now (root connectors and chains): whether `setup desktop` offers ghosts.
pub fn monitors_connected() -> usize {
    scan(&[])
        .map(|cs| {
            cs.iter()
                .filter(|c| c.connected && !c.name.starts_with("eDP"))
                .count()
        })
        .unwrap_or(0)
}

pub fn remove_ghost(state: &Path, which: &str) -> Result<i32> {
    crate::require_root("display remove-ghost")?;
    let ghosts = load_ghosts();
    let keep: Vec<Ghost> = if which == "all" {
        Vec::new()
    } else {
        ghosts
            .iter()
            .filter(|g| g.connector != which)
            .cloned()
            .collect()
    };
    if keep.len() == ghosts.len() {
        bail!("{which} is not a ghost (fjarr-agent display list shows them)");
    }
    let mut record = Record::load(state)?;
    apply(&mut record, state, &keep)?;
    for g in ghosts.iter().filter(|g| !keep.contains(g)) {
        println!(
            "  {} → no longer a ghost · takes effect after a reboot",
            g.connector
        );
    }
    Ok(0)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_chains_root_is_found_by_object_id_or_by_amdgpus_connector_index() {
        // The mini-PC's amdgpu, 2026-10-01: HDMI-A-1 (102), DP-1 (111) …; chained monitors say mst:1-….
        let roots = vec![
            (102, "HDMI-A-1".to_string()),
            (111, "DP-1".to_string()),
            (119, "DP-2".to_string()),
            (125, "DP-3".to_string()),
        ];
        assert_eq!(resolve_mst_root("mst:1-8", &roots).as_deref(), Some("DP-1"));
        assert_eq!(resolve_mst_root("mst:1-1", &roots).as_deref(), Some("DP-1"));
        // Intel: the root's object id.
        assert_eq!(
            resolve_mst_root("mst:119-1", &roots).as_deref(),
            Some("DP-2")
        );
        assert_eq!(resolve_mst_root("mst:99-1", &roots), None);
        assert_eq!(resolve_mst_root("not-mst", &roots), None);
    }

    fn conn(name: &str, connected: bool, monitor: Option<&str>, root: Option<&str>) -> Connector {
        Connector {
            name: name.into(),
            connected,
            monitor: monitor.map(Into::into),
            mst_root: root.map(Into::into),
            ddc_monitor: None,
        }
    }

    #[test]
    fn a_ghosts_edid_is_valid_and_names_itself() {
        let e = edid(2, DEFAULT_MODE);
        // For checking with an independent decoder (`di-edid-decode`, `edid-decode`).
        if let Ok(path) = std::env::var("FJARR_WRITE_GHOST_EDID") {
            std::fs::write(path, e).unwrap();
        }
        assert_eq!(&e[0..8], &[0, 255, 255, 255, 255, 255, 255, 0]);
        assert_eq!(e.iter().fold(0u8, |a, b| a.wrapping_add(*b)), 0, "checksum");
        assert_eq!(edid_monitor_name(&e).as_deref(), Some("Fjarr Ghost 2"));
        // Manufacturer FJR.
        let m = u16::from_be_bytes([e[8], e[9]]);
        let l = |s: u16| (b'A' + ((m >> s) & 0x1F) as u8 - 1) as char;
        assert_eq!(format!("{}{}{}", l(10), l(5), l(0)), "FJR");
        // The serial string, which makes the monitor id distinct from any real monitor's.
        assert!(e[108..126].windows(8).any(|w| w == b"FJGHOST2"));
        // The preferred timing: 1920x1080.
        let d = &e[54..72];
        assert_eq!(d[2] as u32 | ((d[4] as u32 >> 4) << 8), 1920);
        assert_eq!(d[5] as u32 | ((d[7] as u32 >> 4) << 8), 1080);
    }

    #[test]
    fn cvt_reduced_blanking_matches_the_vesa_numbers() {
        // VESA DMT 1080p60 CVT-RB: 138.5 MHz, 2080x1111 total.
        let t = cvt_rb(DEFAULT_MODE);
        assert_eq!(t.pixel_khz, 138_500);
        assert_eq!(t.h_active + t.h_blank, 2080);
        assert_eq!(t.v_active + t.v_blank, 1111);
        assert_eq!(t.v_sync, 5, "16:9");
        // 1280x720@60 CVT-RB: 64.0 MHz (CVT 1.2 calculator).
        assert_eq!(
            cvt_rb(Mode {
                width: 1280,
                height: 720,
                refresh: 60
            })
            .pixel_khz,
            64_000
        );
    }

    #[test]
    fn the_snippet_round_trips_and_carries_both_kernel_parameters() {
        let ghosts = vec![
            Ghost {
                connector: "DP-2".into(),
                number: 1,
                mode: DEFAULT_MODE,
            },
            Ghost {
                connector: "HDMI-A-2".into(),
                number: 2,
                mode: Mode::parse("1280x720").unwrap(),
            },
        ];
        let s = grub_snippet(&ghosts);
        assert!(s.contains("video=DP-2:1920x1080@60e video=HDMI-A-2:1280x720@60e"));
        assert!(s.contains(
            "drm.edid_firmware=DP-2:edid/fjarr-ghost-1.bin,HDMI-A-2:edid/fjarr-ghost-2.bin"
        ));
        assert!(
            s.contains("\"$GRUB_CMDLINE_LINUX_DEFAULT "),
            "appends to the distribution's parameters"
        );
        assert_eq!(parse_snippet(&s), ghosts);
        assert_eq!(parse_snippet("# fjarr-ghosts:\n"), vec![]);
        assert_eq!(
            booted_ghosts("BOOT_IMAGE=/vmlinuz quiet video=DP-2:1920x1080@60e video=efifb:off"),
            vec!["DP-2"]
        );
    }

    #[test]
    fn a_ghost_goes_only_on_a_free_root_connector() {
        // HDMI-A-1 has a monitor; DP-1 is a chain's root (it reads disconnected); DP-2 is free.
        let cs = vec![
            conn("eDP-1", false, None, None),
            conn("HDMI-A-1", true, Some("DELL U2422H"), None),
            conn("DP-1", false, None, None),
            conn("DP-5", true, Some("DELL P2422H"), Some("DP-1")),
            conn("DP-6", true, Some("DELL P2422H"), Some("DP-1")),
            conn("DP-2", false, None, None),
        ];
        let r = rows(&cs, &[], &[]);
        assert_eq!(
            r.iter().find(|(n, _)| n == "DP-1").unwrap().1,
            State::Chain(vec!["DELL P2422H".into(), "DELL P2422H".into()])
        );
        assert!(
            !r.iter().any(|(n, _)| n == "DP-5"),
            "chain members are listed under their root"
        );
        assert_eq!(
            pick_free(&r, None).unwrap(),
            "DP-2",
            "not the laptop panel, not the chain's root"
        );
        assert!(pick_free(&r, Some("HDMI-A-1"))
            .unwrap_err()
            .to_string()
            .contains("monitor plugged in"));
        assert!(pick_free(&r, Some("DP-1"))
            .unwrap_err()
            .to_string()
            .contains("chain"));
        // Every free connector used: say how many this machine can have.
        let g = vec![Ghost {
            connector: "DP-2".into(),
            number: 1,
            mode: DEFAULT_MODE,
        }];
        let r = rows(&cs, &g, &["DP-2".into()]);
        assert!(pick_free(&r, None)
            .unwrap_err()
            .to_string()
            .contains("can have 1 ghost screen"));
    }

    #[test]
    fn a_ghost_is_active_once_booted_and_a_real_monitor_on_it_is_a_failure() {
        let g = vec![Ghost {
            connector: "DP-2".into(),
            number: 1,
            mode: DEFAULT_MODE,
        }];
        let pending = rows(&[conn("DP-2", false, None, None)], &g, &[]);
        assert!(matches!(
            &pending[0].1,
            State::Ghost {
                active: false,
                real_monitor: None,
                ..
            }
        ));
        let active = rows(
            &[conn("DP-2", true, Some("Fjarr Ghost 1"), None)],
            &g,
            &["DP-2".into()],
        );
        assert!(matches!(
            &active[0].1,
            State::Ghost {
                active: true,
                real_monitor: None,
                ..
            }
        ));
        // The mini-PC, 2026-10-01: a DELL on the ghost's HDMI-A-1 reads as the ghost through the
        // kernel and as itself over DDC.
        let mut on_ghost = conn("DP-2", true, Some("Fjarr Ghost 1"), None);
        on_ghost.ddc_monitor = Some("DELL U2422H".into());
        let clash = rows(&[on_ghost], &g, &["DP-2".into()]);
        assert!(
            matches!(&clash[0].1, State::Ghost { real_monitor: Some(m), .. } if m == "DELL U2422H")
        );
    }
}
