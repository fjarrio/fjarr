//! `setup desktop` and the GDM watchdog: a GNOME robot whose desktop is reachable with nobody at
//! the machine (docs/26#fjarr-agent-setup-desktop, ADR-0006, ADR-0028).
//!
//! What it writes, each change recorded for `setup --undo desktop`: the account (created when
//! asked) in the `fjarr-desktop` group, GDM's automatic login, no screen lock or idle blank, the
//! session helper's user unit enabled for that account only, the watchdog timer, and the agent's
//! `helper.user` / `helper.group`.
use std::os::unix::fs::{chown, symlink};
use std::path::{Path, PathBuf};

use anyhow::{bail, Context, Result};
use cliclack::log;

use crate::changes::{Change, Record};
use crate::{config, system, ui, DesktopArgs, YesNo};

pub const FEATURE: &str = "desktop";
pub const PACKAGE: &str = "fjarr-desktop-wayland";
pub const GROUP: &str = "fjarr-desktop";
pub const TABLE: &str = "fjarr.desktop";
pub const DEFAULT_ACCOUNT: &str = "desktop";
pub const GDM_CONF: &str = "/etc/gdm3/custom.conf";
pub const DCONF_DB: &str = "/etc/dconf/db/local.d/00-fjarr-desktop";
pub const DCONF_PROFILE: &str = "/etc/dconf/profile/user";
pub const HELPER_UNIT: &str = "fjarr-desktop-session.service";
pub const HELPER_UNIT_PATH: &str = "/usr/lib/systemd/user/fjarr-desktop-session.service";
pub const WATCHDOG_TIMER: &str = "fjarr-desktop-watchdog.timer";
/// Consecutive misses before the watchdog restarts GDM (two checks, 30 s apart).
pub const WATCHDOG_MISSES: u32 = 2;
const WATCHDOG_STATE: &str = "/run/fjarr/desktop-watchdog";

/// GDM's configuration with the automatic login for `user` in `[daemon]`, every other line kept.
pub fn gdm_autologin(existing: &str, user: &str) -> String {
    let wanted = [
        ("AutomaticLoginEnable", "true".to_string()),
        ("AutomaticLogin", user.to_string()),
    ];
    let mut out: Vec<String> = Vec::new();
    let mut in_daemon = false;
    let mut seen_daemon = false;
    let mut written = [false, false];
    let flush = |out: &mut Vec<String>, written: &mut [bool; 2]| {
        for (i, (k, v)) in wanted.iter().enumerate() {
            if !written[i] {
                out.push(format!("{k}={v}"));
                written[i] = true;
            }
        }
    };
    for line in existing.lines() {
        let t = line.trim();
        if t.starts_with('[') {
            if in_daemon {
                flush(&mut out, &mut written);
            }
            in_daemon = t.eq_ignore_ascii_case("[daemon]");
            seen_daemon |= in_daemon;
            out.push(line.to_string());
            continue;
        }
        if in_daemon {
            // The keys we own replace any setting of them, commented-out examples included.
            let key = t
                .trim_start_matches('#')
                .split('=')
                .next()
                .unwrap_or("")
                .trim();
            if let Some(i) = wanted.iter().position(|(k, _)| *k == key) {
                if !written[i] {
                    out.push(format!("{}={}", wanted[i].0, wanted[i].1));
                    written[i] = true;
                }
                continue;
            }
        }
        out.push(line.to_string());
    }
    if in_daemon {
        flush(&mut out, &mut written);
    }
    if !seen_daemon {
        if !out.is_empty() && !out.last().is_some_and(|l| l.trim().is_empty()) {
            out.push(String::new());
        }
        out.push("[daemon]".to_string());
        flush(&mut out, &mut written);
    }
    out.join("\n") + "\n"
}

/// The dconf keys that keep an unattended desktop reachable: no lock (the account has no password),
/// no idle blank.
pub fn dconf_db_text() -> &'static str {
    "# Written by `fjarr-agent setup desktop`: the desktop logs in by itself and has no password, so a\n\
     # lock screen would strand it, and a blanked screen is all an operator would see.\n\
     # Undo: fjarr-agent setup --undo desktop\n\
     [org/gnome/desktop/screensaver]\n\
     lock-enabled=false\n\
     \n\
     [org/gnome/desktop/session]\n\
     idle-delay=uint32 0\n"
}

