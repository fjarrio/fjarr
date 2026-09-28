//! What `setup` and `drivers detect` find out about this device (docs/26#fjarr-agent-setup): the
//! OS and architecture, hardware H.264 encode (fjarr-agent's own answer), the display server, and
//! the cameras gst-device-monitor-1.0 sees, matched against the catalog.
use std::collections::BTreeMap;
use std::path::Path;
use std::process::Command;

use anyhow::{Context, Result};
use serde::Serialize;

use crate::catalog::{Catalog, Entry};

/// The Debian architecture name, which is what the catalog's `arch` lists.
pub fn deb_arch() -> &'static str {
    match std::env::consts::ARCH {
        "x86_64" => "amd64",
        "aarch64" => "arm64",
        "x86" => "i386",
        "arm" => "armhf",
        "riscv64" => "riscv64",
        other => other,
    }
}

/// `PRETTY_NAME` from os-release: "Ubuntu 26.04 LTS".
pub fn os_name() -> String {
    std::fs::read_to_string("/etc/os-release")
        .ok()
        .and_then(|t| {
            t.lines().find_map(|l| {
                l.strip_prefix("PRETTY_NAME=")
                    .map(|v| v.trim_matches('"').to_string())
            })
        })
        .unwrap_or_else(|| "unknown OS".to_string())
}

pub fn hostname() -> String {
    let mut buf = [0u8; 256];
    // SAFETY: a valid buffer of its stated length; the result is NUL-terminated on success.
    if unsafe { libc::gethostname(buf.as_mut_ptr() as *mut libc::c_char, buf.len()) } == 0 {
        let end = buf.iter().position(|&b| b == 0).unwrap_or(buf.len());
        if let Ok(s) = std::str::from_utf8(&buf[..end]) {
            if !s.is_empty() {
                return s.to_string();
            }
        }
    }
    "device".to_string()
}

/// fjarr-agent's own answer, from the line `--check` prints first, with a throwaway config: the
/// encoder the agent will choose is the one it probes, not one this tool guesses at.
pub fn hardware_encode() -> Option<bool> {
    let out = Command::new(crate::agent_binary())
        .arg("--check")
        .env("FJARR_ROBOT_ID", "setup-probe")
        .env("FJARR_SERVER_URL", "ws://localhost/ws")
        .env("FJARR_MEDIA_ENCODER", "auto")
        .output()
        .ok()?;
    let text = String::from_utf8_lossy(&out.stdout);
    let line = text
        .lines()
        .find(|l| l.starts_with("hardware H.264 encode:"))?;
    Some(line.contains(": available"))
}

/// The graphical session, if any, as loginctl sees it: "Wayland session as desktop", "X11 session
/// as kiosk", or none. `setup desktop` (M3) is what acts on it; here it is shown.
pub fn display_session() -> Option<String> {
    let list = Command::new("loginctl")
        .args(["list-sessions", "--no-legend"])
        .output()
        .ok()?;
    for line in String::from_utf8_lossy(&list.stdout).lines() {
        let Some(id) = line.split_whitespace().next() else {
            continue;
        };
        let show = Command::new("loginctl")
            .args([
                "show-session",
                id,
                "-p",
                "Type",
                "-p",
                "Name",
                "-p",
                "Desktop",
            ])
            .output()
            .ok()?;
        let mut props = BTreeMap::new();
        for l in String::from_utf8_lossy(&show.stdout).lines() {
            if let Some((k, v)) = l.split_once('=') {
                props.insert(k.to_string(), v.to_string());
            }
        }
        let server = match props.get("Type").map(String::as_str) {
            Some("wayland") => "Wayland",
            Some("x11") => "X11",
            _ => continue,
        };
        let desktop = props
            .get("Desktop")
            .filter(|d| !d.is_empty())
            .cloned()
            .or_else(|| {
                Command::new("pidof")
                    .arg("gnome-shell")
                    .output()
                    .ok()
                    .filter(|o| o.status.success())
                    .map(|_| "GNOME".to_string())
            });
        let user = props.get("Name").cloned().unwrap_or_default();
        return Some(match desktop {
            Some(d) => format!("{d} on {server} (session of {user})"),
            None => format!("{server} session of {user}"),
        });
    }
    None
}

/// One caps line of a device: `image/jpeg, width=1280, height=720, framerate=30/1`.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct Mode {
    pub media: String,
    pub format: Option<String>,
    pub width: u32,
    pub height: u32,
    /// The highest frame rate the line offers.
    pub fps: u32,
}

/// A device gst-device-monitor-1.0 found.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Default)]
pub struct Device {
    pub name: String,
    /// `device.path`: /dev/video0.
    pub path: Option<String>,
    /// `device.api`: v4l2, pipewire.
    pub api: Option<String>,
    /// `vvvv:pppp`, lower-case, when udev knows the ids.
    pub usb_id: Option<String>,
    pub serial: Option<String>,
    pub modes: Vec<Mode>,
}

