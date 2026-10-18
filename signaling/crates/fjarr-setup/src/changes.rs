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

    /// The accounts `feature` recorded as created: what an earlier run's files may name.
    pub fn accounts_created(&self, feature: &str) -> Vec<String> {
        self.changes
            .iter()
            .filter(|e| e.feature == feature)
            .filter_map(|e| match &e.change {
                Change::AccountCreated { user } => Some(user.clone()),
                _ => None,
            })
            .collect()
    }

    /// Write `path`, keeping what was there under `backups` for `--undo`, and record it. On a
    /// rerun the first run's backup is the one kept, on disk as in the record (see `add`): the
    /// second run must not replace the customer's file with the first run's output. A recorded
    /// backup that is gone is healed first (`heal_backup`), and what it did is said. `ours` says
    /// whether contents other than `contents` are an earlier setup's output (its header, an
    /// account the record created): such a file is never kept as the original.
    pub fn write_file(
        &mut self,
        feature: &str,
        path: &Path,
        contents: &str,
        backups: &Path,
        ours: &dyn Fn(&str) -> bool,
    ) -> Result<()> {
        if let Some(note) = self.heal_backup(feature, path, contents, ours)? {
            cliclack::log::warning(note)?;
        }
        let already = self.changes.iter().any(|e| {
            e.feature == feature && matches!(&e.change, Change::File { path: p, .. } if p == path)
        });
        let backup = match std::fs::read(path) {
            Ok(_) if already => None,
            Ok(old) => {
                let b = backups.join(backup_name(path));
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

    /// A recorded file whose kept copy is gone: an undo before 26453b3 (2026-10-05) stopped
    /// between restoring the file and saving the record, and every rerun since saw the entry and
    /// took no copy, so `--undo` could never restore it (M3 gate review, finding 18). Before `path`
    /// is written over, keep a fresh copy of it as it is now under the recorded name, or, when it
    /// is gone, record that there was none. When it already holds `contents` or what `ours` takes
    /// for an earlier setup's output, a copy would let `--undo` "restore" setup's output, so none
    /// is taken and the entry stays as it is until the original is put back by hand (or the file
    /// is copied to the kept copy's name by a person who knows it is the original). The note says
    /// which; none when the entry is not stranded.
    /// spec: docs/26-robot-install-and-drivers.md#the-setup-tool
    pub fn heal_backup(
        &mut self,
        feature: &str,
        path: &Path,
        contents: &str,
        ours: &dyn Fn(&str) -> bool,
    ) -> Result<Option<String>> {
        let Some(entry) = self.changes.iter_mut().find(|e| {
            e.feature == feature && matches!(&e.change, Change::File { path: p, .. } if p == path)
        }) else {
            return Ok(None);
        };
        let Change::File {
            backup: Some(b), ..
        } = &entry.change
        else {
            return Ok(None);
        };
        if !copy_is_gone(b)? {
            return Ok(None);
        }
        let b = b.clone();
        let gone = format!(
            "{}: the copy of its previous contents that setup kept for `--undo` ({}) is gone",
            path.display(),
            b.display()
        );
        match std::fs::read(path) {
            Ok(now) if now == contents.as_bytes() || std::str::from_utf8(&now).is_ok_and(ours) => {
                Ok(Some(format!(
                    "{gone}, and the file already holds {}, so its original is lost. No copy \
                     taken: `--undo {feature}` leaves the file as it is and says so. Put its \
                     original back by hand (a package's file: the version it shipped), then run \
                     this setup again, which keeps a copy of it. If the file as it is now is its \
                     original, `{}` makes it the copy `--undo` restores",
                    if now == contents.as_bytes() {
                        "what setup writes"
                    } else {
                        "an earlier setup's output"
                    },
                    keep_as_original(path, &b)
                )))
            }
            Ok(now) => {
                if let Some(dir) = b.parent() {
                    std::fs::create_dir_all(dir)?;
                }
                std::fs::write(&b, now)
                    .with_context(|| format!("keeping a copy of {}", path.display()))?;
                Ok(Some(format!(
                    "{gone}: kept a fresh copy of the file as it is now, before changing it, so \
                     `--undo {feature}` puts this version back"
                )))
            }
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => {
                entry.change = Change::File {
                    path: path.to_path_buf(),
                    backup: None,
                };
                Ok(Some(format!(
                    "{gone}, and so is the file: recorded as setup's own, so `--undo {feature}` \
                     removes it"
                )))
            }
            Err(e) => Err(e).with_context(|| format!("reading {}", path.display())),
        }
    }
}

/// For a caller whose files carry nothing that marks them as setup's: only `contents` itself is.
pub fn only_its_output(_: &str) -> bool {
    false
}

/// The command a person runs to say a file whose kept copy is gone is its original as it is now:
/// setup never decides that on its own (docs/26#the-setup-tool).
pub fn keep_as_original(path: &Path, backup: &Path) -> String {
    format!("sudo cp -p {} {}", shell_quote(path), shell_quote(backup))
}

/// A path for a command line: a backup's `%` is harmless to a shell; anything else odd is quoted.
fn shell_quote(p: &Path) -> String {
    let s = p.display().to_string();
    if s.chars()
        .all(|c| c.is_ascii_alphanumeric() || "/%._-".contains(c))
    {
        s
    } else {
        format!("'{}'", s.replace('\'', "'\\''"))
    }
}

/// Where `path`'s previous contents are kept under the backups directory: the path with `/` as
/// `%`.
fn backup_name(path: &Path) -> String {
    path.to_string_lossy()
        .trim_start_matches('/')
        .replace('/', "%")
}

/// True when a kept copy is gone (not merely unreadable: that is an error, said as one).
pub fn copy_is_gone(backup: &Path) -> Result<bool> {
    match std::fs::symlink_metadata(backup) {
        Ok(_) => Ok(false),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(true),
        Err(e) => Err(e).with_context(|| format!("looking for {}", backup.display())),
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
        r.write_file("setup", &target, "new\n", &backups, &only_its_output)
            .unwrap();
        assert_eq!(
            r.changes[0].change,
            Change::File {
                path: target.clone(),
                backup: None
            }
        );
        std::fs::write(&target, "customer edit\n").unwrap();
        let mut r2 = Record::default();
        r2.write_file("setup", &target, "run one\n", &backups, &only_its_output)
            .unwrap();
        r2.write_file("setup", &target, "run two\n", &backups, &only_its_output)
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

    /// A recorded file, then an undo stopped between restoring it and saving the record (before
    /// 26453b3): the file is the original again, its copy is gone, the entry is still there.
    fn stranded(dir: &Path, original: &str, ours: &str) -> (Record, PathBuf, PathBuf) {
        let backups = dir.join("backups");
        let target = dir.join("etc/gdm3/custom.conf");
        std::fs::create_dir_all(target.parent().unwrap()).unwrap();
        std::fs::write(&target, original).unwrap();
        let mut r = Record::default();
        r.write_file("desktop", &target, ours, &backups, &only_its_output)
            .unwrap();
        let Change::File {
            backup: Some(copy), ..
        } = r.changes[0].change.clone()
        else {
            panic!("{:?}", r.changes);
        };
        std::fs::copy(&copy, &target).unwrap();
        std::fs::remove_file(&copy).unwrap();
        (r, target, copy)
    }

    /// M3 gate review, finding 18: before the heal, a rerun saw the entry and took no copy, so the
    /// original was overwritten with nothing for `--undo` to restore it from.
    #[test]
    fn a_rerun_heals_a_stranded_entry_with_a_fresh_copy_before_writing() {
        let dir = tempfile::tempdir().unwrap();
        let (mut r, target, copy) = stranded(dir.path(), "original\n", "ours\n");
        let note = r
            .heal_backup("desktop", &target, "ours\n", &only_its_output)
            .unwrap();
        assert!(
            note.as_deref().is_some_and(|n| n.contains("fresh copy")),
            "{note:?}"
        );
        assert_eq!(std::fs::read_to_string(&copy).unwrap(), "original\n");
        // Healed: nothing more to say on the next run.
        assert_eq!(
            r.heal_backup("desktop", &target, "ours\n", &only_its_output)
                .unwrap(),
            None
        );
        // The whole rerun, from the stranded state again.
        let dir = tempfile::tempdir().unwrap();
        let (mut r, target, copy) = stranded(dir.path(), "original\n", "ours\n");
        r.write_file(
            "desktop",
            &target,
            "ours\n",
            &dir.path().join("backups"),
            &only_its_output,
        )
        .unwrap();
        assert_eq!(std::fs::read_to_string(&copy).unwrap(), "original\n");
        assert_eq!(std::fs::read_to_string(&target).unwrap(), "ours\n");
        assert_eq!(
            r.changes,
            vec![Entry {
                feature: "desktop".into(),
                change: Change::File {
                    path: target,
                    backup: Some(copy)
                }
            }]
        );
    }

    /// A file that already holds setup's output (the mini-PC since 2026-10-05): a copy would let
    /// `--undo` "restore" setup's own output, so none is taken and the original is said lost.
    #[test]
    fn a_stranded_file_already_holding_setups_output_gets_no_copy() {
        let dir = tempfile::tempdir().unwrap();
        let (mut r, target, copy) = stranded(dir.path(), "original\n", "ours\n");
        std::fs::write(&target, "ours\n").unwrap();
        let before = r.changes.clone();
        let note = r
            .heal_backup("desktop", &target, "ours\n", &only_its_output)
            .unwrap();
        assert!(
            note.as_deref()
                .is_some_and(|n| n.contains("original is lost")),
            "{note:?}"
        );
        assert!(!copy.exists());
        assert_eq!(
            r.changes, before,
            "the entry must still say the file is not the original"
        );
        // The way out for a person who knows the file is its original, named in the note.
        let note = note.unwrap();
        assert!(note.contains(&keep_as_original(&target, &copy)), "{note}");
    }

    /// An earlier setup's output, not this run's (another account then, say): byte equality alone
    /// would take it for the original and let `--undo` restore setup's own earlier output.
    #[test]
    fn a_stranded_file_holding_an_earlier_setups_output_gets_no_copy() {
        let dir = tempfile::tempdir().unwrap();
        let (mut r, target, copy) = stranded(dir.path(), "original\n", "# fjarr: ours, run two\n");
        std::fs::write(&target, "# fjarr: ours, run one\n").unwrap();
        let ours = |t: &str| t.starts_with("# fjarr:");
        let before = r.changes.clone();
        let note = r
            .heal_backup("desktop", &target, "# fjarr: ours, run two\n", &ours)
            .unwrap();
        assert!(
            note.as_deref()
                .is_some_and(|n| n.contains("an earlier setup's output")),
            "{note:?}"
        );
        assert!(!copy.exists());
        assert_eq!(r.changes, before);
        // The same file not taken for setup's by the caller: copied, as any other contents are.
        let note = r
            .heal_backup(
                "desktop",
                &target,
                "# fjarr: ours, run two\n",
                &only_its_output,
            )
            .unwrap();
        assert!(note.is_some_and(|n| n.contains("fresh copy")));
        assert_eq!(
            std::fs::read_to_string(&copy).unwrap(),
            "# fjarr: ours, run one\n"
        );
    }

    #[test]
    fn the_keep_as_original_command_quotes_what_a_shell_would_split() {
        assert_eq!(
            keep_as_original(
                Path::new("/etc/gdm3/custom.conf"),
                Path::new("/var/lib/fjarr/setup-backups/etc%gdm3%custom.conf")
            ),
            "sudo cp -p /etc/gdm3/custom.conf /var/lib/fjarr/setup-backups/etc%gdm3%custom.conf"
        );
        assert_eq!(
            keep_as_original(Path::new("/home/a b/it's"), Path::new("/b")),
            "sudo cp -p '/home/a b/it'\\''s' /b"
        );
    }

    #[test]
    fn a_stranded_file_that_is_gone_is_recorded_as_setups_own() {
        let dir = tempfile::tempdir().unwrap();
        let (mut r, target, _) = stranded(dir.path(), "original\n", "ours\n");
        std::fs::remove_file(&target).unwrap();
        assert!(r
            .heal_backup("desktop", &target, "ours\n", &only_its_output)
            .unwrap()
            .is_some());
        assert_eq!(
            r.changes[0].change,
            Change::File {
                path: target,
                backup: None
            }
        );
    }

    #[test]
    fn an_entry_with_its_copy_in_place_or_another_features_is_left_alone() {
        let dir = tempfile::tempdir().unwrap();
        let backups = dir.path().join("backups");
        let target = dir.path().join("f");
        std::fs::write(&target, "original\n").unwrap();
        let mut r = Record::default();
        r.write_file("desktop", &target, "ours\n", &backups, &only_its_output)
            .unwrap();
        assert_eq!(
            r.heal_backup("desktop", &target, "x\n", &only_its_output)
                .unwrap(),
            None
        );
        std::fs::remove_dir_all(&backups).unwrap();
        assert_eq!(
            r.heal_backup("net", &target, "x\n", &only_its_output)
                .unwrap(),
            None
        );
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
