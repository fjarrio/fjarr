//! The operator's own configuration and credential, on the operator's machine (docs/27#configuration).
//!
//! `~/.config/fjarr/config.toml` names the customer's backend (where the operator API is mounted)
//! or, for an integrator who would rather add nothing, a command that prints a grant. The
//! credential `login` obtains lives beside it at mode 0600 and is never anything but a bearer
//! token the customer's backend minted — Fjarr does not read it, refresh it or know its lifetime.
//!
//! spec: docs/27-network-tunnel.md#configuration · docs/27-network-tunnel.md#logging-in
use std::path::{Path, PathBuf};

use anyhow::{Context, Result};
use serde::Deserialize;

#[derive(Debug, Default, Deserialize)]
pub struct Config {
    #[serde(default)]
    pub backend: Backend,
    #[serde(default)]
    pub net: Net,
}

#[derive(Debug, Default, Deserialize)]
pub struct Backend {
    /// Where the customer's operator API is mounted, e.g. `https://fleet.acme.com/api`.
    pub url: Option<String>,
    /// The escape hatch: a command that prints a grant for `{robot}` on stdout.
    pub grant_command: Option<String>,
    /// The dashboard route `login` hands off to; defaults to `<url without /api>/cli-login`.
    pub login_url: Option<String>,
}

#[derive(Debug, Deserialize)]
pub struct Net {
    #[serde(default = "default_interface")]
    pub interface: String,
    /// Must match the robots' (docs/27#configuration); `100.64.0.0/10` when unset.
    pub range: Option<String>,
}

impl Default for Net {
    fn default() -> Self {
        Self {
            interface: default_interface(),
            range: None,
        }
    }
}

impl Net {
    pub fn range(&self) -> Result<crate::addressing::Range> {
        match &self.range {
            Some(r) => crate::addressing::Range::parse(r).context("[net] range in config.toml"),
            None => Ok(crate::addressing::default_range()),
        }
    }
}

fn default_interface() -> String {
    "fjarr0".to_string()
}

/// `~/.config/fjarr`, or `$FJARR_CONFIG_DIR` for tests and for a lab that has no home to speak of.
pub fn dir() -> Result<PathBuf> {
    if let Ok(d) = std::env::var("FJARR_CONFIG_DIR") {
        return Ok(PathBuf::from(d));
    }
    let base = dirs::config_dir().context("no configuration directory for this user")?;
    Ok(base.join("fjarr"))
}

pub fn load() -> Result<Config> {
    let path = dir()?.join("config.toml");
    match std::fs::read_to_string(&path) {
        Ok(text) => toml::from_str(&text).with_context(|| format!("reading {}", path.display())),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(Config::default()),
        Err(e) => Err(e).with_context(|| format!("reading {}", path.display())),
    }
}

/// Write `[backend] url` (and `login_url`) after a successful login, so the next `list` needs no flag.
pub fn remember_backend(url: &str, login_url: &str) -> Result<()> {
    let d = dir()?;
    std::fs::create_dir_all(&d).with_context(|| format!("creating {}", d.display()))?;
    let path = d.join("config.toml");
    let mut existing: toml::Table = match std::fs::read_to_string(&path) {
        Ok(text) => text.parse().unwrap_or_default(),
        Err(_) => toml::Table::new(),
    };
    let backend = existing
        .entry("backend")
        .or_insert_with(|| toml::Value::Table(toml::Table::new()));
    if let toml::Value::Table(t) = backend {
        t.insert("url".into(), toml::Value::String(url.into()));
        t.insert("login_url".into(), toml::Value::String(login_url.into()));
    }
    std::fs::write(&path, toml::to_string_pretty(&existing)?)
        .with_context(|| format!("writing {}", path.display()))
}

fn credential_path() -> Result<PathBuf> {
    Ok(dir()?.join("credential"))
}

/// The operator credential, or `None` when nobody has logged in on this machine.
pub fn credential() -> Result<Option<String>> {
    let path = credential_path()?;
    match std::fs::read_to_string(&path) {
        Ok(s) => Ok(Some(s.trim().to_string()).filter(|s| !s.is_empty())),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(None),
        Err(e) => Err(e).with_context(|| format!("reading {}", path.display())),
    }
}

