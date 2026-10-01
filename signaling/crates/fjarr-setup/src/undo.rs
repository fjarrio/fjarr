//! `fjarr-agent setup --undo [<feature>]`: exactly the recorded changes, newest first; bare, every
//! feature (docs/26#the-setup-tool).
use std::path::Path;

use anyhow::{Context, Result};
use cliclack::log;

use crate::changes::{Change, Record};
use crate::{config, system};

pub async fn undo(config_path: &Path, state: &Path, feature: Option<&str>) -> Result<i32> {
    let what = feature.unwrap_or("everything");
    crate::require_root(&match feature {
        Some(f) => format!("setup --undo {f}"),
        None => "setup --undo".to_string(),
    })?;
    cliclack::intro(format!("fjarr setup --undo {what}"))?;
    let mut record = Record::load(state)?;
    let changes = match feature {
        Some(f) => record.take(f),
        None => record.take_all(),
    };
    if changes.is_empty() {
        cliclack::outro(format!(
            "nothing recorded for {what} in {}",
            state.display()
        ))?;
        return Ok(0);
    }
    let mut units_changed = false;
    let mut dconf_changed = false;
    let mut grub_changed = false;
    for change in changes {
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
                                return Err(e)
                                    .with_context(|| format!("removing {}", path.display()))
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
                    units_changed = true;
                }
                if path.starts_with("/etc/default/grub.d") {
                    grub_changed = true; // ghost screens (docs/26#ghost-screens)
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
                    continue;
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
                let _ = system::run("loginctl", &["terminate-user", &user]);
                std::thread::sleep(std::time::Duration::from_secs(1));
                system::run("userdel", &["--remove", &user]).with_context(|| {
                    format!("removing the account {user} that setup desktop created")
                })?;
                log::step(format!("account {user}: removed, with its home"))?;
            }
            Change::DconfUpdate => dconf_changed = true,
        }
    }
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
