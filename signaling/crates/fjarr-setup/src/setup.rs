//! `fjarr-agent setup`: the first run, which the install script calls
//! (docs/26#fjarr-agent-setup). It detects the machine, asks the few things only the customer
//! knows (server, device id, token, which cameras, which account for the terminal), writes
//! /etc/fjarr/fjarr.toml, starts the agent and waits for its own word that it is online.
//!
//! Every prompt has a flag; every change is recorded under the feature `setup`; on anything but
//! Ubuntu with apt the options are printed instead. Enrollment is M5: until then the device token
//! the server accepts today is what is asked for and written.
use std::path::Path;
use std::time::Duration;

use anyhow::{bail, Context, Result};
use cliclack::log;

use crate::catalog::Catalog;
use crate::changes::{Change, Record, BACKUPS};
use crate::{config, detect, net, system, ui, NetSetupArgs, SetupArgs, YesNo};

pub const FEATURE: &str = "setup";
pub const AGENT_UNIT: &str = "fjarr-agent.service";
const STATE_DIR: &str = "/var/lib/fjarr";
const REACH_TIMEOUT: Duration = Duration::from_secs(5);

/// The agent's account and group, from the profile's `[core] account` (docs/26#the-system-profile).
fn agent_account(profile: &Path) -> String {
    std::fs::read_to_string(profile)
        .ok()
        .and_then(|t| t.parse::<toml_edit::DocumentMut>().ok())
        .and_then(|d| d.get("core")?.get("account")?.as_str().map(str::to_string))
        .unwrap_or_else(|| "fjarr".to_string())
}

/// A track id from a camera's name: `Logitech C920` → `logitech-c920`, unique among `taken`.
pub fn track_id(name: &str, taken: &[String]) -> String {
    let mut slug = String::new();
    let mut dash = false;
    for c in name.chars().flat_map(char::to_lowercase) {
        if c.is_ascii_alphanumeric() {
            slug.push(c);
            dash = false;
        } else if !dash && !slug.is_empty() {
            slug.push('-');
            dash = true;
        }
        if slug.len() >= 32 {
            break;
        }
    }
    let slug = slug.trim_end_matches('-').to_string();
    let base = if slug.is_empty() {
        "camera".to_string()
    } else {
        slug
    };
    if !taken.contains(&base) {
        return base;
    }
    (2..)
        .map(|n| format!("{base}-{n}"))
        .find(|c| !taken.contains(c))
        .expect("an unused suffix")
}

/// What `setup` would write, for a system it does not change (docs/26#the-setup-tool).
fn print_options(config_path: &Path) -> Result<()> {
    log::warning(
        "This is not Ubuntu with apt, so nothing is changed. To set the device up by hand:",
    )?;
    cliclack::note(
        "options to set",
        format!(
            "1. {}: [agent] robot_id, server_url, dev_token (0640 root:{}), and [media] encoder = \"software\" \
             without VA-API; cameras as [capabilities.\"fjarr.camera\".tracks.<id>] (docs/06); a terminal as \
             [capabilities.\"fjarr.terminal\"] enabled = true, user = \"<account>\"\n\
             2. Start and enable fjarr-agent.service (it runs `fjarr-agent --config {}` as {})\n\
             3. Verify: fjarr-agent --config {} --check",
            config_path.display(),
            "fjarr",
            config_path.display(),
            "fjarr",
            config_path.display()
        ),
    )?;
    Ok(())
}

