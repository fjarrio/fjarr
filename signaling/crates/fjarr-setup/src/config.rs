//! `/etc/fjarr/fjarr.toml`, edited in place with comments and order kept: it is the customer's
//! file, and `setup` only ever touches the keys it names (docs/26#the-setup-tool).
//!
//! The `fjarr.net` keys and their defaults mirror the agent's own
//! (`agent/src/capabilities/net_capability.cpp`, docs/27#configuration).
use std::net::Ipv4Addr;
use std::path::Path;

use anyhow::{bail, Context, Result};
use toml_edit::{value, DocumentMut, Item, Table, Value};

use crate::addressing::Range;

pub const NET_TABLE: &str = "fjarr.net";
/// One SCTP chunk (ADR-0027).
pub const DEFAULT_MTU: usize = 1184;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct NetConfig {
    pub enabled: bool,
    pub interface: String,
    pub range: Range,
    /// `None` means `"auto"`: derived from the device id by the agent.
    pub address: Option<Ipv4Addr>,
    pub mtu: usize,
}

pub fn load(path: &Path) -> Result<DocumentMut> {
    let text = match std::fs::read_to_string(path) {
        Ok(t) => t,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => bail!(
            "{} does not exist: `sudo fjarr-agent setup` writes it",
            path.display()
        ),
        Err(e) => return Err(e).with_context(|| format!("reading {}", path.display())),
    };
    text.parse::<DocumentMut>()
        .with_context(|| format!("parsing {}", path.display()))
}

pub fn save(path: &Path, doc: &DocumentMut) -> Result<()> {
    std::fs::write(path, doc.to_string()).with_context(|| format!("writing {}", path.display()))
}

/// A fresh /etc/fjarr/fjarr.toml, as `setup` writes it on a device that has none: plain, with the
/// keys it asked for, and the viewer where the package put it (docs/24#the-viewer).
pub fn new_document() -> DocumentMut {
    "# Written by `fjarr-agent setup`. Edit freely: setup only touches the keys it asks about, and\n\
     # keeps your comments and order. Reference: docs/23 (agent, media), docs/06 (per capability).\n\
     \n\
     [agent]\n\
     \n\
     [introspect]\n\
     viewer_dir = \"/usr/share/fjarr/viewer\"   # installed by the fjarr-agent package (docs/24#the-viewer)\n"
        .parse()
        .expect("the template parses")
}

/// The agent's identity and server: the keys `agent/src/core/config.cpp` reads. Until M5's
/// enrollment the device token is written here (docs/26#fjarr-agent-setup).
pub fn set_agent(doc: &mut DocumentMut, robot_id: &str, server_url: &str, dev_token: &str) {
    let agent = table_at(doc, &["agent"]);
    replace_value(agent, "robot_id", Value::from(robot_id));
    replace_value(agent, "server_url", Value::from(server_url));
    replace_value(agent, "dev_token", Value::from(dev_token));
}

pub fn agent_values(doc: &DocumentMut) -> (Option<String>, Option<String>, Option<String>) {
    let get = |k: &str| doc.get("agent")?.get(k)?.as_str().map(str::to_string);
    (get("robot_id"), get("server_url"), get("dev_token"))
}

/// `media.encoder` (docs/23): the agent has no silent fallback, so a device without VA-API must
/// say `software` or the service will not start.
pub fn set_encoder(doc: &mut DocumentMut, encoder: &str) {
    let media = table_at(doc, &["media"]);
    replace_value(media, "encoder", Value::from(encoder));
}

#[cfg(test)]
pub fn encoder(doc: &DocumentMut) -> Option<String> {
    doc.get("media")?
        .get("encoder")?
        .as_str()
        .map(str::to_string)
}

/// One camera track in docs/06's format:
/// `[capabilities."fjarr.camera".tracks.<id>]` with `label` and an inline `source`.
pub fn set_camera_track(
    doc: &mut DocumentMut,
    id: &str,
    label: &str,
    source: toml_edit::InlineTable,
) {
    let track = table_at(doc, &["capabilities", "fjarr.camera", "tracks", id]);
    replace_value(track, "label", Value::from(label));
    replace_value(track, "source", Value::InlineTable(source));
}

