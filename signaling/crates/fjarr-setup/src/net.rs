//! `net setup` and `net up` (docs/26#fjarr-agent-net-setup, docs/27#lifecycle).
//!
//! The tunnel's one hard rule is that its device exists before any software that should use it
//! starts, because DDS picks its interfaces when a participant is created. So `net up` is what
//! `fjarr-net.service` runs at every boot, ordered before `fjarr-agent.service`, and `net setup`
//! is the one-time part: the address, the config, the unit, and the customer's own units ordered
//! after it when they use ROS.
use std::net::Ipv4Addr;
use std::path::{Path, PathBuf};

use anyhow::{bail, Context, Result};
use cliclack::log;

use crate::changes::{Change, Record};
use crate::{config, system, ui, Dds, NetSetupArgs, YesNo};

pub const FEATURE: &str = "net";
pub const UNIT: &str = "fjarr-net.service";
pub const CYCLONE_FILE: &str = "/etc/fjarr/cyclonedds.xml";
/// Where a replaced file's previous contents are kept for `--undo`.
const BACKUPS: &str = "/var/lib/fjarr/setup-backups";

/// The device's owner: the agent's account, from the profile's `[net] owner` (docs/26#the-system-profile).
fn owner(profile: &Path) -> String {
    std::fs::read_to_string(profile)
        .ok()
        .and_then(|t| t.parse::<toml_edit::DocumentMut>().ok())
        .and_then(|d| d.get("net")?.get("owner")?.as_str().map(str::to_string))
        .unwrap_or_else(|| "fjarr".to_string())
}

/// What `fjarr-net.service` runs at every boot: the device from the configuration, idempotent.
/// Plain lines, for the journal.
pub async fn up(config_path: &Path, profile: &Path) -> Result<()> {
    crate::require_root("net up")?;
    let doc = config::load(config_path)?;
    let net = config::net(&doc)?;
    if !net.enabled {
        println!(
            "net up: fjarr.net is not enabled in {}; nothing to create",
            config_path.display()
        );
        return Ok(());
    }
    let owner = owner(profile);
    let uid = system::uid_of(&owner)?;
    let address = crate::agent_net_address(config_path)?;
    let peer = net.range.operator();
    let provenance =
        fjarr_netdev::ensure_tun(&net.interface, uid, address, Some(peer), net.mtu).await?;
    println!(
        "net up: {} {} — {address} peer {peer}, mtu {}, owner {owner} (uid {uid})",
        net.interface,
        match provenance {
            fjarr_netdev::Provenance::Created => "created",
            fjarr_netdev::Provenance::Existing => "already there",
        },
        net.mtu
    );
    Ok(())
}

pub fn drop_in_text(unit: &str) -> String {
    format!(
        "# Written by `fjarr-agent net setup`: the tunnel device must exist before this service starts,\n\
         # because DDS picks its interfaces when a participant is created (docs/27#lifecycle).\n\
         # Undo: fjarr-agent setup --undo net\n\
         [Unit]\n\
         After={UNIT}\n\
         Wants={UNIT}\n\
         # {unit}\n"
    )
}

