//! `fjarr-agent setup --undo [<feature>]`: exactly the recorded changes, newest first; bare, every
//! feature (docs/26#the-setup-tool).
use std::path::Path;

use anyhow::{bail, Context, Result};
use cliclack::log;

use crate::changes::{Change, Entry, Record};
use crate::{config, system};

pub async fn undo(config_path: &Path, state: &Path, feature: Option<&str>) -> Result<i32> {
    let what = feature.unwrap_or("everything");
    crate::require_root(&match feature {
        Some(f) => format!("setup --undo {f}"),
        None => "setup --undo".to_string(),
    })?;
    cliclack::intro(format!("fjarr setup --undo {what}"))?;
    let mut record = Record::load(state)?;
    // This feature's changes (or all), newest first; every one is attempted, and one that fails is
    // kept in the record for the next --undo rather than stopping the rest (the mini-PC, 2026-10-01:
    // a userdel refused mid-way left everything after it undone and the record unsaved).
    let all = std::mem::take(&mut record.changes);
    let (mine, rest): (Vec<Entry>, Vec<Entry>) = all
        .into_iter()
        .partition(|e| feature.is_none_or(|f| e.feature == f));
    if mine.is_empty() {
        cliclack::outro(format!(
            "nothing recorded for {what} in {}",
            state.display()
        ))?;
        return Ok(0);
    }
    let mut flags = Flags::default();
    let mut failed: Vec<Entry> = Vec::new();
    let mut pending = mine;
    // The record is saved after every change: an undo stopped half-way (Ctrl-C during
    // update-grub, the mini-PC 2026-10-05) left the whole record in place, and the next --undo
    // looked for backups the first had already restored and removed.
    while let Some(entry) = pending.pop() {
        if let Err(e) = undo_one(&entry.change, &mut flags).await {
            log::warning(format!(
                "{e:#} — kept in the record; `--undo` tries it again"
            ))?;
            failed.insert(0, entry);
        }
        let mut now = rest.clone();
        now.extend(pending.iter().cloned());
        now.extend(failed.iter().cloned());
        record.changes = now;
        record.save(state)?;
    }
    let any_failed = !failed.is_empty();
    let (units_changed, dconf_changed, grub_changed) = (flags.units, flags.dconf, flags.grub);
    if grub_changed && Path::new("/usr/sbin/update-grub").exists() {
        // The kernel line the ghosts were on goes back on the next boot.
        system::run("update-grub", &[])?;
        log::step("update-grub: the kernel command line is back as it was, from the next boot")?;
    }
    if dconf_changed {
        // The database files are back as they were; dconf's compiled database must follow.
        let _ = system::run("dconf", &["update"]);
    }
    record.save(state)?;
    if units_changed {
        system::systemctl(&["daemon-reload"])?;
    }
    // The agent stops using what was undone on its next start; a running one restarts now (and
    // stays down when its configuration is gone: the unit waits for the file).
    if system::systemd_running() {
        system::restart_agent_if_running()?;
    }
    // Setup's own file undone: there is nothing left for --check to verify, and the next step is
    // the one --check would name anyway.
    if any_failed {
        cliclack::outro_cancel("undone, except what is warned about above")?;
        return Ok(1);
    }
    if !config_path.exists() {
        cliclack::outro(format!(
            "undone · {} is gone; `sudo fjarr-agent setup` writes it again",
            config_path.display()
        ))?;
        return Ok(0);
    }
    cliclack::outro("undone · fjarr-agent --check follows")?;
    crate::agent_check(config_path)
}

/// What the reversed changes need afterwards.
#[derive(Default)]
struct Flags {
    units: bool,
    dconf: bool,
    grub: bool,
}