/// The v4l2 source (docs/06): the by-id name, and the mode when one was picked.
pub fn v4l2_source(device: &str, mode: Option<(&str, u32, u32, u32)>) -> toml_edit::InlineTable {
    let mut t = toml_edit::InlineTable::new();
    t.insert("type", Value::from("v4l2"));
    t.insert("device", Value::from(device));
    if let Some((format, w, h, fps)) = mode {
        t.insert("format", Value::from(format));
        t.insert("width", Value::from(w as i64));
        t.insert("height", Value::from(h as i64));
        t.insert("fps", Value::from(fps as i64));
    }
    t
}

pub fn camera_track_ids(doc: &DocumentMut) -> Vec<String> {
    doc.get("capabilities")
        .and_then(|c| c.get("fjarr.camera"))
        .and_then(|c| c.get("tracks"))
        .and_then(Item::as_table)
        .map(|t| t.iter().map(|(k, _)| k.to_string()).collect())
        .unwrap_or_default()
}

/// `[capabilities."fjarr.terminal"]`: on, as `user` (docs/06: there is deliberately no default).
pub fn set_terminal(doc: &mut DocumentMut, user: &str) {
    let term = table_at(doc, &["capabilities", "fjarr.terminal"]);
    replace_value(term, "enabled", Value::from(true));
    replace_value(term, "user", Value::from(user));
}

/// The table at `path`, created on the way as header tables (implicit parents, so the file reads
/// `[capabilities."fjarr.camera".tracks.front]` and not a trail of empty headers).
fn table_at<'a>(doc: &'a mut DocumentMut, path: &[&str]) -> &'a mut Item {
    let mut item: &mut Item = doc.as_item_mut();
    for (i, key) in path.iter().enumerate() {
        let t = item.as_table_mut().expect("a table on the path");
        let child = t.entry(key).or_insert(Item::Table(Table::new()));
        if i + 1 < path.len() {
            if let Some(ct) = child.as_table_mut() {
                if ct.is_empty() {
                    ct.set_implicit(true);
                }
            }
        }
        item = child;
    }
    item
}

pub fn device_id(doc: &DocumentMut) -> Option<String> {
    doc.get("agent")?
        .get("robot_id")?
        .as_str()
        .map(str::to_string)
}

pub fn net(doc: &DocumentMut) -> Result<NetConfig> {
    let t = doc.get("capabilities").and_then(|c| c.get(NET_TABLE));
    let get = |k: &str| t.and_then(|t| t.get(k));
    let range_s = get("range")
        .and_then(Item::as_str)
        .unwrap_or("100.64.0.0/10");
    let range =
        Range::parse(range_s).with_context(|| format!("capabilities.\"{NET_TABLE}\".range"))?;
    let address = match get("address").and_then(Item::as_str).unwrap_or("auto") {
        "auto" => None,
        a => {
            let a: Ipv4Addr = a.parse().with_context(|| {
                format!(
                    "capabilities.\"{NET_TABLE}\".address is neither \"auto\" nor an IPv4 address"
                )
            })?;
            if !range.contains(a) {
                bail!("capabilities.\"{NET_TABLE}\".address {a} is outside range {range}");
            }
            Some(a)
        }
    };
    let mtu = get("mtu")
        .and_then(Item::as_integer)
        .map(|m| m as usize)
        .unwrap_or(DEFAULT_MTU);
    Ok(NetConfig {
        enabled: get("enabled").and_then(Item::as_bool).unwrap_or(false),
        interface: get("interface")
            .and_then(Item::as_str)
            .unwrap_or("fjarr0")
            .to_string(),
        range,
        address,
        mtu,
    })
}

/// Set `capabilities."fjarr.net".<key>`, returning what was there (as TOML text) so the change can
/// be recorded and reversed exactly.
pub fn set_net_value(doc: &mut DocumentMut, key: &str, new: Value) -> Option<String> {
    let caps = doc
        .entry("capabilities")
        .or_insert(Item::Table(Table::new()));
    if let Some(t) = caps.as_table_mut() {
        // A header-less parent: `[capabilities."fjarr.net"]` reads better than an empty `[capabilities]`.
        if t.is_empty() {
            t.set_implicit(true);
        }
    }
    let net = caps
        .as_table_mut()
        .expect("capabilities is a table")
        .entry(NET_TABLE)
        .or_insert(Item::Table(Table::new()));
    let previous = net.get(key).and_then(Item::as_value).map(bare);
    replace_value(net, key, new);
    previous
}

/// The value as TOML text without its decoration: what is recorded, and what parses back.
fn bare(v: &Value) -> String {
    let mut v = v.clone();
    v.decor_mut().clear();
    v.to_string()
}

