//! `fjarr-agent drivers list|detect|install` (docs/26#fjarr-agent-drivers): mostly output, for
//! people and scripts alike (`--json`). The catalog is data; this file only renders it against
//! this machine.
use std::path::Path;

use anyhow::{bail, Result};
use serde::Serialize;

use crate::catalog::{Catalog, Entry};
use crate::{detect, system};

/// An entry's state on this machine.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "kebab-case")]
pub enum Status {
    BuiltIn,
    Installed,
    Available,
    NotForArch,
    NeedsManual,
}

impl Status {
    pub fn label(self, arch: &str) -> String {
        match self {
            Status::BuiltIn => "built in".into(),
            Status::Installed => "installed".into(),
            Status::Available => "available".into(),
            Status::NotForArch => format!("not for {arch}"),
            Status::NeedsManual => "needs manual".into(),
        }
    }
}

pub fn status(e: &Entry, arch: &str, installed: impl Fn(&str) -> bool) -> Status {
    if e.builtin {
        Status::BuiltIn
    } else if !e.arch.is_empty() && !e.arch.iter().any(|a| a == arch) {
        Status::NotForArch
    } else if e.package.as_deref().is_some_and(&installed) {
        Status::Installed
    } else if e.requires_manual.is_some() {
        Status::NeedsManual
    } else {
        Status::Available
    }
}

/// The NOTE column: what a person needs to know before `install`.
pub fn note(e: &Entry, st: Status) -> String {
    match st {
        Status::BuiltIn => match e.source.as_deref() {
            Some(s) => format!("source = {{ type = \"{s}\", … }} in fjarr.toml (docs/06)"),
            None => String::new(),
        },
        Status::NotForArch => format!("available on {}", e.arch.join(", ")),
        Status::NeedsManual => e.requires_manual.clone().unwrap_or_default(),
        Status::Installed | Status::Available => e.prereqs.join("; "),
    }
}

#[derive(Serialize)]
struct ListRow<'a> {
    name: &'a str,
    title: &'a str,
    status: Status,
    package: Option<&'a str>,
    element: Option<&'a str>,
    note: String,
    docs: Option<&'a str>,
}

pub fn list(catalog_path: &Path, json: bool) -> Result<i32> {
    let catalog = Catalog::load(catalog_path)?;
    let arch = detect::deb_arch();
    let rows: Vec<ListRow> = catalog
        .entries
        .iter()
        .map(|e| {
            let st = status(e, arch, system::dpkg_installed);
            ListRow {
                name: &e.name,
                title: &e.title,
                status: st,
                package: e.package.as_deref(),
                element: e.element.as_deref(),
                note: note(e, st),
                docs: e.docs.as_deref(),
            }
        })
        .collect();
    if json {
        println!(
            "{}",
            serde_json::to_string_pretty(&serde_json::json!({
                "catalog": catalog_path,
                "arch": arch,
                "drivers": rows,
            }))?
        );
        return Ok(0);
    }
    println!("  {:<11} {:<15} {:<20} NOTE", "NAME", "STATUS", "PACKAGE");
    for r in &rows {
        println!(
            "  {:<11} {:<15} {:<20} {}",
            r.name,
            r.status.label(arch),
            r.package.unwrap_or("—"),
            r.note
        );
    }
    Ok(0)
}

#[derive(Serialize)]
struct DetectRow {
    #[serde(flatten)]
    found: detect::Detected,
    status: Option<Status>,
}