/// Store the credential at mode 0600. The directory is 0700 too: a credential for the fleet is
/// not something another local user should be able to list, let alone read (docs/10).
pub fn store_credential(credential: &str) -> Result<PathBuf> {
    let d = dir()?;
    std::fs::create_dir_all(&d).with_context(|| format!("creating {}", d.display()))?;
    restrict(&d, 0o700)?;
    let path = credential_path()?;
    std::fs::write(&path, format!("{credential}\n"))
        .with_context(|| format!("writing {}", path.display()))?;
    restrict(&path, 0o600)?;
    Ok(path)
}

pub fn forget_credential() -> Result<()> {
    let path = credential_path()?;
    match std::fs::remove_file(&path) {
        Ok(()) => Ok(()),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(()),
        Err(e) => Err(e).with_context(|| format!("removing {}", path.display())),
    }
}

#[cfg(unix)]
fn restrict(path: &Path, mode: u32) -> Result<()> {
    use std::os::unix::fs::PermissionsExt;
    std::fs::set_permissions(path, std::fs::Permissions::from_mode(mode))
        .with_context(|| format!("chmod {:o} {}", mode, path.display()))
}

#[cfg(not(unix))]
fn restrict(_path: &Path, _mode: u32) -> Result<()> {
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn scratch() -> tempdir::Dir {
        tempdir::Dir::new()
    }

    /// A tiny temp dir without a dependency: unique per test, removed on drop.
    mod tempdir {
        pub struct Dir(pub std::path::PathBuf);
        impl Dir {
            pub fn new() -> Self {
                let p = std::env::temp_dir().join(format!(
                    "fjarr-connect-test-{}-{}",
                    std::process::id(),
                    rand_suffix()
                ));
                std::fs::create_dir_all(&p).unwrap();
                Self(p)
            }
        }
        impl Drop for Dir {
            fn drop(&mut self) {
                let _ = std::fs::remove_dir_all(&self.0);
            }
        }
        fn rand_suffix() -> u128 {
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        }
    }

    // The tests share the process environment, so they take turns.
    static ENV: std::sync::Mutex<()> = std::sync::Mutex::new(());

    #[test]
    fn the_credential_is_stored_unreadable_to_anyone_else() {
        let _g = ENV.lock().unwrap();
        let d = scratch();
        std::env::set_var("FJARR_CONFIG_DIR", &d.0);
        assert_eq!(credential().unwrap(), None);
        let path = store_credential("secret-token").unwrap();
        assert_eq!(credential().unwrap().as_deref(), Some("secret-token"));
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            assert_eq!(
                std::fs::metadata(&path).unwrap().permissions().mode() & 0o777,
                0o600
            );
            assert_eq!(
                std::fs::metadata(&d.0).unwrap().permissions().mode() & 0o777,
                0o700
            );
        }
        forget_credential().unwrap();
        assert_eq!(credential().unwrap(), None);
        forget_credential().unwrap(); // idempotent
    }

    #[test]
    fn login_remembers_the_backend_without_clobbering_the_rest() {
        let _g = ENV.lock().unwrap();
        let d = scratch();
        std::env::set_var("FJARR_CONFIG_DIR", &d.0);
        std::fs::write(
            d.0.join("config.toml"),
            "[net]\ninterface = \"tun9\"\n\n[backend]\ngrant_command = \"acme --robot {robot}\"\n",
        )
        .unwrap();
        remember_backend(
            "https://fleet.acme.com/api",
            "https://fleet.acme.com/cli-login",
        )
        .unwrap();
        let c = load().unwrap();
        assert_eq!(c.net.interface, "tun9");
        assert_eq!(
            c.backend.grant_command.as_deref(),
            Some("acme --robot {robot}")
        );
        assert_eq!(c.backend.url.as_deref(), Some("https://fleet.acme.com/api"));
        assert_eq!(
            c.backend.login_url.as_deref(),
            Some("https://fleet.acme.com/cli-login")
        );
    }

    #[test]
    fn no_config_file_is_an_empty_config_not_an_error() {
        let _g = ENV.lock().unwrap();
        let d = scratch();
        std::env::set_var("FJARR_CONFIG_DIR", &d.0);
        let c = load().unwrap();
        assert_eq!(c.backend.url, None);
        assert_eq!(c.net.interface, "fjarr0");
    }
}