/// Set `key` to `new`, keeping the spacing and trailing comment the customer put on the old value.
fn replace_value(table: &mut Item, key: &str, mut new: Value) {
    if let Some(old) = table.get(key).and_then(Item::as_value) {
        *new.decor_mut() = old.decor().clone();
    }
    table[key] = value(new);
}

/// Put `capabilities."fjarr.net".<key>` back: to `previous`, or gone when it did not exist, pruning
/// the tables `set_net_value` created when they are left empty.
pub fn restore_net_value(doc: &mut DocumentMut, key: &str, previous: Option<&str>) -> Result<()> {
    let Some(net) = doc
        .get_mut("capabilities")
        .and_then(|c| c.get_mut(NET_TABLE))
        .and_then(Item::as_table_mut)
    else {
        return Ok(());
    };
    match previous {
        Some(text) => {
            let v: Value = text
                .parse()
                .with_context(|| format!("recorded value {text:?} for {key}"))?;
            let mut item = Item::Table(std::mem::take(net));
            replace_value(&mut item, key, v);
            *net = item.into_table().expect("still a table");
        }
        None => {
            net.remove(key);
        }
    }
    let net_empty = net.is_empty();
    if net_empty {
        if let Some(caps) = doc.get_mut("capabilities").and_then(Item::as_table_mut) {
            caps.remove(NET_TABLE);
            if caps.is_empty() {
                doc.remove("capabilities");
            }
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    const EXAMPLE: &str = r#"# Example /etc/fjarr/fjarr.toml.
[agent]
robot_id        = "robot-001"   # the device id
server_url      = "wss://signaling.example.com/ws"

[introspect]
viewer_dir = "/usr/share/fjarr/viewer"
"#;

    #[test]
    fn defaults_match_the_agents_when_the_table_is_absent() {
        let doc: DocumentMut = EXAMPLE.parse().unwrap();
        let n = net(&doc).unwrap();
        assert_eq!(
            n,
            NetConfig {
                enabled: false,
                interface: "fjarr0".into(),
                range: Range::parse("100.64.0.0/10").unwrap(),
                address: None,
                mtu: 1184
            }
        );
        assert_eq!(device_id(&doc).as_deref(), Some("robot-001"));
    }

    #[test]
    fn enabling_keeps_the_customers_comments_and_order() {
        let mut doc: DocumentMut = EXAMPLE.parse().unwrap();
        assert_eq!(set_net_value(&mut doc, "enabled", Value::from(true)), None);
        let out = doc.to_string();
        assert!(out.starts_with("# Example /etc/fjarr/fjarr.toml.\n[agent]\nrobot_id        = \"robot-001\"   # the device id\n"), "{out}");
        assert!(
            out.contains("[capabilities.\"fjarr.net\"]\nenabled = true\n"),
            "{out}"
        );
        assert!(
            !out.contains("[capabilities]\n"),
            "no empty parent header: {out}"
        );
        assert!(net(&doc).unwrap().enabled);
    }

    #[test]
    fn a_pinned_address_is_read_back_and_validated_against_the_range() {
        let mut doc: DocumentMut = EXAMPLE.parse().unwrap();
        set_net_value(&mut doc, "address", Value::from("100.70.1.2"));
        assert_eq!(
            net(&doc).unwrap().address,
            Some(Ipv4Addr::new(100, 70, 1, 2))
        );
        set_net_value(&mut doc, "address", Value::from("10.0.0.1"));
        assert!(net(&doc).unwrap_err().to_string().contains("outside range"));
    }

    #[test]
    fn restoring_puts_the_previous_value_back_or_removes_the_key_and_prunes() {
        let mut doc: DocumentMut =
            format!("{EXAMPLE}\n[capabilities.\"fjarr.net\"]\nenabled = false # off\nmtu = 1280\n")
                .parse()
                .unwrap();
        let prev = set_net_value(&mut doc, "enabled", Value::from(true));
        assert_eq!(prev.as_deref(), Some("false"));
        assert!(
            doc.to_string().contains("enabled = true # off"),
            "the customer's comment survives the change: {doc}"
        );
        restore_net_value(&mut doc, "enabled", prev.as_deref()).unwrap();
        assert!(!net(&doc).unwrap().enabled);
        assert!(
            doc.to_string().contains("enabled = false # off"),
            "and the undo: {doc}"
        );
        assert!(
            doc.to_string().contains("mtu = 1280"),
            "other keys untouched"
        );

        let mut doc: DocumentMut = EXAMPLE.parse().unwrap();
        let prev = set_net_value(&mut doc, "enabled", Value::from(true));
        restore_net_value(&mut doc, "enabled", prev.as_deref()).unwrap();
        assert_eq!(doc.to_string(), EXAMPLE, "a created table is pruned again");
    }

    #[test]
    fn a_new_file_carries_the_agent_keys_the_encoder_a_track_and_the_terminal_in_docs_06s_shape() {
        let mut doc = new_document();
        set_agent(&mut doc, "dev-024", "wss://fleet.acme.com/ws", "t0k3n");
        set_encoder(&mut doc, "software");
        set_camera_track(
            &mut doc,
            "front",
            "Logitech C920",
            v4l2_source("usb-046d_C920-video-index0", Some(("mjpeg", 1280, 720, 30))),
        );
        set_camera_track(&mut doc, "rear", "Rear", v4l2_source("/dev/video2", None));
        set_terminal(&mut doc, "operator");
        let out = doc.to_string();
        assert!(out.contains("[agent]\nrobot_id = \"dev-024\"\nserver_url = \"wss://fleet.acme.com/ws\"\ndev_token = \"t0k3n\"\n"), "{out}");
        assert!(out.contains("[media]\nencoder = \"software\"\n"), "{out}");
        assert!(
            out.contains("[capabilities.\"fjarr.camera\".tracks.front]\nlabel = \"Logitech C920\"\nsource = { type = \"v4l2\", device = \"usb-046d_C920-video-index0\", format = \"mjpeg\", width = 1280, height = 720, fps = 30 }\n"),
            "{out}"
        );
        assert!(out.contains("[capabilities.\"fjarr.camera\".tracks.rear]\nlabel = \"Rear\"\nsource = { type = \"v4l2\", device = \"/dev/video2\" }\n"), "{out}");
        assert!(
            out.contains(
                "[capabilities.\"fjarr.terminal\"]\nenabled = true\nuser = \"operator\"\n"
            ),
            "{out}"
        );
        assert!(
            !out.contains("[capabilities]\n") && !out.contains("[capabilities.\"fjarr.camera\"]\n"),
            "no empty headers: {out}"
        );
        assert!(out.contains("viewer_dir = \"/usr/share/fjarr/viewer\""));
        assert_eq!(camera_track_ids(&doc), vec!["front", "rear"]);
        assert_eq!(
            agent_values(&doc),
            (
                Some("dev-024".into()),
                Some("wss://fleet.acme.com/ws".into()),
                Some("t0k3n".into())
            )
        );
        assert_eq!(encoder(&doc).as_deref(), Some("software"));
        // What the agent parses: the same document, back through the parser.
        let again: DocumentMut = out.parse().unwrap();
        assert_eq!(again.to_string(), out);
    }

    #[test]
    fn an_existing_file_is_edited_in_place_keeping_comments_order_and_the_customers_other_keys() {
        let mut doc: DocumentMut = format!(
            "{EXAMPLE}\n[capabilities.\"fjarr.camera\".tracks.arm]\nlabel = \"Arm\"   # keep me\nsource = \"videotestsrc\"\n"
        )
        .parse()
        .unwrap();
        set_agent(&mut doc, "spike", "ws://192.168.10.188:8080/ws", "tok");
        set_camera_track(
            &mut doc,
            "front",
            "Front",
            v4l2_source("usb-x-video-index0", None),
        );
        let out = doc.to_string();
        assert!(out.starts_with("# Example /etc/fjarr/fjarr.toml.\n[agent]\nrobot_id        = \"spike\"   # the device id\nserver_url      = \"ws://192.168.10.188:8080/ws\"\ndev_token = \"tok\"\n"), "{out}");
        assert!(out.contains("label = \"Arm\"   # keep me\n"), "{out}");
        assert!(
            out.contains("[capabilities.\"fjarr.camera\".tracks.front]\n"),
            "{out}"
        );
        assert_eq!(camera_track_ids(&doc), vec!["arm", "front"]);
        assert!(out.contains("[introspect]\nviewer_dir"), "{out}");
    }

    #[test]
    fn a_missing_config_names_setup_as_the_fix() {
        let e = load(Path::new("/nonexistent/fjarr.toml"))
            .unwrap_err()
            .to_string();
        assert!(e.contains("fjarr-agent setup"), "{e}");
    }
}
