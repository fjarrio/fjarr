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
/// The desktop's host side, with no agent (docs/26#packages).
pub const PACKAGE_SESSION: &str = "fjarr-desktop-session";
/// On a container host, `/run/fjarr` belongs to the container agent's fixed uid (docs/26).
pub const CONTAINER_TMPFILES: &str = "/etc/tmpfiles.d/fjarr-container.conf";
pub const CONTAINER_AGENT_UID: u32 = 10001;
/// Backend A, an X11 kiosk (docs/23#desktop-x11).
pub const PACKAGE_X11: &str = "fjarr-desktop-x11";
/// Where `setup desktop` links the kiosk session's autostart entry, and what it links to (docs/26).
pub const X11_AUTOSTART: &str = "/etc/xdg/autostart/fjarr-x11-session.desktop";
pub const X11_AUTOSTART_SOURCE: &str = "/usr/share/fjarr/fjarr-x11-session.desktop";

/// An X11 kiosk when asked, or when its package is the only desktop package installed.
pub fn x11_chosen(flag: bool, x11_installed: bool, wayland_installed: bool) -> bool {
    flag || (x11_installed && !wayland_installed)
}
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
    /// A user session holds seat0 (or there is nothing to watch).
    Healthy,
    /// No user session on seat0 this check: the count of consecutive misses so far.
    Missing(u32),
    /// Missed often enough: restart GDM so the automatic login runs again.
    RestartGdm,
}