/// Parse `gst-device-monitor-1.0 Video/Source` output.
pub fn parse_device_monitor(text: &str) -> Vec<Device> {
    let mut devices = Vec::new();
    let mut current: Option<Device> = None;
    let mut in_caps = false;
    let mut props: BTreeMap<String, String> = BTreeMap::new();
    let finish = |d: Option<Device>,
                  props: &mut BTreeMap<String, String>,
                  out: &mut Vec<Device>| {
        if let Some(mut d) = d {
            d.path = props.get("device.path").cloned();
            d.api = props.get("device.api").cloned();
            d.serial = props.get("device.serial").cloned();
            d.usb_id = match (
                props.get("device.vendor.id"),
                props.get("device.product.id"),
            ) {
                (Some(v), Some(p)) => Some(format!("{}:{}", v.to_lowercase(), p.to_lowercase())),
                _ => None,
            };
            out.push(d);
        }
        props.clear();
    };
    for raw in text.lines() {
        let line = raw.trim();
        if line == "Device found:" {
            finish(current.take(), &mut props, &mut devices);
            current = Some(Device::default());
            in_caps = false;
            continue;
        }
        let Some(d) = current.as_mut() else {
            continue;
        };
        if let Some(v) = line.strip_prefix("name") {
            if let Some(v) = v.trim_start().strip_prefix(':') {
                d.name = v.trim().to_string();
                in_caps = false;
                continue;
            }
        }
        if let Some(v) = line.strip_prefix("caps") {
            if let Some(v) = v.trim_start().strip_prefix(':') {
                in_caps = true;
                if let Some(m) = parse_mode(v.trim()) {
                    d.modes.push(m);
                }
                continue;
            }
        }
        if line.starts_with("properties:") {
            in_caps = false;
            continue;
        }
        if line.starts_with("gst-launch-1.0") || line.starts_with("class") {
            in_caps = false;
            continue;
        }
        if in_caps {
            if let Some(m) = parse_mode(line) {
                d.modes.push(m);
            }
            continue;
        }
        if let Some((k, v)) = line.split_once(" = ") {
            props.insert(k.trim().to_string(), v.trim().trim_matches('"').to_string());
        }
    }
    finish(current.take(), &mut props, &mut devices);
    devices
}

fn parse_mode(line: &str) -> Option<Mode> {
    let mut parts = line.split(", ");
    let media = parts.next()?.trim();
    if !media.contains('/') {
        return None;
    }
    let mut m = Mode {
        media: media.to_string(),
        format: None,
        width: 0,
        height: 0,
        fps: 0,
    };
    for p in parts {
        let Some((k, v)) = p.split_once('=') else {
            continue;
        };
        let v = v.trim();
        match k.trim() {
            "format" => m.format = Some(v.to_string()),
            "width" => m.width = max_int(v),
            "height" => m.height = max_int(v),
            "framerate" => m.fps = max_fraction(v),
            _ => {}
        }
    }
    (m.width > 0 && m.height > 0).then_some(m)
}

/// `640`, or the upper bound of `[ 32, 4096 ]`, or the largest of `{ 640, 1280 }`.
fn max_int(v: &str) -> u32 {
    v.split(|c: char| !c.is_ascii_digit())
        .filter_map(|s| s.parse::<u32>().ok())
        .max()
        .unwrap_or(0)
}

/// The largest `n/d` in a value that may be a single fraction, a list or a range.
fn max_fraction(v: &str) -> u32 {
    let mut best = 0f64;
    for tok in v.split([',', '{', '}', '[', ']', ' ']) {
        if let Some((n, d)) = tok.split_once('/') {
            // `(fraction)30/1`: the type prefix a list carries.
            let n = n.trim_start_matches(|c: char| !c.is_ascii_digit());
            if let (Ok(n), Ok(d)) = (n.parse::<f64>(), d.parse::<f64>()) {
                if d > 0.0 {
                    best = best.max(n / d);
                }
            }
        }
    }
    best.round() as u32
}

/// Run the monitor. `None` when the tool is not installed: detection needs
/// gstreamer1.0-plugins-base-apps, which the fjarr-agent package depends on.
pub fn device_monitor() -> Result<Option<Vec<Device>>> {
    let out = match Command::new("gst-device-monitor-1.0")
        .arg("Video/Source")
        .output()
    {
        Ok(o) => o,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(None),
        Err(e) => return Err(e).context("running gst-device-monitor-1.0"),
    };
    Ok(Some(parse_device_monitor(&String::from_utf8_lossy(
        &out.stdout,
    ))))
}