pub async fn setup(
    config_path: &Path,
    state: &Path,
    profile: &Path,
    catalog_path: &Path,
    args: SetupArgs,
) -> Result<i32> {
    if let Some(other) = args.what.as_deref() {
        // `setup desktop` is routed to desktop::setup before this (main.rs).
        bail!("`fjarr-agent setup {other}`: unknown; `setup` alone is the first run, `setup desktop` sets up the desktop");
    }
    crate::require_root("setup")?;
    cliclack::intro("fjarr setup")?;
    let account = agent_account(profile);

    // 1. This device.
    let hw = detect::hardware_encode();
    let display = detect::display_session();
    log::info(format!(
        "This device: {} · {} · {} · {}",
        detect::os_name(),
        detect::deb_arch(),
        match hw {
            Some(true) => "VA-API H.264 ✔",
            Some(false) => "no hardware H.264 encode",
            None => "H.264 encode unknown (fjarr-agent --check did not answer)",
        },
        display.as_deref().unwrap_or("no graphical session")
    ))?;
    if !system::ubuntu_with_apt() {
        print_options(config_path)?;
        cliclack::outro_cancel("nothing changed")?;
        return Ok(1);
    }

    // 2. The existing configuration, when there is one: its values are the defaults, and the
    //    file is edited in place (comments and order kept).
    let existing = match config::load(config_path) {
        Ok(doc) => Some(doc),
        Err(_) if !config_path.exists() => None,
        Err(e) => return Err(e),
    };
    let (had_id, had_server, had_token) = existing
        .as_ref()
        .map(config::agent_values)
        .unwrap_or_default();

    // 3. Where it connects. (Fjarr Cloud joins this prompt in M7; today it is the server URL.)
    let server = ui::input(
        "Server (your fjarr-server's ws:// or wss:// URL)",
        "--server",
        args.server.clone(),
        args.yes,
        had_server.as_deref(),
    )?;
    let (host, port) = system::ws_host_port(&server)?;
    let device_id = ui::input(
        "Device id",
        "--device-id",
        args.device_id.clone(),
        args.yes,
        Some(had_id.as_deref().unwrap_or(&detect::hostname())),
    )?;
    if device_id.trim().is_empty() || device_id.contains(char::is_whitespace) {
        bail!("--device-id {device_id:?}: one word, no spaces");
    }
    let token = match (args.token.clone(), had_token) {
        (Some(t), _) => ui::secret("Device token", "--token (or FJARR_DEVICE_TOKEN)", Some(t))?,
        (None, Some(t)) if args.yes || !ui::is_terminal() => {
            log::step("Device token  kept from the existing configuration")?;
            t
        }
        (None, Some(t)) => {
            if ui::confirm(
                "Keep the device token already in the configuration?",
                "--token",
                None,
                false,
                true,
            )? {
                t
            } else {
                ui::secret(
                    "Device token (from your dashboard)",
                    "--token (or FJARR_DEVICE_TOKEN)",
                    None,
                )?
            }
        }
        (None, None) => ui::secret(
            "Device token (from your dashboard)",
            "--token (or FJARR_DEVICE_TOKEN)",
            None,
        )?,
    };
    if token.is_empty() {
        bail!("the device token is empty");
    }
    // The typo and the closed port are caught before anything is written; a device provisioned
    // where the server is not reachable yet says so with --offline (offline is a normal state,
    // ADR-0019 addendum, but a first run that cannot be checked is not a finished setup).
    if args.offline {
        log::step(format!("Server {server}  not checked (--offline)"))?;
    } else if let Err(e) = system::tcp_reachable(&host, port, REACH_TIMEOUT) {
        bail!(
            "{server} is not reachable from this device ({e:#}). Check the URL and that fjarr-server is running \
             and its port open; or pass --offline to write the configuration now and let the agent connect when it can. \
             Nothing was changed."
        );
    } else {
        log::success(format!("Server {server} reachable ✔"))?;
    }

    // 4. The encoder: the agent has no silent fallback (docs/23), so a device without VA-API has
    //    to say software or the service will not start.
    let encoder = match (args.encoder, hw) {
        (Some(e), _) => {
            log::step(format!("Encoder  {} (--encoder)", e.as_str()))?;
            e.as_str()
        }
        (None, Some(true)) => "auto",
        (None, _) => {
            if ui::confirm(
                "No hardware H.264 encoder was found. Encode in software (CPU)?",
                "--encoder software|vaapi",
                None,
                args.yes,
                true,
            )? {
                "software"
            } else {
                bail!("no encoder chosen: fix /dev/dri and the VA-API driver, then rerun, or pass --encoder software")
            }
        }
    };

    // 5. Cameras: what gst-device-monitor-1.0 sees, matched against the catalog.
    let catalog = Catalog::load(catalog_path)?;
    let mut tracks: Vec<(String, String, toml_edit::InlineTable)> = Vec::new();
    match detect::device_monitor()? {
        None => log::warning(
            "gst-device-monitor-1.0 is not installed (gstreamer1.0-plugins-base-apps): no cameras detected",
        )?,
        Some(devices) if devices.is_empty() => log::info("Cameras: none found")?,
        Some(devices) => {
            let by_id = detect::by_id_map(Path::new("/dev/v4l/by-id"));
            let found = detect::detect_all(&catalog, devices, &by_id);
            let mut items: Vec<(String, String)> = Vec::new();
            let mut initial: Vec<String> = Vec::new();
            for f in &found {
                let Some(name) = &f.stable_name else { continue };
                let entry = f.driver.as_deref().and_then(|n| catalog.get(n));
                let mode = detect::pick_mode(&f.device.modes)
                    .map(|m| {
                        format!(
                            "{} {}×{}@{}",
                            detect::source_format(m),
                            m.width,
                            m.height,
                            m.fps
                        )
                    })
                    .unwrap_or_else(|| "(no mode picked)".into());
                let hint = match entry {
                    Some(e) if e.builtin => format!("{} · {mode}", f.device.name),
                    Some(e) => format!(
                        "{} → needs the {} driver (drivers install {}, M3)",
                        f.device.name, e.name, e.name
                    ),
                    None => format!("{} → no catalog entry", f.device.name),
                };
                if entry.is_some_and(|e| e.builtin && e.source.as_deref() == Some("v4l2")) {
                    initial.push(name.clone());
                }
                items.push((name.clone(), hint));
            }
            let item_refs: Vec<(String, &str)> =
                items.iter().map(|(v, h)| (v.clone(), h.as_str())).collect();
            let given = args.cameras.clone().map(|list| {
                match list.iter().map(String::as_str).collect::<Vec<_>>()[..] {
                    ["all"] => items.iter().map(|(v, _)| v.clone()).collect(),
                    ["none"] => Vec::new(),
                    _ => list,
                }
            });
            let chosen = ui::multiselect(
                "Cameras found. Which should stream?",
                "--cameras all|none|<device,...>",
                given,
                args.yes,
                &initial,
                &item_refs,
            )?;
            let mut taken = existing
                .as_ref()
                .map(config::camera_track_ids)
                .unwrap_or_default();
            for c in chosen {
                let Some(f) = found.iter().find(|f| {
                    f.stable_name.as_deref() == Some(c.as_str())
                        || f.device.path.as_deref() == Some(c.as_str())
                }) else {
                    bail!("--cameras: {c:?} is not a detected device; `fjarr-agent drivers detect` lists them");
                };
                let entry = f.driver.as_deref().and_then(|n| catalog.get(n));
                if !entry.is_some_and(|e| e.builtin && e.source.as_deref() == Some("v4l2")) {
                    log::warning(format!(
                        "{c}: not a built-in v4l2 source; not added (its driver is M3)"
                    ))?;
                    continue;
                }
                let id = track_id(&f.device.name, &taken);
                taken.push(id.clone());
                let mode = detect::pick_mode(&f.device.modes)
                    .map(|m| (detect::source_format(m), m.width, m.height, m.fps));
                let name = f.stable_name.clone().unwrap_or(c.clone());
                tracks.push((id, f.device.name.clone(), config::v4l2_source(&name, mode)));
            }
        }
    }

    // 6. The terminal: which account, and deliberately no default (docs/06#fjarr.terminal).
    let mut accounts: Vec<(String, &str, &str)> = vec![("none".to_string(), "No terminal", "")];
    accounts.push((account.clone(), "", "the agent's own account: works now"));
    let humans = system::human_accounts();
    for h in &humans {
        if *h != account {
            accounts.push((
                h.clone(),
                "",
                "the agent must run as this account for it to work (docs/06)",
            ));
        }
    }
    let terminal = match args.terminal.clone() {
        Some(t) => {
            log::step(format!("Terminal  {t}"))?;
            t
        }
        None if args.yes => {
            log::step("Terminal  none (--yes; --terminal <account> to enable)")?;
            "none".to_string()
        }
        None => {
            let labelled: Vec<(String, &str, &str)> = accounts
                .iter()
                .map(|(v, l, h)| (v.clone(), if l.is_empty() { v.as_str() } else { l }, *h))
                .collect();
            ui::select(
                "Terminal: as which account?",
                "--terminal none|<account>",
                None,
                &labelled,
            )?
        }
    };
    if terminal != "none" {
        system::uid_of(&terminal).with_context(|| format!("--terminal {terminal}"))?;
        if terminal != account {
            log::warning(format!(
                "the terminal reports unavailable until fjarr-agent runs as {terminal}: the agent verifies the account, \
                 it cannot switch to it (docs/06#fjarr.terminal)"
            ))?;
        }
    }

    // 7. The tunnel next?
    let net_next = ui::select(
        "Also set up the network tunnel (net setup) next?",
        "--net yes|no",
        args.net,
        &[
            (YesNo::No, "No", "fjarr-agent net setup runs it any time"),
            (YesNo::Yes, "Yes", "docs/27"),
        ],
    )?;

    // 8. Write the configuration: the one file, recorded whole (its previous contents kept).
    let mut doc = existing.unwrap_or_else(config::new_document);
    config::set_agent(&mut doc, &device_id, &server, &token);
    config::set_encoder(&mut doc, encoder);
    let n_tracks = tracks.len();
    for (id, label, source) in tracks {
        config::set_camera_track(&mut doc, &id, &label, source);
    }
    if terminal != "none" {
        // The agent's own account is a system account: its login shell (nologin) is no terminal.
        let shell = system::login_shell(&terminal)
            .filter(|s| system::refuses_logins(s))
            .map(|_| {
                if Path::new("/bin/bash").exists() {
                    "/bin/bash"
                } else {
                    "/bin/sh"
                }
            });
        config::set_terminal(&mut doc, &terminal, shell);
    }
    let mut record = Record::load(state)?;
    system::ensure_state_dir(Path::new(STATE_DIR), &account)?;
    record.write_file(FEATURE, config_path, &doc.to_string(), Path::new(BACKUPS))?;
    record.save(state)?;
    match system::set_owner_mode(config_path, "root", &account, 0o640) {
        Ok(()) => {}
        Err(e) => log::warning(format!(
            "{}: could not set 0640 root:{account} ({e:#})",
            config_path.display()
        ))?,
    }
    log::success(format!(
        "{} written (0640, root:{account}) · {n_tracks} camera track{} · terminal: {terminal} · encoder: {encoder}",
        config_path.display(),
        if n_tracks == 1 { "" } else { "s" }
    ))?;

    // 9. The agent: enabled by the package, started here, online is its own word.
    if system::systemd_running() {
        if !system::unit_enabled(AGENT_UNIT) {
            system::systemctl(&["enable", AGENT_UNIT])?;
            record.add(
                FEATURE,
                Change::UnitEnabled {
                    unit: AGENT_UNIT.into(),
                },
            );
        }
        let was_active = system::unit_active(AGENT_UNIT);
        // An earlier failed start (a wrong token) leaves a start-rate limit; this is a new start.
        let _ = system::systemctl(&["reset-failed", AGENT_UNIT]);
        system::systemctl(&["restart", AGENT_UNIT])?;
        if !was_active {
            record.add(
                FEATURE,
                Change::UnitStarted {
                    unit: AGENT_UNIT.into(),
                },
            );
        }
        record.save(state)?;
        match system::wait_for_agent_online(Duration::from_secs(args.timeout)) {
            Ok(()) => log::success(format!(
                "fjarr-agent {} · online ✔",
                if was_active { "restarted" } else { "started" }
            ))?,
            Err(e) => {
                cliclack::outro_cancel("undo: fjarr-agent setup --undo")?;
                return Err(e);
            }
        }
    } else {
        log::warning(format!(
            "systemd is not running here (a container?): fjarr-agent was not started. Run it yourself: \
             fjarr-agent --config {}",
            config_path.display()
        ))?;
    }

    if net_next == YesNo::Yes {
        cliclack::outro("net setup follows · undo: fjarr-agent setup --undo")?;
        return net::setup(
            config_path,
            state,
            profile,
            NetSetupArgs {
                yes: args.yes,
                ros: args.ros,
                ros_units: args.ros_units,
                dds: args.dds,
                address: None,
                lan_interface: None,
            },
        )
        .await;
    }
    cliclack::outro("fjarr-agent --check follows · undo: fjarr-agent setup --undo")?;
    crate::agent_check(config_path)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_track_id_is_a_slug_of_the_cameras_name_unique_among_the_existing_tracks() {
        assert_eq!(track_id("Logitech C920", &[]), "logitech-c920");
        assert_eq!(
            track_id("Integrated Camera: Integrated C", &[]),
            "integrated-camera-integrated-c"
        );
        assert_eq!(
            track_id("Logitech C920", &["logitech-c920".into()]),
            "logitech-c920-2"
        );
        assert_eq!(
            track_id(
                "Logitech C920",
                &["logitech-c920".into(), "logitech-c920-2".into()]
            ),
            "logitech-c920-3"
        );
        assert_eq!(track_id("!!!", &[]), "camera");
        assert_eq!(track_id("  ", &["camera".into()]), "camera-2");
        assert!(track_id(&"x".repeat(100), &[]).len() <= 32);
    }

    #[test]
    fn the_profile_names_the_agents_account_and_its_absence_means_fjarr() {
        let dir = tempfile::tempdir().unwrap();
        let p = dir.path().join("profile.toml");
        std::fs::write(&p, "[core]\naccount = \"robot\"\n").unwrap();
        assert_eq!(agent_account(&p), "robot");
        assert_eq!(agent_account(&dir.path().join("none.toml")), "fjarr");
    }
}