/// One watchdog check (docs/26#fjarr-agent-setup-desktop, ADR-0006): `sessions` is `loginctl
/// list-sessions --json=short`. It acts only when **no user session** holds seat0, whoever's: the
/// robot's own desktop died, or nobody is logged in. A person at the machine holds the seat and is
/// never logged out by it. GDM's greeter is not a user session. GDM down is systemd's to restart.
pub fn watchdog_decide(sessions: &str, gdm_active: bool, misses: u32) -> WatchdogAction {
    if !gdm_active {
        return WatchdogAction::Healthy;
    }
    let list: Vec<serde_json::Value> = serde_json::from_str(sessions).unwrap_or_default();
    let held = list.iter().any(|s| {
        let on_seat = s.get("seat").and_then(|v| v.as_str()) == Some("seat0");
        let user = s.get("user").and_then(|v| v.as_str()).unwrap_or("");
        // An older loginctl has no class field: then the greeter is known by its account.
        let is_user = match s.get("class").and_then(|v| v.as_str()) {
            Some(class) => class == "user",
            None => !user.starts_with("gdm"),
        };
        on_seat && is_user
    });
    if held {
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
    // The agent's configuration names the account; a container host has none, and GDM's automatic
    // login says the same thing (docs/26#a-desktop-in-a-container).
    let Some(user) = config::load(config_path)
        .ok()
        .as_ref()
        .and_then(configured_account)
        .or_else(|| gdm_autologin_user(&std::fs::read_to_string(GDM_CONF).unwrap_or_default()))
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
    match watchdog_decide(&sessions, gdm_active, misses) {
        WatchdogAction::Healthy => {
            let _ = std::fs::remove_file(WATCHDOG_STATE);
        }
        WatchdogAction::Missing(n) => {
            let _ = std::fs::write(WATCHDOG_STATE, n.to_string());
            println!("desktop watchdog: no user session on seat0 ({n} of {WATCHDOG_MISSES})");
        }
        WatchdogAction::RestartGdm => {
            let _ = std::fs::remove_file(WATCHDOG_STATE);
            println!("desktop watchdog: no user session on seat0 for {WATCHDOG_MISSES} checks; restarting GDM so {user} logs in again");
            system::systemctl(&["restart", "gdm.service"])?;
        }
    }
    Ok(())
}

/// The account GDM logs in automatically, from its configuration's `[daemon]` section.
pub fn gdm_autologin_user(conf: &str) -> Option<String> {
    let mut daemon = false;
    let mut enabled = false;
    let mut user = None;
    for line in conf.lines().map(str::trim) {
        if line.starts_with('[') {
            daemon = line == "[daemon]";
            continue;
        }
        if !daemon || line.starts_with('#') {
            continue;
        }
        match line.split_once('=').map(|(k, v)| (k.trim(), v.trim())) {
            Some(("AutomaticLoginEnable", v)) => enabled = v.eq_ignore_ascii_case("true"),
            Some(("AutomaticLogin", v)) if !v.is_empty() => user = Some(v.to_string()),
            _ => {}
        }
    }
    user.filter(|_| enabled)
}

/// Inside the agent's container: the host's desktop account and group, as numbers (the image has
/// neither by name), into the agent's configuration (docs/26#a-desktop-in-a-container).
fn setup_container_side(config_path: &Path, state: &Path, uid: u32, gid: u32) -> Result<i32> {
    let mut doc = config::load(config_path)?;
    let mut record = Record::load(state)?;
    for (key, v) in [("helper.uid", uid), ("helper.gid", gid)] {
        let previous =
            config::set_cap_value(&mut doc, TABLE, key, toml_edit::Value::from(i64::from(v)));
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
    config::save(config_path, &doc)?;
    record.save(state)?;
    log::success(format!(
        "The agent accepts the host's helper from uid {uid} and gives it its socket with gid {gid}"
    ))?;
    cliclack::outro(format!(
        "Desktop set up for the container · fjarr-agent --check follows · undo: fjarr-agent setup --undo {FEATURE}"
    ))?;
    crate::agent_check(config_path)
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

/// An X11 kiosk (docs/26#fjarr-agent-setup-desktop): the session program's autostart entry, and the
/// agent's backend and display. The display manager and the automatic login stay the robot maker's.
async fn setup_x11(config_path: &Path, state: &Path, args: DesktopArgs) -> Result<i32> {
    if !system::dpkg_installed(PACKAGE_X11) {
        bail!("the X11 desktop package is not installed: sudo apt install {PACKAGE_X11}, then run this again");
    }
    let mut doc = config::load(config_path)?;
    let mut record = Record::load(state)?;
    let session = crate::detect::display_session();
    log::info(format!(
        "Session: {} → backend: X11 (A)",
        session
            .as_deref()
            .unwrap_or("no graphical session right now")
    ))?;

    // 1. The kiosk session runs fjarr-x11-session: the agent's grant and the output layout.
    let link = Path::new(X11_AUTOSTART);
    if std::fs::symlink_metadata(link).is_err() {
        std::fs::create_dir_all(link.parent().expect("a parent"))?;
        symlink(X11_AUTOSTART_SOURCE, link)
            .with_context(|| format!("linking {}", link.display()))?;
        record.add(
            FEATURE,
            Change::Symlink {
                path: link.to_path_buf(),
                target: X11_AUTOSTART_SOURCE.into(),
            },
        );
        record.save(state)?;
    }
    log::success(format!(
        "{X11_AUTOSTART} → the kiosk session grants the agent's account and keeps its monitors laid out"
    ))?;

    // 2. The agent's side: backend A on that display.
    let display = args.display.clone().unwrap_or_else(|| ":0".into());
    set_value(&mut record, &mut doc, config_path, "backend", "x11");
    set_value(&mut record, &mut doc, config_path, "display", &display);
    config::save(config_path, &doc)?;
    record.save(state)?;
    log::success(format!("The agent opens the X display {display}"))?;
    log::remark(
        "The display manager and the automatic login are left as they are: the kiosk session is yours. \
         LightDM with an automatic login is the easy X11 kiosk; under GDM the X session loses the screen \
         to a Wayland greeter about 11 s after an automatic login (ADR-0006).",
    )?;
    if system::systemd_running() {
        system::restart_agent_if_running()?;
    }
    cliclack::outro(format!(
        "The kiosk session grants the agent at its next login (log out and in, or reboot) · fjarr-agent --check follows · undo: fjarr-agent setup --undo {FEATURE}"
    ))?;
    crate::agent_check(config_path)
}

pub async fn setup(config_path: &Path, state: &Path, args: DesktopArgs) -> Result<i32> {
    crate::require_root("setup desktop")?;
    cliclack::intro("fjarr setup desktop")?;
    if let (Some(uid), Some(gid)) = (args.helper_uid, args.helper_gid) {
        return setup_container_side(config_path, state, uid, gid);
    }
    if !system::ubuntu_with_apt() {
        print_options()?;
        cliclack::outro_cancel("nothing changed")?;
        return Ok(1);
    }
    if !args.container
        && x11_chosen(
            args.x11,
            system::dpkg_installed(PACKAGE_X11),
            system::dpkg_installed(PACKAGE),
        )
    {
        return setup_x11(config_path, state, args).await;
    }
    if args.container && !system::dpkg_installed(PACKAGE_SESSION) {
        bail!(
            "the desktop's host package is not installed: sudo apt install {PACKAGE_SESSION}, then run this again"
        );
    }
    if !args.container && !system::dpkg_installed(PACKAGE) {
        bail!(
            "the desktop package is not installed: sudo apt install {PACKAGE} (GNOME) or {PACKAGE_X11} (an X11 kiosk), then run this again"
        );
    }
    if !Path::new("/etc/gdm3").exists() && !system::unit_exists("gdm.service") {
        bail!(
            "no GDM on this machine: this version sets up GNOME under GDM (backend E, ADR-0006); \
             an X11 kiosk comes with fjarr-desktop-x11"
        );
    }
    // A container host has no agent and so no agent configuration: the container keeps its own.
    let mut doc = if args.container {
        None
    } else {
        Some(config::load(config_path)?)
    };
    let session = crate::detect::display_session();
    log::info(format!(
        "Session: {} → backend: mutter",
        session
            .as_deref()
            .unwrap_or("no graphical session right now (GDM will start one)")
    ))?;

    // 1. The account the desktop runs as.
    let configured = doc.as_ref().and_then(configured_account);
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
    record.save(state)?;
    log::success(format!(
        "Session helper enabled for {account} · group {GROUP}"
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

    // 4. The agent's side: who the helper runs as, the socket's group. In a container that is the
    //    container's configuration; the host makes /run/fjarr the container agent's instead.
    if let Some(doc) = doc.as_mut() {
        set_value(&mut record, doc, config_path, "helper.user", &account);
        set_value(&mut record, doc, config_path, "helper.group", GROUP);
        config::save(config_path, doc)?;
    } else {
        write_recorded(
            &mut record,
            Path::new(CONTAINER_TMPFILES),
            &format!(
                "# fjarr-setup setup desktop --container (docs/26#a-desktop-in-a-container): the helper's\n\
                 # socket directory, bind-mounted into the agent's container, belongs to its uid.\n\
                 d /run/fjarr 0755 {CONTAINER_AGENT_UID} {CONTAINER_AGENT_UID} -\n"
            ),
        )?;
        let _ = system::run("systemd-tmpfiles", &["--create", CONTAINER_TMPFILES]);
        log::success(format!(
            "/run/fjarr belongs to the container's agent (uid {CONTAINER_AGENT_UID})"
        ))?;
    }
    record.save(state)?;
    // The watchdog last: its first check must already find the configured account.
    if system::systemd_running() {
        system::systemctl(&["enable", "--now", WATCHDOG_TIMER])?;
        record.add(
            FEATURE,
            Change::UnitEnabled {
                unit: WATCHDOG_TIMER.into(),
            },
        );
        record.save(state)?;
        log::success(
            "GDM watchdog enabled: brings the automatic login back when no one holds the seat",
        )?;
        if !args.container {
            system::restart_agent_if_running()?;
        }
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
    if args.container {
        let gid = system::group_gid(GROUP).with_context(|| format!("no {GROUP} group"))?;
        // Plain lines, not a note box: the box wraps the command, and it is meant to be copied.
        log::info(format!(
            "For the agent's container: volume /run/fjarr:/run/fjarr, group_add \"{gid}\", then"
        ))?;
        log::info(format!(
            "docker compose run --rm fjarr-agent setup desktop --helper-uid {uid} --helper-gid {gid}"
        ))?;
        cliclack::outro(format!(
            "Host side set up: {account} is uid {uid}, {GROUP} is gid {gid} · undo: fjarr-setup setup --undo {FEATURE}"
        ))?;
        if reboot == YesNo::Yes && system::systemd_running() {
            system::systemctl(&["reboot"])?;
        }
        return Ok(0);
    }
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

    /// A container host's watchdog names the account from GDM, as there is no agent configuration.
    #[test]
    fn the_automatic_login_account_is_read_from_gdms_daemon_section() {
        let set = gdm_autologin(
            "# GDM\n[daemon]\n#  AutomaticLogin = user1\n\n[security]\nAutomaticLogin=nobody\n",
            "desktop",
        );
        assert_eq!(gdm_autologin_user(&set).as_deref(), Some("desktop"));
        assert_eq!(
            gdm_autologin_user(
                "[daemon]\n#  AutomaticLoginEnable = true\n#  AutomaticLogin = user1\n"
            ),
            None,
            "comments are not a setting"
        );
        assert_eq!(
            gdm_autologin_user("[daemon]\nAutomaticLoginEnable=false\nAutomaticLogin=desktop\n"),
            None,
            "switched off"
        );
    }

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
    fn an_x11_kiosk_is_chosen_when_asked_or_when_only_its_package_is_installed() {
        assert!(x11_chosen(true, false, true), "--x11 wins");
        assert!(x11_chosen(false, true, false), "only fjarr-desktop-x11");
        assert!(!x11_chosen(false, true, true), "both: GNOME unless --x11");
        assert!(!x11_chosen(false, false, true));
        assert!(
            !x11_chosen(false, false, false),
            "neither: the GNOME path says what to install"
        );
    }

    #[test]
    fn the_watchdog_restarts_gdm_only_when_no_one_has_held_the_seat_for_two_checks() {
        let greeter =
            r#"[{"session":"c1","uid":120,"user":"gdm","seat":"seat0","class":"greeter"}]"#;
        let robot = r#"[{"session":"2","uid":1001,"user":"desktop","seat":"seat0","class":"user"},{"session":"5","uid":1000,"user":"erik","seat":null}]"#;
        assert_eq!(watchdog_decide(robot, true, 1), WatchdogAction::Healthy);
        assert_eq!(
            watchdog_decide(greeter, true, 0),
            WatchdogAction::Missing(1)
        );
        assert_eq!(
            watchdog_decide(greeter, true, 1),
            WatchdogAction::RestartGdm
        );
        assert_eq!(
            watchdog_decide(greeter, false, 5),
            WatchdogAction::Healthy,
            "GDM down is systemd's to restart"
        );
        // The mini-PC, 2026-10-01: a person logged in at the machine holds the seat; never log them out.
        let person = r#"[{"session":"3","uid":1000,"user":"robot","seat":"seat0","class":"user","tty":"tty2"}]"#;
        assert_eq!(watchdog_decide(person, true, 7), WatchdogAction::Healthy);
        // A session over ssh (no seat) holds nothing.
        let ssh = r#"[{"session":"9","uid":1001,"user":"desktop","seat":null,"class":"user"}]"#;
        assert_eq!(watchdog_decide(ssh, true, 0), WatchdogAction::Missing(1));
        // An older loginctl without a class: the greeter is known by its account.
        assert_eq!(
            watchdog_decide(r#"[{"user":"gdm-greeter","seat":"seat0"}]"#, true, 0),
            WatchdogAction::Missing(1)
        );
        assert_eq!(
            watchdog_decide(r#"[{"user":"robot","seat":"seat0"}]"#, true, 0),
            WatchdogAction::Healthy
        );
        assert_eq!(
            watchdog_decide("not json", true, 0),
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
