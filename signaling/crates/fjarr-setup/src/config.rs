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
            "{} does not exist: `fjarr-agent setup` writes it (until it exists in this version, copy \
             /usr/share/fjarr/fjarr.toml.example there and set agent.robot_id and agent.server_url)",
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
    fn a_missing_config_names_setup_as_the_fix() {
        let e = load(Path::new("/nonexistent/fjarr.toml"))
            .unwrap_err()
            .to_string();
        assert!(e.contains("fjarr-agent setup"), "{e}");
    }
}
