//! Every change `fjarr-setup` makes is recorded, so `fjarr-agent setup --undo <feature>` reverses
//! exactly those (docs/26#the-setup-tool). Recorded in /var/lib/fjarr/setup-changes.json, with the
//! previous contents of any file it replaced kept beside it.
use std::path::{Path, PathBuf};

use anyhow::{Context, Result};
use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "kind", rename_all = "kebab-case")]
pub enum Change {
    /// A file written. `backup` holds what was there before; none means it did not exist.
    File {
        path: PathBuf,
        backup: Option<PathBuf>,
    },
    /// A unit enabled with `systemctl enable`.
    UnitEnabled { unit: String },
    /// A tun device created.
    Device { name: String },
    /// One key of `capabilities."fjarr.net"` in the configuration set; `previous` is its former
    /// value as TOML text, none when the key did not exist.
    ConfigValue {
        file: PathBuf,
        key: String,
        previous: Option<String>,
    },
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Entry {
    pub feature: String,
    #[serde(flatten)]
    pub change: Change,
}

#[derive(Debug, Default, Serialize, Deserialize)]
pub struct Record {
    pub changes: Vec<Entry>,
}

impl Record {
    pub fn load(path: &Path) -> Result<Record> {
        match std::fs::read_to_string(path) {
            Ok(s) => {
                serde_json::from_str(&s).with_context(|| format!("reading {}", path.display()))
            }
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(Record::default()),
            Err(e) => Err(e).with_context(|| format!("reading {}", path.display())),
        }
    }

    pub fn save(&self, path: &Path) -> Result<()> {
        if let Some(dir) = path.parent() {
            std::fs::create_dir_all(dir).with_context(|| format!("creating {}", dir.display()))?;
        }
        std::fs::write(path, serde_json::to_string_pretty(self)? + "\n")
            .with_context(|| format!("writing {}", path.display()))
    }

    /// Record a change, unless the same one is already recorded: running `net setup` twice must
    /// not make `--undo` restore the file from the second run's backup of the first run's output.
    pub fn add(&mut self, feature: &str, change: Change) {
        let already = self.changes.iter().any(|e| {
            e.feature == feature
                && match (&e.change, &change) {
                    (Change::File { path: a, .. }, Change::File { path: b, .. }) => a == b,
                    (
                        Change::ConfigValue {
                            file: fa, key: ka, ..
                        },
                        Change::ConfigValue {
                            file: fb, key: kb, ..
                        },
                    ) => fa == fb && ka == kb,
                    (a, b) => a == b,
                }
        });
        if !already {
            self.changes.push(Entry {
                feature: feature.to_string(),
                change,
            });
        }
    }

    /// The feature's changes, newest first — the order `--undo` reverses them in — removed from
    /// the record.
    pub fn take(&mut self, feature: &str) -> Vec<Change> {
        let (mine, rest): (Vec<_>, Vec<_>) =
            self.changes.drain(..).partition(|e| e.feature == feature);
        self.changes = rest;
        mine.into_iter().rev().map(|e| e.change).collect()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_rerun_does_not_record_a_file_twice_so_undo_restores_the_original() {
        let mut r = Record::default();
        let original = Change::File {
            path: "/etc/fjarr/fjarr.toml".into(),
            backup: Some("/b/1".into()),
        };
        r.add("net", original.clone());
        r.add(
            "net",
            Change::File {
                path: "/etc/fjarr/fjarr.toml".into(),
                backup: Some("/b/2".into()),
            },
        );
        assert_eq!(r.take("net"), vec![original]);
    }

    #[test]
    fn a_config_key_set_twice_keeps_the_value_from_before_the_first_run() {
        let mut r = Record::default();
        let first = Change::ConfigValue {
            file: "/etc/fjarr/fjarr.toml".into(),
            key: "enabled".into(),
            previous: None,
        };
        r.add("net", first.clone());
        r.add(
            "net",
            Change::ConfigValue {
                file: "/etc/fjarr/fjarr.toml".into(),
                key: "enabled".into(),
                previous: Some("true".into()),
            },
        );
        assert_eq!(r.take("net"), vec![first]);
    }

    #[test]
    fn undo_takes_one_feature_newest_first_and_leaves_the_others() {
        let mut r = Record::default();
        r.add(
            "net",
            Change::File {
                path: "/a".into(),
                backup: None,
            },
        );
        r.add(
            "desktop",
            Change::UnitEnabled {
                unit: "x.service".into(),
            },
        );
        r.add(
            "net",
            Change::UnitEnabled {
                unit: "fjarr-net.service".into(),
            },
        );
        r.add(
            "net",
            Change::Device {
                name: "fjarr0".into(),
            },
        );
        let net = r.take("net");
        assert_eq!(
            net[0],
            Change::Device {
                name: "fjarr0".into()
            }
        );
        assert_eq!(
            net[2],
            Change::File {
                path: "/a".into(),
                backup: None
            }
        );
        assert_eq!(r.changes.len(), 1);
    }

    #[test]
    fn the_record_survives_a_round_trip() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("setup-changes.json");
        let mut r = Record::default();
        r.add(
            "net",
            Change::Device {
                name: "fjarr0".into(),
            },
        );
        r.save(&path).unwrap();
        assert_eq!(Record::load(&path).unwrap().changes, r.changes);
        assert!(Record::load(&dir.path().join("none.json"))
            .unwrap()
            .changes
            .is_empty());
    }
}
