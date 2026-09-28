//! The driver catalog (docs/26#the-driver-catalog): the single source `setup` and `drivers` read,
//! shipped as /usr/share/fjarr/catalog.toml. The catalog is data: a vendor is an entry plus a
//! plugin package, never a change to fjarr-agent.
use std::path::Path;

use anyhow::{bail, Context, Result};
use toml_edit::{DocumentMut, Item};

/// One way an entry recognises attached hardware.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Match {
    /// `vvvv:pppp` as udev reports it, with an optional trailing `*`.
    Usb(String),
    /// gst-device-monitor's `device.api` (`v4l2`, `pipewire`).
    Api(String),
    /// A display server (`x11`, `wayland`); desktop entries, M3.
    Display(String),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Entry {
    pub name: String,
    pub title: String,
    /// Compiled into fjarr-agent; `source` is its `type` in fjarr.toml.
    pub builtin: bool,
    pub source: Option<String>,
    pub package: Option<String>,
    /// What the doctor checks.
    pub element: Option<String>,
    /// Where the package exists; empty means everywhere.
    pub arch: Vec<String>,
    pub matches: Vec<Match>,
    pub prereqs: Vec<String>,
    pub post_install: Option<String>,
    pub docs: Option<String>,
    /// A step the tool cannot do (a SDK behind a EULA).
    pub requires_manual: Option<String>,
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Catalog {
    /// In the file's order: the order `drivers list` shows.
    pub entries: Vec<Entry>,
}

impl Catalog {
    pub fn load(path: &Path) -> Result<Catalog> {
        let text = std::fs::read_to_string(path).with_context(|| {
            format!(
                "reading the driver catalog {}: the fjarr-agent package installs it",
                path.display()
            )
        })?;
        Catalog::parse(&text).with_context(|| format!("parsing {}", path.display()))
    }

    pub fn parse(text: &str) -> Result<Catalog> {
        let doc: DocumentMut = text.parse()?;
        let mut entries = Vec::new();
        for (name, item) in doc.iter() {
            let Some(t) = item.as_table() else {
                bail!("[{name}] is not a table");
            };
            let str_of = |k: &str| t.get(k).and_then(Item::as_str).map(str::to_string);
            let strs_of = |k: &str| -> Vec<String> {
                t.get(k)
                    .and_then(Item::as_array)
                    .map(|a| {
                        a.iter()
                            .filter_map(|v| v.as_str().map(str::to_string))
                            .collect()
                    })
                    .unwrap_or_default()
            };
            let mut matches = Vec::new();
            if let Some(a) = t.get("matches").and_then(Item::as_array) {
                for v in a.iter() {
                    let Some(m) = v.as_inline_table() else {
                        bail!("[{name}] matches: each rule is an inline table like {{ usb = \"8086:0b*\" }}");
                    };
                    let rule =
                        |k: &str| m.get(k).and_then(|v| v.as_str()).map(|s| s.to_lowercase());
                    match (rule("usb"), rule("api"), rule("display")) {
                        (Some(u), _, _) => matches.push(Match::Usb(u)),
                        (_, Some(a), _) => matches.push(Match::Api(a)),
                        (_, _, Some(d)) => matches.push(Match::Display(d)),
                        _ => bail!("[{name}] matches: a rule needs usb, api or display"),
                    }
                }
            }
            let Some(title) = str_of("title") else {
                bail!("[{name}] has no title");
            };
            let builtin = t.get("builtin").and_then(Item::as_bool).unwrap_or(false);
            let entry = Entry {
                name: name.to_string(),
                title,
                builtin,
                source: str_of("source"),
                package: str_of("package"),
                element: str_of("element"),
                arch: strs_of("arch"),
                matches,
                prereqs: strs_of("prereqs"),
                post_install: str_of("post_install"),
                docs: str_of("docs"),
                requires_manual: str_of("requires_manual"),
            };
            if !entry.builtin && entry.package.is_none() {
                bail!("[{name}] is neither builtin nor names a package");
            }
            if entry.builtin && entry.source.is_none() {
                bail!("[{name}] is builtin but names no source type");
            }
            entries.push(entry);
        }
        Ok(Catalog { entries })
    }

    pub fn get(&self, name: &str) -> Option<&Entry> {
        self.entries.iter().find(|e| e.name == name)
    }

    pub fn names(&self) -> Vec<&str> {
        self.entries.iter().map(|e| e.name.as_str()).collect()
    }

    /// The entry for an attached device: a usb id rule beats an api rule (a RealSense is a v4l2
    /// device too, and the vendor entry is the one that brings its depth stream); ties go to the
    /// catalog's order.
    pub fn match_device(&self, usb_id: Option<&str>, api: Option<&str>) -> Option<&Entry> {
        let mut best: Option<(u8, &Entry)> = None;
        for e in &self.entries {
            let score = e
                .matches
                .iter()
                .filter_map(|m| match m {
                    Match::Usb(pat) => usb_id.filter(|id| glob(pat, &id.to_lowercase())).map(|_| 2),
                    Match::Api(a) => api.filter(|x| x.eq_ignore_ascii_case(a)).map(|_| 1),
                    Match::Display(_) => None,
                })
                .max()
                .unwrap_or(0);
            if score > 0 && best.is_none_or(|(s, _)| score > s) {
                best = Some((score, e));
            }
        }
        best.map(|(_, e)| e)
    }
}

/// `pat` with an optional trailing `*`, against `s`. Nothing more: udev ids are hex, and a
/// prefix is what a vendor's product range is.
pub fn glob(pat: &str, s: &str) -> bool {
    match pat.strip_suffix('*') {
        Some(prefix) => s.starts_with(prefix),
        None => pat == s,
    }
}

/// The shipped catalog, so the tests and the tool agree on what is built in.
#[cfg(test)]
pub const SHIPPED: &str = include_str!("../../../../packaging/catalog.toml");

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_shipped_catalog_carries_the_built_in_sources_with_the_elements_the_doctor_checks() {
        let c = Catalog::parse(SHIPPED).unwrap();
        assert_eq!(c.names(), vec!["test", "v4l2", "rtsp"]);
        for e in &c.entries {
            assert!(e.builtin, "{}: vendor entries are M3", e.name);
            assert_eq!(e.source.as_deref(), Some(e.name.as_str()));
            assert!(
                e.element.is_some(),
                "{}: the doctor needs an element",
                e.name
            );
            assert!(e.package.is_none());
        }
        assert_eq!(
            c.get("v4l2").unwrap().matches,
            vec![Match::Api("v4l2".into())]
        );
    }

    #[test]
    fn a_usb_rule_beats_the_api_rule_and_an_unknown_device_matches_nothing() {
        let c = Catalog::parse(&format!(
            "{SHIPPED}\n[realsense]\ntitle = \"Intel RealSense\"\npackage = \"fjarr-gst-realsense\"\narch = [\"amd64\", \"arm64\"]\nmatches = [{{ usb = \"8086:0b*\" }}, {{ usb = \"8086:0a*\" }}]\n"
        ))
        .unwrap();
        assert_eq!(
            c.match_device(Some("046d:0892"), Some("v4l2"))
                .unwrap()
                .name,
            "v4l2"
        );
        assert_eq!(
            c.match_device(Some("8086:0B07"), Some("v4l2"))
                .unwrap()
                .name,
            "realsense"
        );
        assert_eq!(
            c.match_device(Some("8086:0a80"), None).unwrap().name,
            "realsense"
        );
        assert!(c
            .match_device(Some("2b03:f780"), Some("pipewire"))
            .is_none());
        assert!(c.match_device(None, None).is_none());
    }

    #[test]
    fn a_malformed_entry_is_named() {
        let e = Catalog::parse("[zed]\ntitle = \"ZED\"\n")
            .unwrap_err()
            .to_string();
        assert!(e.contains("[zed]") && e.contains("package"), "{e}");
        let e =
            Catalog::parse("[x]\ntitle = \"x\"\npackage = \"p\"\nmatches = [{ serial = \"1\" }]\n")
                .unwrap_err()
                .to_string();
        assert!(e.contains("usb, api or display"), "{e}");
        assert!(Catalog::parse("[x]\npackage = \"p\"\n")
            .unwrap_err()
            .to_string()
            .contains("title"));
    }

    #[test]
    fn the_glob_is_a_prefix_or_an_exact_id() {
        assert!(glob("8086:0b*", "8086:0b07"));
        assert!(!glob("8086:0b*", "8086:0a80"));
        assert!(glob("2b03:f780", "2b03:f780"));
        assert!(!glob("2b03:f780", "2b03:f781"));
    }
}