pub fn detect(catalog_path: &Path, json: bool) -> Result<i32> {
    let catalog = Catalog::load(catalog_path)?;
    let arch = detect::deb_arch();
    let Some(devices) = detect::device_monitor()? else {
        bail!("gst-device-monitor-1.0 is not installed; it comes with gstreamer1.0-plugins-base-apps, which the fjarr-agent package depends on");
    };
    let by_id = detect::by_id_map(Path::new("/dev/v4l/by-id"));
    let rows: Vec<DetectRow> = detect::detect_all(&catalog, devices, &by_id)
        .into_iter()
        .map(|found| {
            let status = found
                .driver
                .as_deref()
                .and_then(|n| catalog.get(n))
                .map(|e| self::status(e, arch, system::dpkg_installed));
            DetectRow { found, status }
        })
        .collect();
    if json {
        println!(
            "{}",
            serde_json::to_string_pretty(&serde_json::json!({ "arch": arch, "devices": rows }))?
        );
        return Ok(0);
    }
    if rows.is_empty() {
        println!("  no video sources found (gst-device-monitor-1.0 Video/Source)");
        return Ok(0);
    }
    for r in &rows {
        let d = &r.found.device;
        let id = match (&d.path, &d.usb_id) {
            (Some(p), _) => p.clone(),
            (None, Some(u)) => format!("usb {u}"),
            (None, None) => "?".into(),
        };
        let driver = match (&r.found.driver, r.status) {
            (Some(n), Some(st)) => format!("{n} ({})", st.label(arch)),
            _ => "no catalog entry".into(),
        };
        let mode = detect::pick_mode(&d.modes)
            .map(|m| {
                format!(
                    "  {} {}×{}@{}",
                    detect::source_format(m),
                    m.width,
                    m.height,
                    m.fps
                )
            })
            .unwrap_or_default();
        println!("  {:<16} {:<32} → {driver}{mode}", id, d.name);
    }
    Ok(0)
}

pub fn install(catalog_path: &Path, name: &str) -> Result<i32> {
    let catalog = Catalog::load(catalog_path)?;
    let Some(e) = catalog.get(name) else {
        bail!(
            "no driver {name:?} in the catalog; it has: {}",
            catalog.names().join(", ")
        );
    };
    if e.builtin {
        println!(
            "{name} is built into fjarr-agent: nothing to install. {}",
            note(e, Status::BuiltIn)
        );
        return Ok(0);
    }
    // Vendor packages, their repositories and the udev reload are M3, with the first vendor entry
    // (docs/26#roadmap); until then the catalog names the package and the tool stays honest.
    bail!(
        "installing vendor drivers is not built in this version (M3): {name} is package {}{}",
        e.package.as_deref().unwrap_or("?"),
        match &e.requires_manual {
            Some(m) => format!(", and needs a manual step first: {m}"),
            None => String::new(),
        }
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    fn entry(name: &str) -> Entry {
        Entry {
            name: name.into(),
            title: name.into(),
            builtin: false,
            source: None,
            package: Some(format!("fjarr-gst-{name}")),
            element: None,
            arch: vec!["amd64".into(), "arm64".into()],
            matches: vec![],
            prereqs: vec!["Intel's apt repository (added for you)".into()],
            post_install: None,
            docs: None,
            requires_manual: None,
        }
    }

    #[test]
    fn the_status_is_built_in_installed_available_not_for_arch_or_needs_manual() {
        let c = Catalog::parse(crate::catalog::SHIPPED).unwrap();
        assert_eq!(
            status(c.get("v4l2").unwrap(), "amd64", |_| false),
            Status::BuiltIn
        );
        let e = entry("realsense");
        assert_eq!(
            status(&e, "amd64", |p| p == "fjarr-gst-realsense"),
            Status::Installed
        );
        assert_eq!(status(&e, "amd64", |_| false), Status::Available);
        assert_eq!(status(&e, "riscv64", |_| false), Status::NotForArch);
        let mut zed = entry("zed");
        zed.requires_manual = Some("ZED SDK from stereolabs.com (EULA)".into());
        assert_eq!(status(&zed, "amd64", |_| false), Status::NeedsManual);
        assert_eq!(
            status(&zed, "amd64", |_| true),
            Status::Installed,
            "installed by hand still counts"
        );
        assert_eq!(
            note(&zed, Status::NeedsManual),
            "ZED SDK from stereolabs.com (EULA)"
        );
        assert_eq!(
            note(&e, Status::Available),
            "Intel's apt repository (added for you)"
        );
        assert_eq!(note(&e, Status::NotForArch), "available on amd64, arm64");
        assert!(note(c.get("v4l2").unwrap(), Status::BuiltIn).contains("type = \"v4l2\""));
        assert_eq!(Status::NotForArch.label("riscv64"), "not for riscv64");
    }
}
