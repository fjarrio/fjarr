//! `fjarr-agent setup --undo <feature>`: exactly the recorded changes, newest first
//! (docs/26#the-setup-tool).
use std::path::Path;

use anyhow::{Context, Result};
use cliclack::log;

use crate::changes::{Change, Record};
use crate::{config, system};

pub async fn undo(config_path: &Path, state: &Path, feature: &str) -> Result<i32> {
    crate::require_root(&format!("setup --undo {feature}"))?;
    cliclack::intro(format!("fjarr setup --undo {feature}"))?;
    let mut record = Record::load(state)?;
    let changes = record.take(feature);
    if changes.is_empty() {
        cliclack::outro(format!(
            "nothing recorded for {feature:?} in {}",
            state.display()
        ))?;
        return Ok(0);
    }
    let mut units_changed = false;
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
            }
            Change::ConfigValue {
                file,
                key,
                previous,
            } => {
                let mut doc = config::load(&file)?;
                config::restore_net_value(&mut doc, &key, previous.as_deref())?;
                config::save(&file, &doc)?;
                log::step(format!(
                    "{}: capabilities.\"{}\".{key} {}",
                    file.display(),
                    config::NET_TABLE,
                    match previous {
                        Some(v) => format!("= {v} again"),
                        None => "removed".to_string(),
                    }
                ))?;
            }
        }
    }
    record.save(state)?;
    if units_changed {
        system::systemctl(&["daemon-reload"])?;
    }
    // The agent stops using what was undone on its next start; a running one restarts now.
    system::restart_agent_if_running()?;
    cliclack::outro("undone · fjarr-agent --check follows")?;
    crate::agent_check(config_path)
}