/// The Cyclone DDS file docs/27#ros2 measured, with this device's real LAN interface: a name that
/// does not exist would leave Cyclone on the tunnel alone.
pub fn cyclone_xml(tunnel: &str, lan: &str, self_addr: Ipv4Addr, operator: Ipv4Addr) -> String {
    format!(
        r#"<?xml version="1.0" encoding="UTF-8"?>
<!-- Written by `fjarr-agent net setup` (docs/27#ros2). Use it with
     CYCLONEDDS_URI=file://{CYCLONE_FILE} in the environment of the ROS 2 processes. -->
<CycloneDDS xmlns="https://cdds.io/config">
  <Domain id="any">
    <General>
      <AllowMulticast>true</AllowMulticast>
      <Interfaces>
        <!-- The tunnel first, the local network still usable: without explicit priorities Cyclone
             picks one interface and logs that it chose it "arbitrarily". -->
        <NetworkInterface name="{tunnel}" priority="10" multicast="true"/>
        <NetworkInterface name="{lan}" priority="1" multicast="true"/>
      </Interfaces>
    </General>
    <Discovery>
      <ParticipantIndex>auto</ParticipantIndex>
      <Peers>
        <Peer address="{operator}"/>   <!-- the operator, always this address -->
        <Peer address="{self_addr}"/>   <!-- this device's own address -->
      </Peers>
    </Discovery>
  </Domain>
</CycloneDDS>
"#
    )
}

/// Write `path`, keeping what was there for `--undo`, and record it.
fn write_recorded(record: &mut Record, path: &Path, contents: &str) -> Result<()> {
    let backup = match std::fs::read(path) {
        Ok(old) => {
            let name = path
                .to_string_lossy()
                .trim_start_matches('/')
                .replace('/', "%");
            let b = PathBuf::from(BACKUPS).join(name);
            std::fs::create_dir_all(BACKUPS)?;
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
    // The first run's backup is the one that matters (changes.rs), so this is a no-op on a rerun.
    record.add(
        FEATURE,
        Change::File {
            path: path.to_path_buf(),
            backup,
        },
    );
    Ok(())
}

/// What `net setup` would do, for a system it does not change (docs/26#the-setup-tool).
fn print_options(net: &config::NetConfig, owner: &str) -> Result<()> {
    log::warning(
        "This is not Ubuntu with apt, so nothing is changed. To set up the tunnel by hand:",
    )?;
    let peer = net.range.operator();
    cliclack::note(
        "options to set",
        format!(
            "1. In {}: [capabilities.\"fjarr.net\"] enabled = true\n\
             2. At every boot, before the device's own software:\n   \
                ip tuntap add dev {ifc} mode tun user {owner}\n   \
                ip addr add $(fjarr-agent --net-address) peer {peer} dev {ifc}\n   \
                ip link set {ifc} mtu {mtu} up\n   \
                (or enable {UNIT}, which runs `fjarr-setup net up`)\n\
             3. Services that use ROS 2: a drop-in with After={UNIT} and Wants={UNIT}",
            crate::DEFAULT_CONFIG,
            ifc = net.interface,
            mtu = net.mtu
        ),
    )?;
    Ok(())
}

pub async fn setup(
    config_path: &Path,
    state: &Path,
    profile: &Path,
    args: NetSetupArgs,
) -> Result<i32> {
    crate::require_root("net setup")?;
    cliclack::intro("fjarr net setup")?;
    let mut doc = config::load(config_path)?;
    let mut net = config::net(&doc)?;
    let owner = owner(profile);
    let uid = system::uid_of(&owner)?;

    if !system::ubuntu_with_apt() {
        print_options(&net, &owner)?;
        cliclack::outro_cancel("nothing changed")?;
        return Ok(1);
    }

    // 1. The address: pinned by --address, or the agent's own derivation.
    let mut record = Record::load(state)?;
    if let Some(a) = args.address {
        if !net.range.contains(a) {
            bail!(
                "--address {a} is outside the configured range {}",
                net.range
            );
        }
        if a == net.range.operator() {
            bail!("--address {a} is the operator's own address (docs/27#addressing)");
        }
        let prev =
            config::set_net_value(&mut doc, "address", toml_edit::Value::from(a.to_string()));
        record.add(
            FEATURE,
            Change::ConfigValue {
                file: config_path.to_path_buf(),
                key: "address".into(),
                previous: prev,
            },
        );
        config::save(config_path, &doc)?;
        net.address = Some(a);
    }
    let address = crate::agent_net_address(config_path)?;
    let device_id = config::device_id(&doc).unwrap_or_else(|| "(no agent.robot_id)".into());
    log::info(format!(
        "Device id: {device_id} → tunnel address {address} ({})",
        if net.address.is_some() {
            "pinned"
        } else {
            "derived"
        }
    ))?;

    // 2. The range against what already routes on this machine.
    let routes = system::routes_v4().await?;
    let overlaps = system::overlapping(&routes, &net.range, &net.interface);
    if overlaps.is_empty() {
        log::success(format!("Range {} is free on this machine ✔", net.range))?;
    } else {
        let list: Vec<String> = overlaps
            .iter()
            .map(|r| {
                format!(
                    "{}/{} via {}",
                    r.dst,
                    r.prefix,
                    r.oif.as_deref().unwrap_or("?")
                )
            })
            .collect();
        log::warning(format!(
            "Range {} overlaps an existing route: {}. Only two /32s are added, so this usually still works; \
             `range` in capabilities.\"fjarr.net\" changes it (docs/27#addressing)",
            net.range,
            list.join(", ")
        ))?;
        if !ui::confirm("Continue with this range?", "--yes", None, args.yes, true)? {
            cliclack::outro_cancel("nothing changed")?;
            return Ok(1);
        }
    }

    // 3. ROS is an explicit question, never detected (docs/26#fjarr-agent-net-setup).
    let ros = ui::select(
        "Does software on this device use ROS 2 / DDS over the tunnel?",
        "--ros yes|no",
        args.ros,
        &[
            (
                YesNo::Yes,
                "Yes",
                "its services are ordered after the tunnel",
            ),
            (YesNo::No, "No", ""),
        ],
    )?;
    let mut ros_units = Vec::new();
    let mut dds = Dds::None;
    if ros == YesNo::Yes {
        let choices: Vec<(String, &str)> = system::enabled_services()
            .into_iter()
            .map(|u| {
                let hint = if u == "docker.service" {
                    "ROS in containers: they start with the runtime"
                } else {
                    ""
                };
                (u, hint)
            })
            .collect();
        ros_units = ui::multiselect(
            "Which services start it? They must start after the tunnel.",
            "--ros-units a.service,b.service",
            args.ros_units,
            &choices,
        )?;
        for u in &ros_units {
            if !u.contains('.') {
                bail!("--ros-units: {u:?} is not a unit name (bringup.service, docker.service, …)");
            }
            if !system::unit_exists(u) {
                log::warning(format!(
                    "{u} is not a unit systemd knows yet; the ordering applies once it exists"
                ))?;
            }
        }
        dds = ui::select(
            "Which DDS?",
            "--dds cyclone|fastdds|none",
            args.dds,
            &[
                (
                    Dds::Cyclone,
                    "Cyclone DDS",
                    &format!("writes {CYCLONE_FILE} with the LAN interface"),
                ),
                (Dds::Fastdds, "Fast DDS", "no file needed"),
                (Dds::None, "None", ""),
            ],
        )?;
    }

    // 4. The config, the device, the unit — in the order a boot will need them.
    let prev = config::set_net_value(&mut doc, "enabled", toml_edit::Value::from(true));
    record.add(
        FEATURE,
        Change::ConfigValue {
            file: config_path.to_path_buf(),
            key: "enabled".into(),
            previous: prev,
        },
    );
    config::save(config_path, &doc)?;
    system::ensure_state_dir(Path::new("/var/lib/fjarr"), &owner)?;
    record.save(state)?;

    let peer = net.range.operator();
    let provenance =
        fjarr_netdev::ensure_tun(&net.interface, uid, address, Some(peer), net.mtu).await?;
    if provenance == fjarr_netdev::Provenance::Created {
        record.add(
            FEATURE,
            Change::Device {
                name: net.interface.clone(),
            },
        );
        record.save(state)?;
    }
    log::success(format!(
        "{} {} · {address} peer {peer} · mtu {} · owner {owner}",
        net.interface,
        if provenance == fjarr_netdev::Provenance::Created {
            "created"
        } else {
            "already there"
        },
        net.mtu
    ))?;

    let mut drop_ins = 0;
    for u in &ros_units {
        write_recorded(&mut record, &system::drop_in_path(u), &drop_in_text(u))?;
        drop_ins += 1;
    }
    let mut cyclone_note = None;
    if dds == Dds::Cyclone {
        let lan = match args
            .lan_interface
            .clone()
            .or_else(|| system::default_route_interface(&routes))
        {
            Some(l) => l,
            None => bail!("no default route to take the LAN interface from: pass --lan-interface"),
        };
        write_recorded(
            &mut record,
            Path::new(CYCLONE_FILE),
            &cyclone_xml(&net.interface, &lan, address, peer),
        )?;
        cyclone_note = Some(format!(
            "{CYCLONE_FILE} written (LAN interface {lan}). Start the ROS 2 processes with CYCLONEDDS_URI=file://{CYCLONE_FILE}"
        ));
    }
    record.save(state)?;

    system::systemctl(&["daemon-reload"])?;
    system::systemctl(&["enable", UNIT])?;
    record.add(FEATURE, Change::UnitEnabled { unit: UNIT.into() });
    record.save(state)?;
    // The unit is idempotent (`net up` on a device that exists is a no-op), so starting it now makes
    // this boot look like every later one.
    system::systemctl(&["start", UNIT])?;
    // A running agent attaches on restart; a stopped one is left alone.
    system::restart_agent_if_running()?;

    log::success(format!(
        "{UNIT} installed · {drop_ins} ordering drop-in{} · config updated · fjarr-agent restarted if it was running",
        if drop_ins == 1 { "" } else { "s" }
    ))?;
    if let Some(n) = cyclone_note {
        log::info(n)?;
    }
    cliclack::outro(format!(
        "fjarr-agent --check follows · undo: fjarr-agent setup --undo {FEATURE}"
    ))?;
    crate::agent_check(config_path)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_drop_in_orders_after_and_wants_the_tunnel_unit() {
        let t = drop_in_text("bringup.service");
        assert!(
            t.contains("[Unit]\nAfter=fjarr-net.service\nWants=fjarr-net.service\n"),
            "{t}"
        );
        assert!(t.contains("docs/27#lifecycle"));
    }

    #[test]
    fn the_cyclone_file_is_docs_27s_with_the_real_lan_interface() {
        let x = cyclone_xml(
            "fjarr0",
            "wlp3s0",
            "100.70.118.224".parse().unwrap(),
            "100.64.0.1".parse().unwrap(),
        );
        assert!(x.contains(r#"<NetworkInterface name="fjarr0" priority="10" multicast="true"/>"#));
        assert!(x.contains(r#"<NetworkInterface name="wlp3s0" priority="1" multicast="true"/>"#));
        assert!(x.contains(r#"<Peer address="100.64.0.1"/>"#));
        assert!(x.contains(r#"<Peer address="100.70.118.224"/>"#));
        assert!(x.contains("<AllowMulticast>true</AllowMulticast>"));
        assert!(x.contains("<ParticipantIndex>auto</ParticipantIndex>"));
        assert!(
            !x.contains("eth0"),
            "the lab's interface name must not leak"
        );
    }

    #[test]
    fn a_profile_names_the_owner_and_its_absence_means_the_agents_account() {
        let dir = tempfile::tempdir().unwrap();
        let p = dir.path().join("profile.toml");
        std::fs::write(&p, "[net]\nowner = \"robot\"\n").unwrap();
        assert_eq!(owner(&p), "robot");
        assert_eq!(owner(&dir.path().join("none.toml")), "fjarr");
    }
}
