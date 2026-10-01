//! Every change `fjarr-setup` makes is recorded, so `fjarr-agent setup --undo <feature>` reverses
//! exactly those (docs/26#the-setup-tool). Recorded in /var/lib/fjarr/setup-changes.json, with the
//! previous contents of any file it replaced kept beside it.
use std::path::{Path, PathBuf};

use anyhow::{Context, Result};
use serde::{Deserialize, Serialize};

/// Where a replaced file's previous contents are kept for `--undo`.
pub const BACKUPS: &str = "/var/lib/fjarr/setup-backups";

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
    /// A unit started (one the package had already enabled): undone with `systemctl stop`.
    UnitStarted { unit: String },
    /// A tun device created.
    Device { name: String },
    /// One key of `capabilities."<table>"` in the configuration set (`key` may be dotted:
    /// `helper.user`); `previous` is its former value as TOML text, none when it did not exist.
    /// Records from before `table` existed were all `fjarr.net`'s.
    ConfigValue {
        file: PathBuf,
        #[serde(default = "net_table")]
        table: String,
        key: String,
        previous: Option<String>,
    },
    /// An account `setup desktop` created: undone with `userdel --remove`, after its session ends.
    AccountCreated { user: String },
    /// An account added to a group (`usermod -aG`): undone with `gpasswd -d`.
    GroupMember { user: String, group: String },
    /// A symlink made (a user unit enabled for one account): undone by removing it.
    Symlink { path: PathBuf, target: PathBuf },
    /// `dconf update` must run again after its database files change; recorded so `--undo` does.
    DconfUpdate,
}

fn net_table() -> String {
    crate::config::NET_TABLE.to_string()
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
                            file: fa,
                            table: ta,
                            key: ka,
                            ..
                        },
                        Change::ConfigValue {
                            file: fb,
                            table: tb,
                            key: kb,
                            ..
                        },
                    ) => fa == fb && ta == tb && ka == kb,
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
    #[cfg(test)]
    pub fn take(&mut self, feature: &str) -> Vec<Change> {
        let (mine, rest): (Vec<_>, Vec<_>) =
            self.changes.drain(..).partition(|e| e.feature == feature);
        self.changes = rest;
        mine.into_iter().rev().map(|e| e.change).collect()
    }

    /// Every feature's changes, newest first: a bare `setup --undo`.
    #[cfg(test)]
    pub fn take_all(&mut self) -> Vec<Change> {
        self.changes.drain(..).rev().map(|e| e.change).collect()
    }

    #[cfg(test)]
    pub fn features(&self) -> Vec<String> {
        let mut f: Vec<String> = Vec::new();
        for e in &self.changes {
            if !f.contains(&e.feature) {
                f.push(e.feature.clone());
            }
        }
        f
    }

    /// Write `path`, keeping what was there under `backups` for `--undo`, and record it. On a
    /// rerun the first run's backup is the one kept, on disk as in the record (see `add`): the
    /// second run must not replace the customer's file with the first run's output.
    pub fn write_file(
        &mut self,
        feature: &str,
        path: &Path,
        contents: &str,
        backups: &Path,
    ) -> Result<()> {
        let already = self.changes.iter().any(|e| {
            e.feature == feature && matches!(&e.change, Change::File { path: p, .. } if p == path)
        });
        let backup = match std::fs::read(path) {
            Ok(_) if already => None,
            Ok(old) => {
                let name = path
                    .to_string_lossy()
                    .trim_start_matches('/')
                    .replace('/', "%");
                let b = backups.join(name);
                std::fs::create_dir_all(backups)?;
                std::fs::write(&b, old)
                    .with_context(|| format!("keeping a copy of {}", path.display()))?;
                Some(b)
            }
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => None,
            Err(e) => return Err(e).with_context(|| format!("reading {}", path.display())),
        };
        if let Some(dir) = path.parent() {
            std::fs::create_dir_all(dir)?;
        }
        std::fs::write(path, contents).with_context(|| format!("writing {}", path.display()))?;
        self.add(
            feature,
            Change::File {
                path: path.to_path_buf(),
                backup,
            },
        );
        Ok(())
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
            table: "fjarr.net".into(),
            key: "enabled".into(),
            previous: None,
        };
        r.add("net", first.clone());
        r.add(
            "net",
            Change::ConfigValue {
                file: "/etc/fjarr/fjarr.toml".into(),
                table: "fjarr.net".into(),
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
    fn a_bare_undo_takes_every_feature_newest_first() {
        let mut r = Record::default();
        r.add(
            "setup",
            Change::File {
                path: "/etc/fjarr/fjarr.toml".into(),
                backup: None,
            },
        );
        r.add(
            "setup",
            Change::UnitStarted {
                unit: "fjarr-agent.service".into(),
            },
        );
        r.add(
            "net",
            Change::UnitEnabled {
                unit: "fjarr-net.service".into(),
            },
        );
        assert_eq!(r.features(), vec!["setup", "net"]);
        let all = r.take_all();
        assert_eq!(
            all[0],
            Change::UnitEnabled {
                unit: "fjarr-net.service".into()
            }
        );
        assert_eq!(
            all[2],
            Change::File {
                path: "/etc/fjarr/fjarr.toml".into(),
                backup: None
            }
        );
        assert!(r.changes.is_empty());
    }

    #[test]
    fn writing_a_file_twice_keeps_the_first_backup_and_a_new_file_records_none() {
        let dir = tempfile::tempdir().unwrap();
        let backups = dir.path().join("backups");
        let target = dir.path().join("etc/fjarr/fjarr.toml");
        let mut r = Record::default();
        r.write_file("setup", &target, "new\n", &backups).unwrap();
        assert_eq!(
            r.changes[0].change,
            Change::File {
                path: target.clone(),
                backup: None
            }
        );
        std::fs::write(&target, "customer edit\n").unwrap();
        let mut r2 = Record::default();
        r2.write_file("setup", &target, "run one\n", &backups)
            .unwrap();
        r2.write_file("setup", &target, "run two\n", &backups)
            .unwrap();
        let Change::File {
            backup: Some(b), ..
        } = &r2.changes[0].change
        else {
            panic!("{:?}", r2.changes);
        };
        assert_eq!(std::fs::read_to_string(b).unwrap(), "customer edit\n");
        assert_eq!(std::fs::read_to_string(&target).unwrap(), "run two\n");
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