/// Reverse one recorded change.
async fn undo_one(change: &Change, flags: &mut Flags) -> Result<()> {
    let change = change.clone();
    match change {
        Change::Device { name } => {
            let was = fjarr_netdev::delete_link(&name).await?;
            log::step(format!(
                "device {name}: {}",
                if was { "deleted" } else { "was already gone" }
            ))?;
        }
        Change::UnitEnabled { unit } => {
            system::systemctl(&["disable", "--now", &unit])?;
            log::step(format!("unit {unit}: disabled and stopped"))?;
        }
        Change::UnitStarted { unit } => {
            system::systemctl(&["stop", &unit])?;
            log::step(format!("unit {unit}: stopped"))?;
        }
        Change::File { path, backup } => {
            match backup {
                Some(b) => {
                    std::fs::copy(&b, &path).with_context(|| {
                        format!("restoring {} from {}", path.display(), b.display())
                    })?;
                    let _ = std::fs::remove_file(&b);
                    log::step(format!("{}: previous contents restored", path.display()))?;
                }
                None => {
                    match std::fs::remove_file(&path) {
                        Ok(()) => {}
                        Err(e) if e.kind() == std::io::ErrorKind::NotFound => {}
                        Err(e) => {
                            return Err(e).with_context(|| format!("removing {}", path.display()))
                        }
                    }
                    // A drop-in directory setup created is removed with its only file.
                    if let Some(dir) = path.parent() {
                        if dir.extension().is_some_and(|e| e == "d") {
                            let _ = std::fs::remove_dir(dir);
                        }
                    }
                    log::step(format!("{}: removed", path.display()))?;
                }
            }
            if path.starts_with("/etc/systemd") {
                flags.units = true;
            }
            if path.starts_with("/etc/default/grub.d") {
                flags.grub = true; // ghost screens (docs/26#ghost-screens)
            }
        }
        Change::ConfigValue {
            file,
            table,
            key,
            previous,
        } => {
            // The file may already be gone: a bare undo that removed setup's own file first.
            if !file.exists() {
                log::step(format!("{}: already removed", file.display()))?;
                return Ok(());
            }
            let mut doc = config::load(&file)?;
            config::restore_cap_value(&mut doc, &table, &key, previous.as_deref())?;
            config::save(&file, &doc)?;
            log::step(format!(
                "{}: capabilities.\"{table}\".{key} {}",
                file.display(),
                match previous {
                    Some(v) => format!("= {v} again"),
                    None => "removed".to_string(),
                }
            ))?;
        }
        Change::Symlink { path, .. } => match std::fs::remove_file(&path) {
            Ok(()) => log::step(format!("{}: removed", path.display()))?,
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => {
                log::step(format!("{}: already gone", path.display()))?
            }
            Err(e) => return Err(e).with_context(|| format!("removing {}", path.display())),
        },
        Change::GroupMember { user, group } => {
            if system::run("gpasswd", &["-d", &user, &group]).is_ok() {
                log::step(format!("{user}: no longer in {group}"))?;
            } else {
                log::step(format!("{user}: was not in {group} (or is gone)"))?;
            }
        }
        Change::AccountCreated { user } => {
            // Its session first: userdel refuses an account with running processes.
            if !end_session(&user) {
                // A display manager's automatic login brings the session straight back (LightDM
                // on the mini-PC's X11 kiosk, 2026-10-05): say so, rather than userdel's "in use".
                bail!(
                    "the account {user} that setup desktop created is still logged in (a display \
                     manager's automatic login logs it straight back in): switch that off or log \
                     {user} out, then `--undo` again"
                );
            }
            system::run("userdel", &["--remove", &user]).with_context(|| {
                format!("removing the account {user} that setup desktop created")
            })?;
            log::step(format!("account {user}: removed, with its home"))?;
        }
        Change::DconfUpdate => flags.dconf = true,
    }
    Ok(())
}

/// End the account's session and wait for its processes to go: userdel refuses an account that
/// still runs anything, and a GNOME session takes seconds to exit.
/// False when it still runs something after 20 s.
fn end_session(user: &str) -> bool {
    let _ = system::run("loginctl", &["terminate-user", user]);
    for _ in 0..40 {
        if system::run("pgrep", &["-u", user]).is_err() {
            return true;
        }
        std::thread::sleep(std::time::Duration::from_millis(500));
    }
    false
}

#[cfg(test)]
mod tests {
    use super::*;

    /// `--undo desktop` takes back the welcome wizard's masks setup made in an account's home, and
    /// nothing the account had there (docs/26#fjarr-agent-setup-desktop).
    #[tokio::test]
    async fn undoing_the_desktop_removes_the_wizards_masks_and_nothing_else() {
        use std::os::unix::fs::MetadataExt;
        let dir = tempfile::tempdir().unwrap();
        let (home, state) = (
            dir.path().join("home"),
            dir.path().join("setup-changes.json"),
        );
        let units = home.join(".config/systemd/user");
        std::fs::create_dir_all(&units).unwrap();
        let own = units.join("robot-ui.service");
        std::fs::write(&own, "[Service]\nExecStart=/usr/local/bin/robot-ui\n").unwrap();
        let me = std::fs::metadata(&home).unwrap();
        let mut record = Record::default();
        crate::desktop::mask_initial_setup(&mut record, &state, &home, me.uid(), me.gid()).unwrap();
        let changes = Record::load(&state).unwrap().take(crate::desktop::FEATURE);
        assert_eq!(changes.len(), 2, "{changes:?}");
        let mut flags = Flags::default();
        for c in &changes {
            undo_one(c, &mut flags).await.unwrap();
        }
        for unit in [
            "gnome-initial-setup-first-login.service",
            "gnome-initial-setup-upgrade-login.service",
        ] {
            assert!(
                std::fs::symlink_metadata(units.join(unit)).is_err(),
                "{unit} is still masked"
            );
        }
        assert!(own.exists(), "the account's own unit went too");
        // Already gone (a second --undo, or removed by hand): not an error.
        for c in &changes {
            undo_one(c, &mut flags).await.unwrap();
        }
    }
}