/// `/dev/v4l/by-id/<name>` → `/dev/videoN`: the name the agent's v4l2 source takes, stable across
/// replug (docs/06). Absent on a machine without cameras.
pub fn by_id_map(dir: &Path) -> BTreeMap<String, String> {
    let mut map = BTreeMap::new();
    let Ok(rd) = std::fs::read_dir(dir) else {
        return map;
    };
    for e in rd.flatten() {
        if let Ok(target) = std::fs::canonicalize(e.path()) {
            let name = e.file_name().to_string_lossy().to_string();
            let target = target.to_string_lossy().to_string();
            // The capture node (index0) is the one to name; a metadata node shares the id.
            let prefer = name.ends_with("index0") || !map.values().any(|t| *t == target);
            if prefer {
                map.retain(|_, t| *t != target);
                map.insert(name, target);
            }
        }
    }
    map
}

/// What `v4l2 device=` should say for a detected device: its by-id name, else its path.
pub fn stable_name(d: &Device, by_id: &BTreeMap<String, String>) -> Option<String> {
    let path = d.path.as_deref()?;
    by_id
        .iter()
        .find(|(_, t)| t.as_str() == path)
        .map(|(n, _)| n.clone())
        .or_else(|| Some(path.to_string()))
}

/// The mode a track proposes: MJPEG 1280×720@30 when the camera has it (the budget's default
/// tier, docs/16, at no decode cost beyond jpegdec), else the largest MJPEG mode up to 1080p at
/// ≥ 15 fps, else the largest raw mode, else nothing (the agent picks). Editable in fjarr.toml.
pub fn pick_mode(modes: &[Mode]) -> Option<&Mode> {
    let jpeg = |m: &&Mode| m.media == "image/jpeg";
    let raw = |m: &&Mode| m.media == "video/x-raw";
    let fits = |m: &&Mode| m.width * m.height <= 1920 * 1080 && m.fps >= 15;
    if let Some(m) = modes
        .iter()
        .filter(jpeg)
        .find(|m| m.width == 1280 && m.height == 720 && m.fps >= 30)
    {
        return Some(m);
    }
    fn largest<'a>(it: impl Iterator<Item = &'a Mode>) -> Option<&'a Mode> {
        it.max_by_key(|m| (m.width * m.height, m.fps))
    }
    largest(modes.iter().filter(jpeg).filter(fits))
        .or_else(|| largest(modes.iter().filter(raw).filter(fits)))
        .or_else(|| largest(modes.iter()))
}

/// The v4l2 source's `format` for a mode (docs/06): the agent accepts auto, mjpeg and yuyv.
pub fn source_format(m: &Mode) -> &'static str {
    match (m.media.as_str(), m.format.as_deref()) {
        ("image/jpeg", _) => "mjpeg",
        ("video/x-raw", Some("YUY2")) => "yuyv",
        _ => "auto",
    }
}

/// A detected device with the catalog's answer.
#[derive(Debug, Clone, Serialize)]
pub struct Detected {
    #[serde(flatten)]
    pub device: Device,
    /// `v4l2 device=` for a built-in source: the by-id name, else the path.
    pub stable_name: Option<String>,
    /// The catalog entry's name, when one matches.
    pub driver: Option<String>,
}