/// The dconf user profile with the system `local` database in it; None when it already has it.
pub fn dconf_profile(existing: Option<&str>) -> Option<String> {
    match existing {
        None => Some("user-db:user\nsystem-db:local\n".to_string()),
        Some(text) if text.lines().any(|l| l.trim() == "system-db:local") => None,
        Some(text) => {
            let mut t = text.to_string();
            if !t.ends_with('\n') && !t.is_empty() {
                t.push('\n');
            }
            if !t.lines().any(|l| l.trim().starts_with("user-db:")) {
                t = format!("user-db:user\n{t}");
            }
            t.push_str("system-db:local\n");
            Some(t)
        }
    }
}

/// Where enabling the helper's user unit for one account puts its symlink.
pub fn helper_wants_link(home: &Path) -> PathBuf {
    home.join(".config/systemd/user/graphical-session.target.wants")
        .join(HELPER_UNIT)
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum WatchdogAction {
    /// The account has its session on seat0 (or there is nothing to watch).
    Healthy,
    /// No session this check: the count of consecutive misses so far.
    Missing(u32),
    /// Missed often enough: restart GDM so the automatic login runs again.
    RestartGdm,
}

/// One watchdog check (docs/26#fjarr-agent-setup-desktop): `sessions` is `loginctl list-sessions
/// --json=short`. GDM down is systemd's to restart, not ours.
pub fn watchdog_decide(
    sessions: &str,
    user: &str,
    gdm_active: bool,
    misses: u32,
) -> WatchdogAction {
    if !gdm_active {
        return WatchdogAction::Healthy;
    }
    let list: Vec<serde_json::Value> = serde_json::from_str(sessions).unwrap_or_default();
    let present = list.iter().any(|s| {
        s.get("user").and_then(|u| u.as_str()) == Some(user)
            && s.get("seat").and_then(|v| v.as_str()) == Some("seat0")
    });
    if present {
        WatchdogAction::Healthy
    } else if misses + 1 >= WATCHDOG_MISSES {
        WatchdogAction::RestartGdm
    } else {
        WatchdogAction::Missing(misses + 1)
    }
}

/// The configured desktop account: `capabilities."fjarr.desktop".helper.user`.
pub fn configured_account(doc: &toml_edit::DocumentMut) -> Option<String> {
    doc.get("capabilities")?
        .get(TABLE)?
        .get("helper")?
        .get("user")?
        .as_str()
        .map(str::to_string)
}

/// What `fjarr-desktop-watchdog.service` runs every 30 s. Plain lines, for the journal.
pub fn watchdog(config_path: &Path) -> Result<()> {
    crate::require_root("desktop watchdog")?;
    let Some(user) = config::load(config_path)
        .ok()
        .as_ref()
        .and_then(configured_account)
    else {
        println!("desktop watchdog: no desktop account configured (capabilities.\"{TABLE}\".helper.user); nothing to watch");
        return Ok(());
    };
    let gdm_active = system::unit_active("gdm.service");
    let sessions =
        system::run("loginctl", &["list-sessions", "--json=short"]).unwrap_or_else(|_| "[]".into());
    let misses: u32 = std::fs::read_to_string(WATCHDOG_STATE)
        .ok()
        .and_then(|s| s.trim().parse().ok())
        .unwrap_or(0);
    match watchdog_decide(&sessions, &user, gdm_active, misses) {
        WatchdogAction::Healthy => {
            let _ = std::fs::remove_file(WATCHDOG_STATE);
        }
        WatchdogAction::Missing(n) => {
            let _ = std::fs::write(WATCHDOG_STATE, n.to_string());
            println!("desktop watchdog: {user} has no session on seat0 ({n} of {WATCHDOG_MISSES})");
        }
        WatchdogAction::RestartGdm => {
            let _ = std::fs::remove_file(WATCHDOG_STATE);
            println!("desktop watchdog: {user} has had no session on seat0 for {WATCHDOG_MISSES} checks; restarting GDM so it logs in again");
            system::systemctl(&["restart", "gdm.service"])?;
        }
    }
    Ok(())
}

/// What `setup desktop` would do, for a system it does not change (docs/26#the-setup-tool).
fn print_options() -> Result<()> {
    log::warning(
        "This is not Ubuntu with apt, so nothing is changed. To set up the desktop by hand:",
    )?;
    cliclack::note(
        "options to set",
        format!(
            "1. An account that logs in automatically ({GDM_CONF}: [daemon] AutomaticLoginEnable=true, AutomaticLogin=<account>)\n\
             2. That account in the {GROUP} group, with no screen lock and no idle blanking\n\
             3. {HELPER_UNIT} enabled for that account: systemctl --user enable {HELPER_UNIT}\n\
             4. In {}: [capabilities.\"{TABLE}\".helper] user = \"<account>\", group = \"{GROUP}\"",
            crate::DEFAULT_CONFIG
        ),
    )?;
    Ok(())
}

fn write_recorded(record: &mut Record, path: &Path, contents: &str) -> Result<()> {
    record.write_file(FEATURE, path, contents, Path::new(crate::changes::BACKUPS))
}

fn set_value(
    record: &mut Record,
    doc: &mut toml_edit::DocumentMut,
    config_path: &Path,
    key: &str,
    v: &str,
) {
    let previous = config::set_cap_value(doc, TABLE, key, toml_edit::Value::from(v));
    record.add(
        FEATURE,
        Change::ConfigValue {
            file: config_path.to_path_buf(),
            table: TABLE.into(),
            key: key.into(),
            previous,
        },
    );
}

/// Create the directories down to `dir` that do not exist, owned by the account (a root-owned
/// ~/.config would break the account's own systemd and GNOME settings).
fn create_owned_dirs(dir: &Path, uid: libc::uid_t, gid: libc::gid_t) -> Result<()> {
    let mut missing = Vec::new();
    let mut d = dir;
    while !d.exists() {
        missing.push(d.to_path_buf());
        match d.parent() {
            Some(p) => d = p,
            None => break,
        }
    }
    for m in missing.iter().rev() {
        std::fs::create_dir(m).with_context(|| format!("creating {}", m.display()))?;
        chown(m, Some(uid), Some(gid)).with_context(|| format!("chown {}", m.display()))?;
    }
    Ok(())
}

pub async fn setup(config_path: &Path, state: &Path, args: DesktopArgs) -> Result<i32> {
    crate::require_root("setup desktop")?;
    cliclack::intro("fjarr setup desktop")?;
    if !system::ubuntu_with_apt() {
        print_options()?;
        cliclack::outro_cancel("nothing changed")?;
        return Ok(1);
    }
    if !system::dpkg_installed(PACKAGE) {
        bail!(
            "the desktop package is not installed: sudo apt install {PACKAGE}, then run this again"
        );
    }
    if !Path::new("/etc/gdm3").exists() && !system::unit_exists("gdm.service") {
        bail!(
            "no GDM on this machine: this version sets up GNOME under GDM (backend E, ADR-0006); \
             an X11 kiosk comes with fjarr-desktop-x11"
        );
    }
    let mut doc = config::load(config_path)?;
    let session = crate::detect::display_session();
    log::info(format!(
        "Session: {} → backend: mutter",
        session
            .as_deref()
            .unwrap_or("no graphical session right now (GDM will start one)")
    ))?;

    // 1. The account the desktop runs as.
    let configured = configured_account(&doc);
    let mut choices: Vec<(String, String, String)> = Vec::new();
    if system::passwd_entry(DEFAULT_ACCOUNT).is_none() {
        choices.push((
            DEFAULT_ACCOUNT.into(),
            format!("Create \"{DEFAULT_ACCOUNT}\""),
            "no password, no remote login".into(),
        ));
    }
    for a in system::human_accounts() {
        choices.push((a.clone(), format!("Use existing: {a}"), String::new()));
    }
    let options: Vec<(String, &str, &str)> = choices
        .iter()
        .map(|(v, l, h)| (v.clone(), l.as_str(), h.as_str()))
        .collect();
    let account =
        match args
            .account
            .clone()
            .or_else(|| if args.yes { configured.clone() } else { None })
        {
            Some(a) => a,
            None if args.yes => DEFAULT_ACCOUNT.to_string(),
            None => ui::select(
                "Which account should the desktop run as? It logs in by itself at boot.",
                "--account NAME",
                None,
                &options,
            )?,
        };
    if account == "root" || account == "fjarr" {
        bail!("--account {account}: the desktop must be an ordinary account, not root or the agent's own (docs/10)");
    }
    let mut record = Record::load(state)?;
    if system::passwd_entry(&account).is_none() {
        system::run(
            "useradd",
            &[
                "--create-home",
                "--user-group",
                "--shell",
                "/bin/bash",
                "--comment",
                "Fjarr desktop",
                &account,
            ],
        )?;
        let _ = system::run("passwd", &["--lock", &account]);
        record.add(
            FEATURE,
            Change::AccountCreated {
                user: account.clone(),
            },
        );
        record.save(state)?;
        log::success(format!(
            "Account {account} created (password locked: it only ever logs in automatically)"
        ))?;
    }
    if !system::in_group(&account, GROUP) {
        system::run("usermod", &["-aG", GROUP, &account])?;
        record.add(
            FEATURE,
            Change::GroupMember {
                user: account.clone(),
                group: GROUP.into(),
            },
        );
        record.save(state)?;
    }
    let (home, uid, gid) = system::passwd_entry(&account)
        .with_context(|| format!("{account} is not in the passwd database"))?;

    // 2. GDM logs it in at boot; nothing locks or blanks its screen.
    let gdm = std::fs::read_to_string(GDM_CONF).unwrap_or_default();
    write_recorded(
        &mut record,
        Path::new(GDM_CONF),
        &gdm_autologin(&gdm, &account),
    )?;
    log::success(format!("Auto-login enabled for {account} in GDM"))?;
    write_recorded(&mut record, Path::new(DCONF_DB), dconf_db_text())?;
    if let Some(p) = dconf_profile(std::fs::read_to_string(DCONF_PROFILE).ok().as_deref()) {
        write_recorded(&mut record, Path::new(DCONF_PROFILE), &p)?;
    }
    record.add(FEATURE, Change::DconfUpdate);
    if let Err(e) = system::run("dconf", &["update"]) {
        log::warning(format!(
            "dconf update: {e}; the screen-lock setting applies once dconf is installed"
        ))?;
    }
    record.save(state)?;
    log::success("No screen lock or idle blanking (for every account on this machine: an appliance's desktop)")?;

    // 3. The helper for that account only, and the watchdog.
    let link = helper_wants_link(&home);
    create_owned_dirs(link.parent().expect("a parent"), uid, gid)?;
    if std::fs::symlink_metadata(&link).is_err() {
        symlink(HELPER_UNIT_PATH, &link).with_context(|| format!("linking {}", link.display()))?;
        let _ = std::os::unix::fs::lchown(&link, Some(uid), Some(gid));
        record.add(
            FEATURE,
            Change::Symlink {
                path: link.clone(),
                target: HELPER_UNIT_PATH.into(),
            },
        );
    }
    if system::systemd_running() {
        system::systemctl(&["enable", "--now", WATCHDOG_TIMER])?;
        record.add(
            FEATURE,
            Change::UnitEnabled {
                unit: WATCHDOG_TIMER.into(),
            },
        );
    }
    record.save(state)?;
    log::success(format!(
        "Session helper enabled for {account} · group {GROUP} · GDM watchdog enabled"
    ))?;

    // 3b. Ghost screens (docs/26#ghost-screens): given, or offered when no monitor is plugged in.
    let monitors = crate::display::monitors_connected();
    let ghosts = match args.ghost_screens {
        Some(n) => n,
        None if args.yes || monitors > 0 => 0,
        None => ui::select(
            "No monitor is plugged in. Ghost screens for when none is attached?",
            "--ghost-screens N",
            None,
            &[
                (0u32, "None", ""),
                (1, "1", "change later: fjarr-agent display"),
                (2, "2", ""),
            ],
        )?,
    };
    if ghosts > 0 {
        let added = crate::display::add_ghosts(
            &mut record,
            state,
            ghosts,
            None,
            crate::display::DEFAULT_MODE,
        )?;
        for g in &added {
            log::success(format!(
                "{} → Fjarr Ghost {} ({}) · after a reboot",
                g.connector, g.number, g.mode
            ))?;
        }
    }

    // 4. The agent's side: who the helper runs as, the socket's group.
    set_value(&mut record, &mut doc, config_path, "helper.user", &account);
    set_value(&mut record, &mut doc, config_path, "helper.group", GROUP);
    config::save(config_path, &doc)?;
    record.save(state)?;
    if system::systemd_running() {
        system::restart_agent_if_running()?;
    }

    // 5. The group applies at the account's next login.
    let reboot = ui::select(
        &format!("{account} must log in once for its group to apply. Reboot now?"),
        "--reboot yes|no",
        args.reboot,
        &[
            (YesNo::Yes, "Yes", ""),
            (
                YesNo::No,
                "Later",
                "the desktop is reachable after the next boot",
            ),
        ],
    )?;
    cliclack::outro(format!(
        "fjarr-agent --check follows · undo: fjarr-agent setup --undo {FEATURE}"
    ))?;
    let code = crate::agent_check(config_path)?;
    if reboot == YesNo::Yes && system::systemd_running() {
        system::systemctl(&["reboot"])?;
    }
    Ok(code)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn autologin_goes_into_daemon_and_keeps_every_other_line() {
        let ubuntu = "# GDM configuration storage\n\n[daemon]\n# Uncomment the line below to force the login screen to use Xorg\n#WaylandEnable=false\n\n# Enabling automatic login\n#  AutomaticLoginEnable = true\n#  AutomaticLogin = user1\n\n[security]\n\n[debug]\n";
        let out = gdm_autologin(ubuntu, "desktop");
        assert!(out.contains("[daemon]\n# Uncomment the line below"));
        assert!(out.contains("AutomaticLoginEnable=true\nAutomaticLogin=desktop\n"));
        assert!(
            !out.contains("AutomaticLogin = user1"),
            "the commented example is replaced, not left to confuse"
        );
        assert!(out.contains("#WaylandEnable=false"), "Wayland stays on");
        assert!(out.contains("[security]") && out.contains("[debug]"));
        assert_eq!(
            gdm_autologin(&out, "desktop"),
            out,
            "a rerun changes nothing"
        );
        assert!(gdm_autologin(&out, "kiosk").contains("AutomaticLogin=kiosk\n"));
    }

    #[test]
    fn autologin_makes_a_daemon_section_when_there_is_none() {
        assert_eq!(
            gdm_autologin("", "desktop"),
            "[daemon]\nAutomaticLoginEnable=true\nAutomaticLogin=desktop\n"
        );
        assert_eq!(
            gdm_autologin("[security]\nDisallowTCP=true\n", "d"),
            "[security]\nDisallowTCP=true\n\n[daemon]\nAutomaticLoginEnable=true\nAutomaticLogin=d\n"
        );
    }

    #[test]
    fn the_dconf_profile_gains_the_local_database_once() {
        assert_eq!(
            dconf_profile(None).unwrap(),
            "user-db:user\nsystem-db:local\n"
        );
        assert_eq!(dconf_profile(Some("user-db:user\nsystem-db:local\n")), None);
        assert_eq!(
            dconf_profile(Some("user-db:user\nsystem-db:site")).unwrap(),
            "user-db:user\nsystem-db:site\nsystem-db:local\n"
        );
    }

    #[test]
    fn the_watchdog_restarts_gdm_only_after_two_checks_without_the_session() {
        let theirs =
            r#"[{"session":"c1","uid":120,"user":"gdm","seat":"seat0","class":"greeter"}]"#;
        let ours = r#"[{"session":"2","uid":1001,"user":"desktop","seat":"seat0","class":"user"},{"session":"5","uid":1000,"user":"erik","seat":null}]"#;
        assert_eq!(
            watchdog_decide(ours, "desktop", true, 1),
            WatchdogAction::Healthy
        );
        assert_eq!(
            watchdog_decide(theirs, "desktop", true, 0),
            WatchdogAction::Missing(1)
        );
        assert_eq!(
            watchdog_decide(theirs, "desktop", true, 1),
            WatchdogAction::RestartGdm
        );
        assert_eq!(
            watchdog_decide(theirs, "desktop", false, 5),
            WatchdogAction::Healthy,
            "GDM down is systemd's to restart"
        );
        // A session over ssh (no seat) is not the desktop.
        let ssh = r#"[{"session":"9","uid":1001,"user":"desktop","seat":null}]"#;
        assert_eq!(
            watchdog_decide(ssh, "desktop", true, 0),
            WatchdogAction::Missing(1)
        );
        assert_eq!(
            watchdog_decide("not json", "desktop", true, 0),
            WatchdogAction::Missing(1)
        );
    }

    #[test]
    fn the_helper_is_enabled_for_one_account_under_its_graphical_session() {
        assert_eq!(
            helper_wants_link(Path::new("/home/desktop")),
            PathBuf::from("/home/desktop/.config/systemd/user/graphical-session.target.wants/fjarr-desktop-session.service")
        );
    }

    #[test]
    fn the_helper_keys_are_set_and_undone_without_touching_the_rest() {
        let original = "[agent]\nrobot_id = \"r\"   # keep me\n";
        let mut doc: toml_edit::DocumentMut = original.parse().unwrap();
        assert_eq!(
            config::set_cap_value(&mut doc, TABLE, "helper.user", "desktop".into()),
            None
        );
        config::set_cap_value(&mut doc, TABLE, "helper.group", GROUP.into());
        assert_eq!(configured_account(&doc).as_deref(), Some("desktop"));
        assert!(doc.to_string().contains("[capabilities.\"fjarr.desktop\".helper]\nuser = \"desktop\"\ngroup = \"fjarr-desktop\""), "{doc}");
        config::restore_cap_value(&mut doc, TABLE, "helper.group", None).unwrap();
        config::restore_cap_value(&mut doc, TABLE, "helper.user", None).unwrap();
        assert_eq!(
            doc.to_string(),
            original,
            "undo leaves no empty tables behind"
        );
    }
}