pub fn detect_all(
    catalog: &Catalog,
    devices: Vec<Device>,
    by_id: &BTreeMap<String, String>,
) -> Vec<Detected> {
    devices
        .into_iter()
        .map(|d| {
            let entry: Option<&Entry> = catalog.match_device(d.usb_id.as_deref(), d.api.as_deref());
            Detected {
                stable_name: stable_name(&d, by_id),
                driver: entry.map(|e| e.name.clone()),
                device: d,
            }
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A laptop's UVC camera as gst-device-monitor-1.0 1.28 prints it (two nodes, one is IR).
    pub const SAMPLE: &str = r#"Probing devices...


Device found:

	name  : Integrated Camera: Integrated C
	class : Video/Source
	caps  : video/x-raw, format=YUY2, width=640, height=480, pixel-aspect-ratio=1/1, framerate=30/1
	        video/x-raw, format=YUY2, width=640, height=360, pixel-aspect-ratio=1/1, framerate=30/1
	        image/jpeg, width=2592, height=1944, pixel-aspect-ratio=1/1, framerate=30/1
	        image/jpeg, width=1920, height=1080, pixel-aspect-ratio=1/1, framerate=30/1
	        image/jpeg, width=1280, height=720, pixel-aspect-ratio=1/1, framerate=30/1
	        image/jpeg, width=640, height=480, pixel-aspect-ratio=1/1, framerate={ (fraction)30/1, (fraction)15/1 }
	properties:
		udev-probed = true
		device.bus_path = pci-0000:00:14.0-usb-0:9:1.0
		device.bus = usb
		device.subsystem = video4linux
		device.vendor.id = 04f2
		device.vendor.name = "Chicony\\x20Electronics\\x20Co.\\x2cLtd."
		device.product.id = B805
		device.product.name = "Integrated\ Camera:\ Integrated\ C"
		device.serial = Chicony_Electronics_Co._Ltd._Integrated_Camera_0001
		device.capabilities = :capture:
		device.api = v4l2
		device.path = /dev/video0
		v4l2.device.driver = uvcvideo
	gst-launch-1.0 v4l2src ! ...


Device found:

	name  : Integrated Camera: Integrated I
	class : Video/Source
	caps  : video/x-raw, format=GRAY8, width=640, height=360, pixel-aspect-ratio=1/1, framerate=15/1
	properties:
		udev-probed = true
		device.vendor.id = 04f2
		device.product.id = b805
		device.api = v4l2
		device.path = /dev/video2
	gst-launch-1.0 v4l2src device=/dev/video2 ! ...
"#;

    #[test]
    fn the_monitor_output_parses_into_devices_with_ids_paths_and_modes() {
        let d = parse_device_monitor(SAMPLE);
        assert_eq!(d.len(), 2);
        assert_eq!(d[0].name, "Integrated Camera: Integrated C");
        assert_eq!(d[0].path.as_deref(), Some("/dev/video0"));
        assert_eq!(d[0].api.as_deref(), Some("v4l2"));
        assert_eq!(
            d[0].usb_id.as_deref(),
            Some("04f2:b805"),
            "ids are lower-cased"
        );
        assert_eq!(d[0].modes.len(), 6);
        assert_eq!(
            d[0].modes[2],
            Mode {
                media: "image/jpeg".into(),
                format: None,
                width: 2592,
                height: 1944,
                fps: 30
            }
        );
        assert_eq!(d[0].modes[5].fps, 30, "a framerate list takes its highest");
        assert_eq!(d[1].path.as_deref(), Some("/dev/video2"));
        assert_eq!(d[1].modes[0].format.as_deref(), Some("GRAY8"));
        assert!(parse_device_monitor("Probing devices...\n\n").is_empty());
    }

    #[test]
    fn the_proposed_mode_is_mjpeg_720p30_when_the_camera_has_it_else_the_largest_that_fits() {
        let d = parse_device_monitor(SAMPLE);
        let m = pick_mode(&d[0].modes).unwrap();
        assert_eq!(
            (m.width, m.height, m.fps, source_format(m)),
            (1280, 720, 30, "mjpeg")
        );
        let m = pick_mode(&d[1].modes).unwrap();
        assert_eq!((m.width, m.height, source_format(m)), (640, 360, "auto"));
        let modes = vec![
            Mode {
                media: "image/jpeg".into(),
                format: None,
                width: 3840,
                height: 2160,
                fps: 30,
            },
            Mode {
                media: "image/jpeg".into(),
                format: None,
                width: 1920,
                height: 1080,
                fps: 10,
            },
            Mode {
                media: "video/x-raw".into(),
                format: Some("YUY2".into()),
                width: 1920,
                height: 1080,
                fps: 30,
            },
        ];
        let m = pick_mode(&modes).unwrap();
        assert_eq!(
            (m.width, source_format(m)),
            (1920, "yuyv"),
            "a 4K-only MJPEG camera streams raw 1080p"
        );
        assert!(pick_mode(&[]).is_none());
    }

    #[test]
    fn a_device_takes_its_by_id_name_and_the_catalog_names_the_built_in_driver() {
        let c = Catalog::parse(crate::catalog::SHIPPED).unwrap();
        let by_id: BTreeMap<String, String> = [(
            "usb-Chicony_Integrated_Camera-video-index0".to_string(),
            "/dev/video0".to_string(),
        )]
        .into();
        let all = detect_all(&c, parse_device_monitor(SAMPLE), &by_id);
        assert_eq!(
            all[0].stable_name.as_deref(),
            Some("usb-Chicony_Integrated_Camera-video-index0")
        );
        assert_eq!(all[0].driver.as_deref(), Some("v4l2"));
        assert_eq!(
            all[1].stable_name.as_deref(),
            Some("/dev/video2"),
            "no by-id link: the path"
        );
        assert_eq!(all[1].driver.as_deref(), Some("v4l2"));
    }

    #[test]
    fn the_by_id_map_prefers_the_capture_node_over_a_metadata_node_of_the_same_camera() {
        let dir = tempfile::tempdir().unwrap();
        let target = dir.path().join("video0");
        std::fs::write(&target, "").unwrap();
        std::os::unix::fs::symlink(&target, dir.path().join("usb-Acme-video-index1")).unwrap();
        std::os::unix::fs::symlink(&target, dir.path().join("usb-Acme-video-index0")).unwrap();
        let map = by_id_map(dir.path());
        assert_eq!(map.len(), 1);
        assert!(map.contains_key("usb-Acme-video-index0"), "{map:?}");
    }
}
